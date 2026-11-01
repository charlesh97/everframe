#include "wifi_prov.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "lwip/inet.h"

#include "dns_server.h"
#include "display_provisioning.h"

#define EVERFRAME_NAMESPACE "everframe"

#define LED_GPIO GPIO_NUM_21
#define LED_ON_LEVEL 0
#define LED_OFF_LEVEL 1

#define FORM_BODY_MAX_LEN 600
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_SCAN_MAX_RECORDS 30

static const char *TAG = "wifi_prov";

/*
 * The setup form is intentionally embedded directly in the firmware. This
 * avoids filesystem and binary-embedding machinery for this first Wi-Fi-only
 * iteration.
 *
 * The portal deliberately collects only Wi-Fi credentials. Device ownership
 * and image routing are established after the frame is online; no cloud
 * credentials belong in the portal or in NVS.
 */
static const char SETUP_HTML[] =
    "<!doctype html>"
    "<html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>EverFrame Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#f4f3ef;color:#24231f;"
    "margin:0;padding:24px}"
    ".card{max-width:460px;margin:24px auto;background:white;padding:28px;"
    "border-radius:16px;box-shadow:0 8px 28px #0002}"
    "h1{margin:0 0 8px}p{color:#666;margin:0 0 24px}"
    "label{display:block;font-weight:600;margin:16px 0 6px}"
    "input,select{box-sizing:border-box;width:100%;padding:12px;border:1px solid #bbb;"
    "border-radius:8px;font-size:16px}"
    "button{width:100%;margin-top:24px;padding:13px;border:0;border-radius:8px;"
    "background:#24231f;color:white;font-size:16px;font-weight:700}"
    "button.secondary{margin-top:8px;background:#e9e7e0;color:#24231f}"
    "small{display:block;color:#777;margin-top:5px}"
    "</style></head><body><main class=\"card\">"
    "<h1>EverFrame Setup</h1>"
    "<p>Connect this frame to your home Wi-Fi.</p>"
    "<form method=\"POST\" action=\"/config\">"
    "<label for=\"network_list\">Nearby networks</label>"
    "<select id=\"network_list\" disabled><option>Scanning...</option></select>"
    "<button id=\"rescan\" class=\"secondary\" type=\"button\">Rescan</button>"
    "<small id=\"scan_status\" aria-live=\"polite\">Looking for Wi-Fi networks...</small>"
    "<label for=\"wifi_ssid\">Network name</label>"
    "<input id=\"wifi_ssid\" name=\"wifi_ssid\" type=\"text\" maxlength=\"32\" required>"
    "<small>Select a network above or enter a hidden network manually.</small>"
    "<label for=\"wifi_pass\">Wi-Fi password</label>"
    "<input id=\"wifi_pass\" name=\"wifi_pass\" type=\"password\" maxlength=\"64\" required>"
    "<small>Your password stays on this frame.</small>"
    "<button type=\"submit\">Save Wi-Fi</button>"
    "</form></main><script>"
    "const list=document.getElementById('network_list');"
    "const ssid=document.getElementById('wifi_ssid');"
    "const status=document.getElementById('scan_status');"
    "async function scan(){"
    "list.disabled=true;list.innerHTML='<option>Scanning...</option>';"
    "status.textContent='Looking for Wi-Fi networks...';"
    "try{const response=await fetch('/networks',{cache:'no-store'});"
    "if(!response.ok)throw new Error('scan failed');"
    "const networks=await response.json();list.innerHTML='';"
    "const prompt=document.createElement('option');prompt.value='';"
    "prompt.textContent=networks.length?'Choose a network':'No networks found';"
    "list.appendChild(prompt);"
    "for(const network of networks){const option=document.createElement('option');"
    "option.value=network.ssid;option.disabled=!network.secure;"
    "option.textContent=network.ssid+' ('+network.rssi+' dBm, '+"
    "(network.secure?'secured':'open - unsupported')+')';list.appendChild(option);}"
    "list.disabled=false;status.textContent=networks.length?"
    "networks.length+' network'+(networks.length===1?'':'s')+' found.':"
    "'No networks found. Enter the network name manually.';"
    "}catch(error){list.innerHTML='<option>Scan unavailable</option>';"
    "status.textContent='Could not scan. Enter the network name manually.';}"
    "}"
    "list.addEventListener('change',()=>{if(list.value)ssid.value=list.value;});"
    "document.getElementById('rescan').addEventListener('click',scan);scan();"
    "</script></body></html>";

static const char SAVED_HTML[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" "
    "content=\"width=device-width,initial-scale=1\">"
    "<title>EverFrame Setup</title></head>"
    "<body style=\"font-family:system-ui,sans-serif;text-align:center;padding:48px\">"
    "<h1>Wi-Fi saved!</h1>"
    "<p>Restarting EverFrame and connecting to Wi-Fi...</p>"
    "</body></html>";

/** Return the numeric value of one hexadecimal character, or -1. */
static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

/**
 * Decode one application/x-www-form-urlencoded value.
 *
 * Browsers encode spaces as '+' and other special bytes as "%HH". Rejecting
 * malformed or oversized data keeps parsing predictable and ensures every
 * value written to NVS is a normal, terminated C string.
 */
static esp_err_t url_decode(const char *encoded, size_t encoded_len,
                            char *decoded, size_t decoded_size)
{
    size_t output_index = 0;

    for (size_t input_index = 0; input_index < encoded_len; input_index++) {
        unsigned char value = encoded[input_index];

        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (input_index + 2 >= encoded_len) {
                return ESP_ERR_INVALID_ARG;
            }

            int high = hex_value(encoded[input_index + 1]);
            int low = hex_value(encoded[input_index + 2]);
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }

            value = (unsigned char)((high << 4) | low);
            input_index += 2;
        }

        /* Embedded NUL bytes would make validation disagree with NVS. */
        if (value == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        if (output_index + 1 >= decoded_size) {
            return ESP_ERR_INVALID_SIZE;
        }

        decoded[output_index++] = (char)value;
    }

    decoded[output_index] = '\0';
    return ESP_OK;
}

/**
 * Find and decode a named field in a form-urlencoded request body.
 *
 * The request is a sequence of key=value pairs separated by '&'. The field
 * names in this form contain only ASCII letters and underscores, so only the
 * values need URL decoding.
 */
static esp_err_t get_form_value(const char *body, const char *wanted_key,
                                char *value, size_t value_size)
{
    const char *pair = body;
    size_t wanted_key_len = strlen(wanted_key);

    value[0] = '\0';
    while (*pair != '\0') {
        const char *pair_end = strchr(pair, '&');
        if (pair_end == NULL) {
            pair_end = pair + strlen(pair);
        }

        const char *equals = memchr(pair, '=', pair_end - pair);
        if (equals != NULL && (size_t)(equals - pair) == wanted_key_len &&
            memcmp(pair, wanted_key, wanted_key_len) == 0) {
            return url_decode(equals + 1, pair_end - equals - 1,
                              value, value_size);
        }

        if (*pair_end == '\0') {
            break;
        }
        pair = pair_end + 1;
    }

    return ESP_ERR_NOT_FOUND;
}

/** Save Wi-Fi credentials and the provisioning-complete flag atomically. */
static esp_err_t save_configuration(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (error != ESP_OK) {
        return error;
    }

    /*
     * prov_done is written last. nvs_commit() makes the complete group durable,
     * so app_main never chooses station mode with only partial credentials.
     */
    error = nvs_set_str(nvs_handle, "wifi_ssid", ssid);
    if (error == ESP_OK) {
        error = nvs_set_str(nvs_handle, "wifi_pass", password);
    }
    if (error == ESP_OK) {
        error = nvs_set_u8(nvs_handle, "prov_done", 1);
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return error;
}

/** Serve the setup form at the captive portal's root URL. */
static esp_err_t root_get_handler(httpd_req_t *request)
{
    ESP_LOGI(TAG, "Serving setup form");
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

/** Sort scan results strongest-first before duplicate SSIDs are removed. */
static int compare_scan_records(const void *left, const void *right)
{
    const wifi_ap_record_t *left_record = left;
    const wifi_ap_record_t *right_record = right;
    return (int)right_record->rssi - (int)left_record->rssi;
}

/** Encode an SSID safely inside a JSON string. */
static esp_err_t json_escape_ssid(const uint8_t *ssid, char *escaped,
                                  size_t escaped_size)
{
    size_t output = 0;
    size_t length = strnlen((const char *)ssid, WIFI_SSID_MAX_LEN);
    for (size_t index = 0; index < length; ++index) {
        uint8_t value = ssid[index];
        const char *replacement = NULL;
        if (value == '"') {
            replacement = "\\\"";
        } else if (value == '\\') {
            replacement = "\\\\";
        }

        if (replacement != NULL) {
            if (output + 2U >= escaped_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            escaped[output++] = replacement[0];
            escaped[output++] = replacement[1];
        } else if (value < 0x20U) {
            if (output + 6U >= escaped_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            int written = snprintf(escaped + output, escaped_size - output,
                                   "\\u%04x", value);
            if (written != 6) {
                return ESP_ERR_INVALID_SIZE;
            }
            output += 6U;
        } else {
            if (output + 1U >= escaped_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            escaped[output++] = (char)value;
        }
    }
    escaped[output] = '\0';
    return ESP_OK;
}

/** Scan nearby access points and return a compact, strongest-first JSON list. */
static esp_err_t networks_get_handler(httpd_req_t *request)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 40,
            .max = 120,
        },
    };
    ESP_LOGI(TAG, "Scanning for nearby Wi-Fi networks");
    esp_err_t error = esp_wifi_scan_start(&scan_config, true);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(error));
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_send(request, "{\"error\":\"scan unavailable\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    uint16_t available = 0;
    error = esp_wifi_scan_get_ap_num(&available);
    if (error != ESP_OK) {
        httpd_resp_send_500(request);
        return error;
    }
    uint16_t count = available > WIFI_SCAN_MAX_RECORDS ?
        WIFI_SCAN_MAX_RECORDS : available;
    wifi_ap_record_t *records =
        count == 0 ? NULL : calloc(count, sizeof(*records));
    if (count > 0 && records == NULL) {
        httpd_resp_send_500(request);
        return ESP_ERR_NO_MEM;
    }
    if (count > 0) {
        error = esp_wifi_scan_get_ap_records(&count, records);
        if (error != ESP_OK) {
            free(records);
            httpd_resp_send_500(request);
            return error;
        }
        qsort(records, count, sizeof(*records), compare_scan_records);
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    error = httpd_resp_send_chunk(request, "[", 1);
    unsigned unique_count = 0;
    for (uint16_t index = 0; error == ESP_OK && index < count; ++index) {
        if (records[index].ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (uint16_t prior = 0; prior < index; ++prior) {
            if (strncmp((const char *)records[index].ssid,
                        (const char *)records[prior].ssid,
                        WIFI_SSID_MAX_LEN) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        char escaped[WIFI_SSID_MAX_LEN * 6U + 1U];
        error = json_escape_ssid(records[index].ssid, escaped, sizeof(escaped));
        if (error != ESP_OK) {
            break;
        }
        char item[sizeof(escaped) + 80U];
        int written = snprintf(item, sizeof(item),
                               "%s{\"ssid\":\"%s\",\"rssi\":%d,"
                               "\"secure\":%s}",
                               unique_count > 0 ? "," : "", escaped,
                               (int)records[index].rssi,
                               records[index].authmode == WIFI_AUTH_OPEN ?
                                   "false" : "true");
        if (written < 0 || (size_t)written >= sizeof(item)) {
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        error = httpd_resp_send_chunk(request, item, written);
        if (error == ESP_OK) {
            ++unique_count;
        }
    }
    if (error == ESP_OK) {
        error = httpd_resp_send_chunk(request, "]", 1);
    }
    if (error == ESP_OK) {
        error = httpd_resp_send_chunk(request, NULL, 0);
    }
    ESP_LOGI(TAG, "Wi-Fi scan returned %u unique network(s)", unique_count);
    free(records);
    return error;
}

/** Read, validate, and save a submitted setup form. */
static esp_err_t config_post_handler(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > FORM_BODY_MAX_LEN) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Form body is empty or too large");
        return ESP_FAIL;
    }

    char *body = malloc(request->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_500(request);
        return ESP_ERR_NO_MEM;
    }

    /* httpd_req_recv() may return a partial body, so keep reading to the end. */
    size_t received_total = 0;
    while (received_total < request->content_len) {
        int received = httpd_req_recv(request, body + received_total,
                                      request->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received_total += received;
    }
    body[received_total] = '\0';

    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASSWORD_MAX_LEN + 1];

    esp_err_t ssid_error = get_form_value(body, "wifi_ssid", ssid,
                                          sizeof(ssid));
    esp_err_t password_error = get_form_value(body, "wifi_pass", password,
                                              sizeof(password));
    free(body);

    /* Required values must be present, validly encoded, and non-empty. */
    if (ssid_error != ESP_OK || password_error != ESP_OK ||
        ssid[0] == '\0' || password[0] == '\0') {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Wi-Fi SSID and password are required");
        return ESP_FAIL;
    }

    esp_err_t error = save_configuration(ssid, password);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save configuration: %s",
                 esp_err_to_name(error));
        httpd_resp_send_500(request);
        return error;
    }

    /* Never log the submitted password. */
    ESP_LOGI(TAG, "Configuration saved for Wi-Fi SSID '%s'", ssid);
    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    /* Give the browser time to receive the confirmation page before rebooting. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/**
 * Redirect every unknown captive-portal probe to the setup form.
 *
 * iOS requires a response body to recognize a captive portal; a bare redirect
 * is not sufficient. This matches the ESP-IDF captive_portal example.
 */
static esp_err_t http_404_error_handler(httpd_req_t *request,
                                        httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    httpd_resp_send(request, "Redirect to the captive portal",
                    HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting captive-portal probe to root");
    return ESP_OK;
}

/** Start the port-80 web server and register its two routes. */
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting setup web server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start setup web server");
        return NULL;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    const httpd_uri_t networks_uri = {
        .uri = "/networks",
        .method = HTTP_GET,
        .handler = networks_get_handler,
    };

    if (httpd_register_uri_handler(server, &root_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &networks_uri) != ESP_OK ||
        httpd_register_uri_handler(server, &config_uri) != ESP_OK ||
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                   http_404_error_handler) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HTTP handlers");
        httpd_stop(server);
        return NULL;
    }

    return server;
}

/** Log phones/laptops joining and leaving the temporary setup network. */
static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "Client " MACSTR " joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "Client " MACSTR " left, AID=%d reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

/** Build the unique SoftAP name from the final two bytes of the base MAC. */
static esp_err_t make_softap_ssid(char *ssid, size_t ssid_size)
{
    uint8_t mac[6];
    esp_err_t error = esp_read_mac(mac, ESP_MAC_BASE);
    if (error != ESP_OK) {
        return error;
    }

    int written = snprintf(ssid, ssid_size, "EverFrame-%02X%02X",
                           mac[4], mac[5]);
    return written > 0 && (size_t)written < ssid_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

esp_err_t wifi_prov_start(void)
{
    char softap_ssid[sizeof("EverFrame-FFFF")];
    esp_err_t error = make_softap_ssid(softap_ssid, sizeof(softap_ssid));
    if (error != ESP_OK) {
        return error;
    }

    /* Create the default AP network interface (192.168.4.1 with DHCP). */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
    if (ap_netif == NULL || station_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                       wifi_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t wifi_config = { 0 };
    memcpy(wifi_config.ap.ssid, softap_ssid, strlen(softap_ssid));
    wifi_config.ap.ssid_len = strlen(softap_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        return error;
    }

    esp_netif_ip_info_t ip_info;
    error = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (error != ESP_OK) {
        return error;
    }

    char ip_address[16];
    inet_ntoa_r(ip_info.ip.addr, ip_address, sizeof(ip_address));
    ESP_LOGI(TAG, "Provisioning SoftAP ready");
    ESP_LOGI(TAG, "Join open network: %s", softap_ssid);
    ESP_LOGI(TAG, "Captive portal IP: %s", ip_address);

    /* Unknown paths are expected during captive-portal detection. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        return ESP_FAIL;
    }

    /* Resolve every hostname to the SoftAP so probe URLs reach port 80 here. */
    dns_server_config_t dns_config =
        DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server_handle_t dns_handle = start_dns_server(&dns_config);
    if (dns_handle == NULL) {
        httpd_stop(server);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Captive portal is ready at http://%s/", ip_address);

    error = display_show_qr_ap(softap_ssid);
    if (error != ESP_OK) {
        /* Keep the web setup usable even if the panel is disconnected. */
        ESP_LOGE(TAG, "Could not render provisioning QR: %s",
                 esp_err_to_name(error));
    }

    /*
     * GPIO21 drives the XIAO ESP32S3's onboard LED active-low. Blinking here
     * clearly distinguishes setup mode while the HTTP and DNS tasks run.
     */
    bool led_is_on = false;
    while (true) {
        led_is_on = !led_is_on;
        gpio_set_level(LED_GPIO, led_is_on ? LED_ON_LEVEL : LED_OFF_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
