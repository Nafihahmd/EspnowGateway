// espnow_gateway/main.c
// Gateway using ESPNOW <-> USB Serial/JTAG (usb_serial_jtag driver) for Node-RED
// Target: ESP32-C6 (Seeed XIAO). Uses built-in USB Serial/JTAG driver (no tinyusb).

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_crc.h"
#include "driver/usb_serial_jtag.h"
#include "cJSON.h"

// #include "driver/usb_serial_jtag.h"   // USB Serial/JTAG driver API (install/read/write)
#include "espnow_example.h"

static const char *TAG = "espnow_gateway_usbjtag";

#define USB_LINE_MAX    1024
#define USB_QUEUE_LEN   8

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint8_t s_my_mac[6];

static QueueHandle_t s_usb_line_q = NULL;

/* helper: mac string */
static void mac_to_str(const uint8_t *mac, char *out, size_t len) {
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void mac_from_str(const char *s, uint8_t *mac) {
    unsigned int b[6] = {0};
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) == 6) {
        for (int i=0;i<6;i++) mac[i] = (uint8_t)b[i];
    } else {
        memset(mac, 0, 6);
    }
}

/* prepare espnow packet (same layout as client) */
static int prepare_espnow_json(uint8_t *buf, size_t buf_len, const char *json, const uint8_t *dest_mac) {
    size_t jlen = strlen(json);
    size_t total_len = sizeof(espnow_data_t) + jlen;
    if (total_len > buf_len) return -1;
    espnow_data_t *hdr = (espnow_data_t *)buf;
    hdr->type = (memcmp(dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0) ? EXAMPLE_ESPNOW_DATA_BROADCAST : EXAMPLE_ESPNOW_DATA_UNICAST;
    hdr->state = 0;
    hdr->seq_num = 0;
    hdr->crc = 0;
    hdr->magic = esp_random();
    memcpy(hdr->payload, json, jlen);
    hdr->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)hdr, total_len);
    return (int)total_len;
}

/* add peer if missing and send */
static esp_err_t ensure_peer_and_send(const uint8_t *mac, const char *json) {
    if (memcmp(mac, s_broadcast_mac, ESP_NOW_ETH_ALEN) != 0) {
        if (!esp_now_is_peer_exist(mac)) {
            esp_now_peer_info_t peer = {0};
            memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
            peer.channel = CONFIG_ESPNOW_CHANNEL;
            peer.ifidx = ESPNOW_WIFI_IF;
            peer.encrypt = false;
            esp_err_t r = esp_now_add_peer(&peer);
            if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
                ESP_LOGW(TAG, "esp_now_add_peer failed: %d", r);
            }
        }
    }
    const size_t BUF_SZ = 1024;
    uint8_t *buf = malloc(BUF_SZ);
    if (!buf) return ESP_ERR_NO_MEM;
    int total = prepare_espnow_json(buf, BUF_SZ, json, mac);
    if (total < 0) { free(buf); return ESP_ERR_INVALID_ARG; }
    esp_err_t res = esp_now_send(mac, buf, total);
    free(buf);
    return res;
}

/* ESPNOW receive callback: forward JSON payload to host over USB Serial/JTAG */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (!data || len <= (int)sizeof(espnow_data_t)) return;
    espnow_data_t *hdr = (espnow_data_t *)data;
    int json_len = len - sizeof(espnow_data_t);
    char *json = malloc(json_len + 1);
    if (!json) return;
    memcpy(json, hdr->payload, json_len);
    json[json_len] = 0;

    ESP_LOGI(TAG, "ESPNOW recv from "MACSTR": %s", MAC2STR(recv_info->src_addr), json);

    // write to host via usb_serial_jtag
    if (usb_serial_jtag_is_connected()) {
        printf("%s\n",(const char*)json);
        // usb_serial_jtag_write_bytes("\n", 1);
        // flush: wait short time for TX to finish
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
    }

    // if client sent registration, reply with gateway_info (unicast)
    cJSON *root = cJSON_Parse(json);
    if (root) {
        cJSON *type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "register") == 0) {
            char mymac[18]; mac_to_str(s_my_mac, mymac, sizeof(mymac));
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", "gateway_info");
            cJSON *pl = cJSON_CreateObject();
            cJSON_AddStringToObject(pl, "mac", mymac);
            cJSON_AddItemToObject(o, "payload", pl);
            char *s = cJSON_PrintUnformatted(o);
            ensure_peer_and_send(recv_info->src_addr, s);
            cJSON_free(s);
            cJSON_Delete(o);
            ESP_LOGI(TAG, "Replied gateway_info to "MACSTR, MAC2STR(recv_info->src_addr));
        }
        cJSON_Delete(root);
    }

    free(json);
}

/* USB Serial/JTAG line assembler task.
   Reads raw bytes from usb_serial_jtag_read_bytes and splits into newline-terminated lines.
   Puts malloc'd line pointers into s_usb_line_q for processing by usb_line_task.
*/
static void usb_reader_task(void *arg) {
    uint8_t buf[256];
    char line[USB_LINE_MAX];
    size_t idx = 0;

    while (1) {
        if (!usb_serial_jtag_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int r = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(500));
        if (r > 0) {
            for (int i = 0; i < r; ++i) {
                char c = (char)buf[i];
                if (c == '\n' || c == '\r') {
                    if (idx == 0) continue;
                    line[idx] = 0;
                    char *copy = strdup(line);
                    if (copy) {
                        if (xQueueSend(s_usb_line_q, &copy, pdMS_TO_TICKS(10)) != pdTRUE) {
                            free(copy);
                        }
                    }
                    idx = 0;
                } else {
                    if (idx < (USB_LINE_MAX - 1)) line[idx++] = c;
                    else idx = 0; // overflow, drop line
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* usb_line_task: processes completed JSON lines from Node-RED */
static void usb_line_task(void *arg) {
    char *line = NULL;
    while (1) {
        if (xQueueReceive(s_usb_line_q, &line, portMAX_DELAY) == pdTRUE && line != NULL) {
            ESP_LOGI(TAG, "USB RX: %s", line);
            cJSON *root = cJSON_Parse(line);
            if (root) {
                cJSON *macj = cJSON_GetObjectItem(root, "mac");
                cJSON *cmd  = cJSON_GetObjectItem(root, "cmd");
                if (cJSON_IsString(macj) && cJSON_IsString(cmd)) {
                    uint8_t target[6];
                    mac_from_str(macj->valuestring, target);
                    if (memcmp(target, "\0\0\0\0\0\0", 6) == 0) {
                        ESP_LOGW(TAG, "Invalid target MAC from Node-RED");
                    } else {
                        if (strcmp(cmd->valuestring, "query_config") == 0) {
                            cJSON *o = cJSON_CreateObject();
                            cJSON_AddStringToObject(o, "type", "config_request");
                            char *s = cJSON_PrintUnformatted(o);
                            ensure_peer_and_send(target, s);
                            cJSON_free(s);
                            cJSON_Delete(o);
                        } else if (strcmp(cmd->valuestring, "set_config") == 0) {
                            cJSON *pl = cJSON_GetObjectItem(root, "payload");
                            if (pl) {
                                cJSON *o = cJSON_CreateObject();
                                cJSON_AddStringToObject(o, "type", "set_config");
                                cJSON_AddItemToObject(o, "payload", cJSON_Duplicate(pl, 1));
                                char *s = cJSON_PrintUnformatted(o);
                                ensure_peer_and_send(target, s);
                                cJSON_free(s);
                                cJSON_Delete(o);
                            }
                        } else if (strcmp(cmd->valuestring, "unicast_json") == 0) {
                            cJSON *pl = cJSON_GetObjectItem(root, "payload");
                            if (pl) {
                                char *s = cJSON_PrintUnformatted(pl);
                                ensure_peer_and_send(target, s);
                                cJSON_free(s);
                            }
                        } else {
                            ESP_LOGW(TAG, "Unknown cmd from Node-RED: %s", cmd->valuestring);
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "Invalid command JSON from Node-RED");
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "Failed to parse JSON from Node-RED");
            }
            free(line);
            line = NULL;
        }
    }
}

/* wifi & espnow init */
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
#if CONFIG_ESPNOW_WIFI_MODE_STATION
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

void app_main(void) {
    ESP_LOGI(TAG, "Gateway (USB Serial/JTAG) starting...");

    ESP_ERROR_CHECK(nvs_flash_init());

    // get gateway MAC (for replies)
    esp_efuse_mac_get_default(s_my_mac);
    char mymac[18]; mac_to_str(s_my_mac, mymac, sizeof(mymac));
    ESP_LOGI(TAG, "Gateway MAC: %s", mymac);

    // create queue for incoming USB lines
    s_usb_line_q = xQueueCreate(USB_QUEUE_LEN, sizeof(char *));
    if (!s_usb_line_q) {
        ESP_LOGE(TAG, "failed to create usb line queue");
        return;
    }

    // init wifi & esp-now
    wifi_init();
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));

    // add broadcast peer so we can receive client discovery broadcasts
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    peer.channel = CONFIG_ESPNOW_CHANNEL;
    peer.ifidx = ESPNOW_WIFI_IF;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    // install usb_serial_jtag driver
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 4096
    };
    ESP_ERROR_CHECK( usb_serial_jtag_driver_install(&usb_cfg) );

    // start reader and processor tasks
    xTaskCreate(usb_reader_task, "usb_reader", 4096, NULL, 5, NULL);
    xTaskCreate(usb_line_task, "usb_line", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Gateway ready. USB Serial/JTAG should enumerate on host.");
}
