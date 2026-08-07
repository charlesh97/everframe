/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Adapted from the ESP-IDF v6.0.2 captive_portal example.
 */

#pragma once

#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DNS_SERVER_MAX_ITEMS
#define DNS_SERVER_MAX_ITEMS 1
#endif

/* Convenience configuration for the common captive-portal case: one rule. */
#define DNS_SERVER_CONFIG_SINGLE(queried_name, netif_key)  {        \
        .num_of_entries = 1,                                        \
        .item = { { .name = queried_name, .if_key = netif_key } }   \
        }

/**
 * @brief Definition of one DNS entry: name to IP address (or network interface).
 *
 * @note Use string literals, or otherwise ensure that the strings remain valid
 * for the lifetime of the DNS server. The server copies the pointers, not the
 * strings themselves.
 */
typedef struct dns_entry_pair {
    const char *name;       /**<! Exact DNS name to answer, or "*" for every name. */
    const char *if_key;     /**<! Network interface whose IPv4 address is returned. */
    esp_ip4_addr_t ip;      /**<! Static IPv4 address used when if_key is NULL. */
} dns_entry_pair_t;

/**
 * @brief Rules used when answering DNS A queries.
 *
 * @note To define more rules, set DNS_SERVER_MAX_ITEMS before including this
 * header. EverFrame uses one wildcard rule so every hostname resolves to the
 * SoftAP and captive-portal checks always reach the setup web server.
 */
typedef struct dns_server_config {
    int num_of_entries;                          /**<! Number of configured rules. */
    dns_entry_pair_t item[DNS_SERVER_MAX_ITEMS]; /**<! DNS name/address rules. */
} dns_server_config_t;

/** @brief Opaque handle for a running DNS server. */
typedef struct dns_server_handle *dns_server_handle_t;

/**
 * @brief Start a small DNS server that answers IPv4 A queries.
 *
 * Each query is matched against the configured rules. A matching rule returns
 * either the current IPv4 address of a network interface or a static address.
 *
 * @param config DNS matching and response rules.
 * @return Server handle on success, or NULL if allocation/task creation fails.
 */
dns_server_handle_t start_dns_server(dns_server_config_t *config);

/**
 * @brief Stop the DNS task and release its state.
 *
 * @param handle Server handle returned by start_dns_server().
 */
void stop_dns_server(dns_server_handle_t handle);

#ifdef __cplusplus
}
#endif
