#ifndef ESPNOW_EXAMPLE_H
#define ESPNOW_EXAMPLE_H

#include <string.h>
#include <stdbool.h>
#include "esp_now.h"
#include "esp_err.h"
#include "esp_types.h"

/*
 * NOTE:
 * - The example projects use a `s_broadcast_mac` array defined in their C files:
 *     static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,...};
 *   The macro IS_BROADCAST_ADDR uses that symbol. If you prefer, you can
 *   replace that macro to compare against a literal broadcast address.
 */

/* ESPNOW can work in both station and softap mode. Configure in menuconfig:
   set CONFIG_ESPNOW_WIFI_MODE_STATION for station mode or clear for softap. */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

/* queue size for events posted from ESPNOW callbacks into the espnow task */
#define ESPNOW_QUEUE_SIZE           6

/* Broadcast comparison macro - uses s_broadcast_mac defined in the C file */
#define IS_BROADCAST_ADDR(addr) (memcmp((addr), s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

/* IDs for events posted to the espnow task */
typedef enum {
    EXAMPLE_ESPNOW_SEND_CB,
    EXAMPLE_ESPNOW_RECV_CB,
} espnow_event_id_t;

/* send callback info */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} espnow_event_send_cb_t;

/* receive callback info */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
} espnow_event_recv_cb_t;

/* union of event info payloads */
typedef union {
    espnow_event_send_cb_t send_cb;
    espnow_event_recv_cb_t recv_cb;
} espnow_event_info_t;

/* full event struct posted to the espnow task queue */
typedef struct {
    espnow_event_id_t id;
    espnow_event_info_t info;
} espnow_event_t;

/* Types of ESPNOW user data (broadcast/unicast) used by example */
enum {
    EXAMPLE_ESPNOW_DATA_BROADCAST,
    EXAMPLE_ESPNOW_DATA_UNICAST,
    EXAMPLE_ESPNOW_DATA_MAX,
};

/* User-defined ESPNOW packet layout.
 * The payload is variable-length and starts immediately after this header.
 */
typedef struct {
    uint8_t type;                         // Broadcast or unicast marker (EXAMPLE_ESPNOW_DATA_*)
    uint8_t state;                        // state flag (user-defined)
    uint16_t seq_num;                     // sequence number
    uint16_t crc;                         // CRC16 of the whole packet (crc field set to 0 while computing)
    uint32_t magic;                       // magic number (user-defined)
    uint8_t payload[0];                   // flexible array member -> actual JSON payload bytes follow
} __attribute__((packed)) espnow_data_t;

/* Parameters used by the example send task to manage sending */
typedef struct {
    bool unicast;                         // send unicast
    bool broadcast;                       // send broadcast
    uint8_t state;                        // state to fill into espnow_data_t::state
    uint32_t magic;                       // magic number to fill into espnow_data_t::magic
    uint16_t count;                       // how many unicast packets to send (example)
    uint16_t delay;                       // delay between sends (ms)
    int len;                              // total length of ESPNOW packet (bytes)
    uint8_t *buffer;                      // pointer to allocated buffer holding packet data
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];   // destination MAC for unicast
} espnow_send_param_t;

#endif // ESPNOW_EXAMPLE_H
