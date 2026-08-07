/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Adapted from the ESP-IDF v6.0.2 captive_portal example. This is deliberately
 * a tiny DNS responder: it only needs to answer IPv4 A queries with the SoftAP
 * address so phones and laptops discover the local setup page.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "dns_server.h"

#define DNS_PORT (53)
#define DNS_MAX_LEN (256)

#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

static const char *TAG = "dns_server";

/* Header at the beginning of every DNS packet. */
typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

/* Fixed fields following the encoded name in a DNS question. */
typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

/* IPv4 answer appended for each A question that matches a rule. */
typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

/* Private state owned by the DNS task. */
struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

/**
 * Convert a DNS label-encoded name into a normal dot-separated string.
 *
 * For example, "\x07example\x03com\x00" becomes "example.com". The return
 * value points to the byte immediately following the encoded name so the
 * caller can read the question type and class.
 */
static char *parse_dns_name(char *raw_name, char *parsed_name,
                            size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_iterator = parsed_name;
    int name_len = 0;

    do {
        int sub_name_len = *label;

        /* Add one because a dot is inserted after every label. */
        name_len += sub_name_len + 1;
        if (name_len > parsed_name_max_len) {
            return NULL;
        }

        memcpy(name_iterator, label + 1, sub_name_len);
        name_iterator[sub_name_len] = '.';
        name_iterator += sub_name_len + 1;
        label += sub_name_len + 1;
    } while (*label != 0);

    /* Replace the final dot with the string terminator. */
    parsed_name[name_len - 1] = '\0';
    return label + 1;
}

/**
 * Parse a DNS request and prepare a response containing the SoftAP address.
 *
 * The original question is copied into the response. For every matching A
 * question, an answer is appended using DNS name compression to refer back to
 * the name in the question section.
 */
static int parse_dns_request(char *request, size_t request_len, char *reply,
                             size_t reply_max_len, dns_server_handle_t handle)
{
    if (request_len > reply_max_len) {
        return -1;
    }

    memset(reply, 0, reply_max_len);
    memcpy(reply, request, request_len);

    /* DNS packets use network byte order, unlike the ESP32's native order. */
    dns_header_t *header = (dns_header_t *)reply;
    ESP_LOGD(TAG, "DNS query id=0x%X flags=0x%X questions=%d",
             ntohs(header->id), ntohs(header->flags), ntohs(header->qd_count));

    /* Ignore requests that are not standard DNS queries. */
    if ((header->flags & OPCODE_MASK) != 0) {
        return 0;
    }

    /* Mark the packet as a response and provide one answer per question. */
    header->flags |= QR_FLAG;
    uint16_t question_count = ntohs(header->qd_count);
    header->an_count = htons(question_count);

    int reply_len = question_count * sizeof(dns_answer_t) + request_len;
    if (reply_len > reply_max_len) {
        return -1;
    }

    char *answer_ptr = reply + request_len;
    char *question_ptr = reply + sizeof(dns_header_t);
    char name[128];

    for (int question_index = 0; question_index < question_count;
         question_index++) {
        char *name_end = parse_dns_name(question_ptr, name, sizeof(name));
        if (name_end == NULL) {
            ESP_LOGE(TAG, "Failed to parse DNS question");
            return -1;
        }

        dns_question_t *question = (dns_question_t *)name_end;
        uint16_t question_type = ntohs(question->type);
        uint16_t question_class = ntohs(question->class);

        ESP_LOGD(TAG, "Question type=%d class=%d name=%s",
                 question_type, question_class, name);

        if (question_type == QD_TYPE_A) {
            esp_ip4_addr_t ip = { .addr = IPADDR_ANY };

            /* Find the first exact or wildcard rule that matches this name. */
            for (int entry_index = 0; entry_index < handle->num_of_entries;
                 entry_index++) {
                dns_entry_pair_t *entry = &handle->entry[entry_index];
                if (strcmp(entry->name, "*") != 0 &&
                    strcmp(entry->name, name) != 0) {
                    continue;
                }

                if (entry->if_key != NULL) {
                    esp_netif_t *netif =
                        esp_netif_get_handle_from_ifkey(entry->if_key);
                    esp_netif_ip_info_t ip_info;
                    if (netif != NULL &&
                        esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        ip.addr = ip_info.ip.addr;
                    }
                    break;
                }

                if (entry->ip.addr != IPADDR_ANY) {
                    ip.addr = entry->ip.addr;
                    break;
                }
            }

            /* A query without a matching rule receives no usable answer. */
            if (ip.addr != IPADDR_ANY) {
                dns_answer_t *answer = (dns_answer_t *)answer_ptr;
                answer->ptr_offset =
                    htons(0xC000 | (question_ptr - reply));
                answer->type = htons(question_type);
                answer->class = htons(question_class);
                answer->ttl = htonl(ANS_TTL_SEC);
                answer->addr_len = htons(sizeof(ip.addr));
                answer->ip_addr = ip.addr;

                ESP_LOGD(TAG,
                         "Answer PTR=0x%" PRIX16 " IP=0x%" PRIX32,
                         ntohs(answer->ptr_offset), ip.addr);
            }
        }

        /* Move past this question and the answer reserved for it. */
        question_ptr = name_end + sizeof(dns_question_t);
        answer_ptr += sizeof(dns_answer_t);
    }

    return reply_len;
}

/**
 * Listen on UDP port 53 and answer DNS requests until the server is stopped.
 *
 * Only IPv4 is required because the captive portal maps A queries to the
 * SoftAP's 192.168.4.1 address.
 */
static void dns_server_task(void *parameter)
{
    char receive_buffer[128];
    char address_string[128];
    dns_server_handle_t handle = parameter;

    while (handle->started) {
        struct sockaddr_in destination = {
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
        };

        int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (socket_fd < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }

        int error = bind(socket_fd, (struct sockaddr *)&destination,
                         sizeof(destination));
        if (error < 0) {
            ESP_LOGE(TAG, "Unable to bind UDP port %d: errno %d",
                     DNS_PORT, errno);
            close(socket_fd);
            break;
        }
        ESP_LOGI(TAG, "DNS server listening on UDP port %d", DNS_PORT);

        while (handle->started) {
            struct sockaddr_in6 source_address;
            socklen_t source_length = sizeof(source_address);
            int received = recvfrom(socket_fd, receive_buffer,
                                    sizeof(receive_buffer) - 1, 0,
                                    (struct sockaddr *)&source_address,
                                    &source_length);

            if (received < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }

            if (source_address.sin6_family == PF_INET) {
                inet_ntoa_r(
                    ((struct sockaddr_in *)&source_address)->sin_addr.s_addr,
                    address_string, sizeof(address_string) - 1);
            } else if (source_address.sin6_family == PF_INET6) {
                inet6_ntoa_r(source_address.sin6_addr, address_string,
                             sizeof(address_string) - 1);
            } else {
                strcpy(address_string, "unknown");
            }

            receive_buffer[received] = '\0';
            char reply[DNS_MAX_LEN];
            int reply_len = parse_dns_request(receive_buffer, received, reply,
                                              sizeof(reply), handle);

            ESP_LOGD(TAG, "Received %d bytes from %s; reply length=%d",
                     received, address_string, reply_len);
            if (reply_len <= 0) {
                ESP_LOGE(TAG, "Failed to prepare DNS reply");
                continue;
            }

            int sent = sendto(socket_fd, reply, reply_len, 0,
                              (struct sockaddr *)&source_address,
                              source_length);
            if (sent < 0) {
                ESP_LOGE(TAG, "sendto failed: errno %d", errno);
                break;
            }
        }

        shutdown(socket_fd, 0);
        close(socket_fd);
    }

    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, NULL, TAG, "Missing DNS config");
    ESP_RETURN_ON_FALSE(config->num_of_entries > 0, NULL, TAG,
                        "DNS config has no entries");

    size_t allocation_size = sizeof(struct dns_server_handle) +
                             config->num_of_entries * sizeof(dns_entry_pair_t);
    dns_server_handle_t handle = calloc(1, allocation_size);
    ESP_RETURN_ON_FALSE(handle != NULL, NULL, TAG,
                        "Failed to allocate DNS server handle");

    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item,
           config->num_of_entries * sizeof(dns_entry_pair_t));

    BaseType_t task_created = xTaskCreate(dns_server_task, "dns_server", 4096,
                                          handle, 5, &handle->task);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        free(handle);
        return NULL;
    }

    return handle;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    handle->started = false;
    vTaskDelete(handle->task);
    free(handle);
}
