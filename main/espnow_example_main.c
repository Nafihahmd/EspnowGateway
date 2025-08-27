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
#include "nvs_helper.h"

static const char *TAG = "espnow_gateway";
static QueueHandle_t s_usb_line_q = NULL;

#define USB_LINE_MAX    1024
#define USB_QUEUE_LEN   8

static void mac_from_str(const char *s, uint8_t *mac) {
    unsigned int b[6] = {0};
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) == 6) {
        for (int i=0;i<6;i++) mac[i] = (uint8_t)b[i];
    } else {
        memset(mac, 0, 6);
    }
}

esp_err_t espnow_send_json(const uint8_t *mac_addr, cJSON *json);
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
                cJSON *type  = cJSON_GetObjectItem(root, "type");
                if (cJSON_IsString(macj) && cJSON_IsString(type)) {
                    uint8_t target[6];
                    mac_from_str(macj->valuestring, target);
                    if (memcmp(target, "\0\0\0\0\0\0", 6) == 0) {
                        ESP_LOGW(TAG, "Invalid target MAC from Node-RED");
                    } else {
                        ESP_LOGI(TAG, "Valid MAC");
                        if (strcmp(type->valuestring, "get_config") == 0) {
                            cJSON *o = cJSON_CreateObject();
                            cJSON_AddStringToObject(o, "type", "config_request");
                            // char *s = cJSON_PrintUnformatted(o);
                            espnow_send_json(target, o);
                            // cJSON_free(s);
                            cJSON_Delete(o);
                        } else if (strcmp(type->valuestring, "set_config") == 0) {
                            cJSON *cfg = cJSON_GetObjectItem(root, "configurations");
                        ESP_LOGI(TAG, "Set Config");
                            if (cfg) {
                                cJSON *o = cJSON_CreateObject();
                                cJSON_AddStringToObject(o, "type", "set_config");
                                cJSON_AddItemToObject(o, "configurations", cJSON_Duplicate(cfg, 1));
                                // char *s = cJSON_PrintUnformatted(o);
                                espnow_send_json(target, o);
                                // cJSON_free(s);
                                cJSON_Delete(o);
                            }
                        } else if (strcmp(type->valuestring, "forward") == 0) {
                            cJSON *pl = cJSON_GetObjectItem(root, "payload");
                            if (pl) {
                                // char *s = cJSON_PrintUnformatted(pl);
                                espnow_send_json(target, pl);
                                // cJSON_free(s);
                            }
                        } else {
                            ESP_LOGW(TAG, "Unknown type from Node-RED: %s", type->valuestring);
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
uint8_t s_my_mac[6];
uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint8_t s_gateway_mac[6];
bool gateway_known = false;
static QueueHandle_t s_espnow_queue = NULL;

/* ------------ helpers ------------- */
void mac_to_str(const uint8_t *mac, char *str, size_t len) {
    snprintf(str, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
// bool is_broadcast_mac(const uint8_t *mac) {
//     return memcmp(mac, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0;
// }


/* WiFi should start before using ESPNOW */
void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK(esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR));
#endif
}

/* ESPNOW sending callback function */
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    espnow_event_t evt;
    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (tx_info == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    
    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}


/* ------------- ESPNOW receive callback (from gateway or other) ------------- */
void espnow_register_cmd_handler(const char *json) {
    // Parse JSON for config_request replies or set_config messages
    cJSON *root = cJSON_Parse(json);
    if (root) {
        cJSON *type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "register") == 0) {
                cJSON *mac_addr = cJSON_GetObjectItem(root, "mac");
                if (mac_addr && cJSON_IsString(mac_addr)) {
                    //reply with gateway MAC
                    char mymac[18];
                    uint8_t target[6];
                    mac_from_str(mac_addr->valuestring, target);
                    // Add peer if not exists
                    if (esp_now_is_peer_exist(target) == false) {
                        esp_now_peer_info_t peer;
                        ESP_LOGI(TAG, "Adding peer "MACSTR, MAC2STR(target));
                        memset(&peer, 0, sizeof(esp_now_peer_info_t));
                        peer.channel = CONFIG_ESPNOW_CHANNEL;
                        peer.ifidx = ESPNOW_WIFI_IF;
                        peer.encrypt = true;
                        memcpy(peer.lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                        memcpy(peer.peer_addr, target, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
                        nvs_store_peer_mac(target);
                    }
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, "type", "register_ack");
                    mac_to_str(s_my_mac, mymac, sizeof(mymac));
                    cJSON_AddStringToObject(o, "mac", mymac);
                    ESP_LOGI(TAG, "Registering gateway MAC %s to node "MACSTR, mymac, MAC2STR((uint8_t*)target));
                    espnow_send_json(s_broadcast_mac, o);
                    cJSON_Delete(o);
                }
            } 
            // else if (strcmp(type->valuestring, "set_config")==0) {
            //     cJSON *pl = cJSON_GetObjectItem(root, "payload");
            //     if (pl) {
            //         cJSON *it = pl->child;
            //         while (it) {
            //             if (cJSON_IsNumber(it)) {
            //                 // accept cfg0..cfg4
            //                 if (strncmp(it->string, "cfg", 3) == 0) {
            //                     // parse index
            //                     int idx = atoi(it->string + 3);
            //                     if (idx >= 0 && idx < CFG_COUNT) {
            //                         nvs_set_cfg(idx, it->valueint);
            //                         ESP_LOGI(TAG, "Updated %s = %d", it->string, it->valueint);
            //                     }
            //                 }
            //             }
            //             it = it->next;
            //         }
            //     }
            // }
        }
        cJSON_Delete(root);
    }

    // free(json);
}


/* ESPNOW receiving callback function */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    espnow_event_t evt;
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

    if (recv_info->src_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    evt.id = ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    
    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

/* Parse received ESPNOW data. */
int espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *type)
{
    espnow_data_t *buf = (espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(espnow_data_t)) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    *type = buf->type;
    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc) {
        buf->crc = crc;
        return 0; // Success
    }

    return -1; // CRC error
}

/* Prepare ESPNOW data to be sent. */
void espnow_data_prepare(espnow_send_param_t *send_param, uint8_t *payload, uint16_t payload_len)
{
    espnow_data_t *buf = (espnow_data_t *)send_param->buffer;

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? ESPNOW_DATA_BROADCAST : ESPNOW_DATA_UNICAST;
    buf->crc = 0;
    
    // Copy payload if provided
    if (payload != NULL && payload_len > 0) {
        memcpy(buf->payload, payload, payload_len);
    }
    
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

/* Deinitialize ESPNOW */
static void espnow_deinit(espnow_send_param_t *send_param)
{
    if (send_param) {
        free(send_param->buffer);
        free(send_param);
    }
    
    if (s_espnow_queue) {
        vQueueDelete(s_espnow_queue);
        s_espnow_queue = NULL;
    }
    
    esp_now_deinit();
}

/* ESPNOW task to handle events */
static void espnow_task(void *pvParameter)
{
    espnow_event_t evt;
    uint8_t data_type;
    espnow_send_param_t *send_param = (espnow_send_param_t *)pvParameter;

    while (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case ESPNOW_SEND_CB:
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                ESP_LOGI(TAG, "Send data to "MACSTR", status: %d", 
                         MAC2STR(send_cb->mac_addr), send_cb->status);
                break;
            }
            case ESPNOW_RECV_CB:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                
                if (espnow_data_parse(recv_cb->data, recv_cb->data_len, &data_type) == 0) {
                    espnow_data_t *buf = (espnow_data_t *)recv_cb->data;
                    int payload_len = recv_cb->data_len - sizeof(espnow_data_t);
                    if (data_type == ESPNOW_DATA_BROADCAST) {
                        
                        ESP_LOGI(TAG, "Receive broadcast data from: "MACSTR", len: %d", 
                                 MAC2STR(recv_cb->mac_addr), recv_cb->data_len);
                        if (payload_len > 0) {
                            // Add null terminator to make it a valid C string
                            char *json_str = malloc(payload_len + 1);
                            if (json_str) {
                                memcpy(json_str, buf->payload, payload_len);
                                json_str[payload_len] = '\0';
                                
                                // Parse and print the JSON
                                cJSON *root = cJSON_Parse(json_str);
                                if (root) {
                                    char *printed = cJSON_PrintUnformatted(root);
                                    // ESP_LOGI(TAG, "Received JSON: %s", printed);
                                    // write to host via usb_serial_jtag
                                    if (usb_serial_jtag_is_connected()) {
                                        printf("%s\n",(const char*)printed);
                                        // usb_serial_jtag_write_bytes("\n", 1);
                                        // flush: wait short time for TX to finish
                                        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
                                    }
                                    espnow_register_cmd_handler(printed);
                                    free(printed);
                                    cJSON_Delete(root);
                                } else {
                                    ESP_LOGI(TAG, "Received data (not JSON): %s", json_str);
                                }
                            }
                            free(json_str);
                                
                        }
                        
                    } else if (data_type == ESPNOW_DATA_UNICAST) {                                            
                        // espnow_data_t *buf = (espnow_data_t *)recv_cb->data;
                        // int payload_len = recv_cb->data_len - sizeof(espnow_data_t);
                        ESP_LOGI(TAG, "Receive unicast data from: "MACSTR", len: %d", 
                                 MAC2STR(recv_cb->mac_addr), recv_cb->data_len);
                        
                        if (payload_len > 0) {
                            // Add null terminator to make it a valid C string
                            char *json_str = malloc(payload_len + 1);
                            if (json_str) {
                                memcpy(json_str, buf->payload, payload_len);
                                json_str[payload_len] = '\0';
                                
                                // Parse and print the JSON
                                cJSON *root = cJSON_Parse(json_str);
                                if (root) {
                                    char *printed = cJSON_PrintUnformatted(root);
                                    // ESP_LOGI(TAG, "Received JSON: %s", printed);
                                    // write to host via usb_serial_jtag
                                    if (usb_serial_jtag_is_connected()) {
                                        printf("%s\n",(const char*)printed);
                                        // usb_serial_jtag_write_bytes("\n", 1);
                                        // flush: wait short time for TX to finish
                                        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
                                    }
                                    // espnow_register_cmd_handler(printed);
                                    free(printed);
                                    cJSON_Delete(root);
                                } else {
                                    ESP_LOGI(TAG, "Received data (not JSON): %s", json_str);
                                }
                            }
                            free(json_str);
                                
                        }
                    }
                } else {
                    ESP_LOGI(TAG, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                
                free(recv_cb->data);
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
        }
    }
}

/* API to send JSON data */
esp_err_t espnow_send_json(const uint8_t *mac_addr, cJSON *json)
{
    ESP_LOGI(TAG, "espnow_send_json");
    if (!json) {
        ESP_LOGE(TAG, "Invalid JSON object");
        return ESP_FAIL;
    }
    
    // Convert JSON to string
    char *json_str = cJSON_PrintUnformatted(json);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print JSON");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Sending JSON: %s", json_str);
    
    size_t json_len = strlen(json_str);
    size_t total_len = sizeof(espnow_data_t) + json_len;
    
    // Allocate buffer
    uint8_t *buffer = malloc(total_len);
    if (!buffer) {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        free(json_str);
        return ESP_FAIL;
    }
    
    // Prepare send parameters
    espnow_send_param_t send_param;
    send_param.unicast = !IS_BROADCAST_ADDR(mac_addr);
    send_param.broadcast = IS_BROADCAST_ADDR(mac_addr);
    send_param.delay = 0;
    send_param.len = total_len;
    send_param.buffer = buffer;
    memcpy(send_param.dest_mac, mac_addr, ESP_NOW_ETH_ALEN);
    
    // Prepare the data
    espnow_data_prepare(&send_param, (uint8_t *)json_str, json_len);
    
    // Send the data
    esp_err_t err = esp_now_send(send_param.dest_mac, send_param.buffer, send_param.len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send failed: %s", esp_err_to_name(err));
    }
    
    // Clean up
    free(buffer);
    free(json_str);
    
    return err;
}

/* Initialize ESPNOW */
esp_err_t espnow_init(void)
{
    espnow_send_param_t *send_param;

    s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    if (s_espnow_queue == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
    ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL));
#endif
    
    /* Set primary master key. */
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    peer.channel = CONFIG_ESPNOW_CHANNEL;
    peer.ifidx = ESPNOW_WIFI_IF;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    // Get all stored peers (using the additional function)
    uint8_t all_macs[MAX_PEERS][6];
    size_t peer_count = MAX_PEERS;
    if (nvs_get_all_peers(all_macs, &peer_count) == ESP_OK) {
        for (int i = 0; i < peer_count; i++) {
            memset(&peer, 0, sizeof(esp_now_peer_info_t));
            peer.channel = CONFIG_ESPNOW_CHANNEL;
            peer.ifidx = ESPNOW_WIFI_IF;
            peer.encrypt = true;
            memcpy(peer.lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
            memcpy(peer.peer_addr, all_macs[i], ESP_NOW_ETH_ALEN);
            ESP_ERROR_CHECK(esp_now_add_peer(&peer));
            ESP_LOGI(TAG, "Peer %d: %02X:%02X:%02X:%02X:%02X:%02X", 
                    i, all_macs[i][0], all_macs[i][1], all_macs[i][2], 
                    all_macs[i][3], all_macs[i][4], all_macs[i][5]);
        }
    }

    /* Initialize sending parameters. */
    send_param = malloc(sizeof(espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG, "Malloc send parameter fail");
        espnow_deinit(send_param);
        return ESP_FAIL;
    }
    
    memset(send_param, 0, sizeof(espnow_send_param_t));
    send_param->broadcast = true;
    send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
    
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        espnow_deinit(send_param);
        return ESP_FAIL;
    }
    
    memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);

    xTaskCreate(espnow_task, "espnow_task", 8192, send_param, 4, NULL);

    return ESP_OK;
}

/* API to send data */
esp_err_t espnow_send_data(const uint8_t *mac_addr, const uint8_t *data, uint16_t len)
{
    espnow_send_param_t send_param;
    
    send_param.unicast = !IS_BROADCAST_ADDR(mac_addr);
    send_param.broadcast = IS_BROADCAST_ADDR(mac_addr);
    send_param.delay = 0; // No delay for event-based sending
    send_param.len = sizeof(espnow_data_t) + len;
    send_param.buffer = malloc(send_param.len);
    
    if (send_param.buffer == NULL) {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        return ESP_FAIL;
    }
    
    memcpy(send_param.dest_mac, mac_addr, ESP_NOW_ETH_ALEN);
    
    // Prepare the data
    espnow_data_prepare(&send_param, (uint8_t *)data, len);
    
    // Send the data
    esp_err_t err = esp_now_send(send_param.dest_mac, send_param.buffer, send_param.len);
    
    free(send_param.buffer);
    return err;
}
#include "driver/gpio.h"
#define WIFI_ENABLE      3   // GPIO3 (RF ANTENNA SWITCH EN)
#define WIFI_ANT_CONFIG  14  // GPIO14
void app_main(void) {
    ESP_LOGI(TAG, "Gateway (USB Serial/JTAG) starting...");
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WIFI_ENABLE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Configure WIFI_ANT_CONFIG (GPIO14) as output
    io_conf.pin_bit_mask = (1ULL << WIFI_ANT_CONFIG);
    gpio_config(&io_conf);

    // Set WIFI_ENABLE = LOW  (Activate RF switch control)
    gpio_set_level(WIFI_ENABLE, 0);

    // Delay 100 ms
    vTaskDelay(pdMS_TO_TICKS(100));

    // Set WIFI_ANT_CONFIG = HIGH (Use external antenna)
    gpio_set_level(WIFI_ANT_CONFIG, 1);

    // ESP_ERROR_CHECK(nvs_flash_init());
    nvs_init();

    // get gateway MAC (for replies)
    // esp_efuse_mac_get_default(s_my_mac);    
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
    char mymac[18]; mac_to_str(s_my_mac, mymac, sizeof(mymac));
    ESP_LOGI(TAG, "Gateway MAC: %s", mymac);

    // create queue for incoming USB lines
    s_usb_line_q = xQueueCreate(USB_QUEUE_LEN, sizeof(char *));
    if (!s_usb_line_q) {
        ESP_LOGE(TAG, "failed to create usb line queue");
        return;
    }

    // init wifi
    wifi_init();
    // install usb_serial_jtag driver
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 4096
    };
    ESP_ERROR_CHECK( usb_serial_jtag_driver_install(&usb_cfg) );

    // start reader and processor tasks
    xTaskCreate(usb_reader_task, "usb_reader", 4096, NULL, 5, NULL);
    xTaskCreate(usb_line_task, "usb_line", 4096, NULL, 5, NULL);
    espnow_init();

    ESP_LOGI(TAG, "Gateway ready. USB Serial/JTAG should enumerate on host.");
}
