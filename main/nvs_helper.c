/* NVS_HELPER.C
   NVS Helper Implementation File

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "nvs_helper.h"
// #include "cJSON.h"
// #include "espnow_data.h"

static const char *TAG = "nvs_helper";

/* Configs */
#define TAG "nvs_peer_mac"
#define NVS_NAMESPACE "peer_storage"
#define PEER_MAC_KEY "peer_macs"

static nvs_handle_t gnvs_handle;
esp_err_t nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Open NVS namespace
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &gnvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t nvs_store_peer_mac(const uint8_t *mac) {
    // First, load existing peers
    uint8_t mac_list[MAX_PEERS][6];
    size_t required_size = 0;
    esp_err_t ret = nvs_get_blob(gnvs_handle, PEER_MAC_KEY, NULL, &required_size);
    
    size_t peer_count = 0;
    if (ret == ESP_OK && required_size > 0) {
        // Existing peers found
        ret = nvs_get_blob(gnvs_handle, PEER_MAC_KEY, mac_list, &required_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error reading peer list: %s", esp_err_to_name(ret));
            return ret;
        }
        peer_count = required_size / 6;
        
        // Check if MAC already exists
        for (int i = 0; i < peer_count; i++) {
            if (memcmp(mac_list[i], mac, 6) == 0) {
                ESP_LOGW(TAG, "MAC already exists in storage");
                return ESP_OK; // MAC already exists
            }
        }
    }
    
    // Check if we have space for new peer
    if (peer_count >= MAX_PEERS) {
        ESP_LOGE(TAG, "Peer storage full (max %d peers)", MAX_PEERS);
        return ESP_ERR_NO_MEM;
    }
    
    // Add new MAC to list
    memcpy(mac_list[peer_count], mac, 6);
    peer_count++;
    
    // Store updated list
    ret = nvs_set_blob(gnvs_handle, PEER_MAC_KEY, mac_list, peer_count * 6);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error storing peer list: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Commit changes
    ret = nvs_commit(gnvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing peer list: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Stored peer MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t nvs_load_peer_mac(uint8_t *mac_out) {
    // This function loads the first peer MAC
    // For multiple peers, you'd need a different approach
    
    size_t required_size = 0;
    esp_err_t ret = nvs_get_blob(gnvs_handle, PEER_MAC_KEY, NULL, &required_size);
    
    if (ret != ESP_OK || required_size == 0) {
        ESP_LOGE(TAG, "No peer MACs found in storage");
        return ESP_ERR_NOT_FOUND;
    }
    
    if (required_size < 6) {
        ESP_LOGE(TAG, "Invalid peer data size: %d", required_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint8_t mac_list[MAX_PEERS][6];
    ret = nvs_get_blob(gnvs_handle, PEER_MAC_KEY, mac_list, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error reading peer list: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Return the first MAC
    memcpy(mac_out, mac_list[0], 6);
    
    ESP_LOGI(TAG, "Loaded peer MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5]);
    return ESP_OK;
}

esp_err_t nvs_erase_peer_mac(void) {
    esp_err_t ret = nvs_erase_key(gnvs_handle, PEER_MAC_KEY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error erasing peer key: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_commit(gnvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing erase: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Erased all peer MACs from storage");
    return ESP_OK;
}

// Additional function to get all stored peers
esp_err_t nvs_get_all_peers(uint8_t mac_list[][6], size_t *count) {
    size_t required_size = 0;
    esp_err_t ret = nvs_get_blob(gnvs_handle, PEER_MAC_KEY, NULL, &required_size);

    ESP_LOGI(TAG, "nvs_get_all_peers: required_size=%d", required_size);
    
    if (ret != ESP_OK || required_size == 0) {
        *count = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    if (required_size % 6 != 0) {
        ESP_LOGE(TAG, "Invalid peer data size: %d", required_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    *count = required_size / 6;
    ret = nvs_get_blob(gnvs_handle, PEER_MAC_KEY, mac_list, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error reading peer list: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

// Close NVS when done (optional)
// void nvs_close(void) {
//     nvs_close(nvs_handle);
// }
