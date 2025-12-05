#include "ssid_manager.h"

#include <algorithm>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "SsidManager"
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10

SsidManager::SsidManager() {
    LoadFromNvs();
}

SsidManager::~SsidManager() {
}

void SsidManager::Clear() {
    ssid_list_.clear();
    SaveToNvs();
}

void SsidManager::LoadFromNvs() {
    ssid_list_.clear();

    // Load ssid, password and username from NVS
    nvs_handle_t nvs_handle;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace %s doesn't exist", NVS_NAMESPACE);
        return;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string suffix = (i > 0) ? std::to_string(i) : "";
        std::string ssid_key = "ssid" + suffix;
        std::string password_key = "password" + suffix;
        std::string username_key = "username" + suffix;
        
        char ssid[33] = {0};
        char password[65] = {0};
        char username[65] = {0};

        size_t length = sizeof(ssid);
        if (nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length) != ESP_OK) {
            continue;
        }
        
        length = sizeof(password);
        if (nvs_get_str(nvs_handle, password_key.c_str(), password, &length) != ESP_OK) {
            // Password might be optional for open networks, but usually stored
        }

        length = sizeof(username);
        if (nvs_get_str(nvs_handle, username_key.c_str(), username, &length) != ESP_OK) {
            // Username is optional
            username[0] = '\0'; 
        }

        ssid_list_.push_back({ssid, password, username});
    }
    nvs_close(nvs_handle);
}

void SsidManager::SaveToNvs() {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string suffix = (i > 0) ? std::to_string(i) : "";
        std::string ssid_key = "ssid" + suffix;
        std::string password_key = "password" + suffix;
        std::string username_key = "username" + suffix;
        
        if (i < ssid_list_.size()) {
            nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str());
            nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());
            if (!ssid_list_[i].username.empty()) {
                nvs_set_str(nvs_handle, username_key.c_str(), ssid_list_[i].username.c_str());
            } else {
                nvs_erase_key(nvs_handle, username_key.c_str());
            }
        } else {
            nvs_erase_key(nvs_handle, ssid_key.c_str());
            nvs_erase_key(nvs_handle, password_key.c_str());
            nvs_erase_key(nvs_handle, username_key.c_str());
        }
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

void SsidManager::AddSsid(const std::string& ssid, const std::string& password, const std::string& username) {
    for (auto& item : ssid_list_) {
        // ESP_LOGI(TAG, "compare [%s] [%s]", item.ssid.c_str(), ssid.c_str());
        if (item.ssid == ssid) {
            ESP_LOGW(TAG, "SSID %s already exists, overwrite it", ssid.c_str());
            item.password = password;
            item.username = username;
            SaveToNvs();
            return;
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        ESP_LOGW(TAG, "SSID list is full, pop one");
        ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password, username});
    SaveToNvs();
}

void SsidManager::RemoveSsid(int index) {
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    ssid_list_.erase(ssid_list_.begin() + index);
    SaveToNvs();
}

void SsidManager::SetDefaultSsid(int index) {
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    // Move the ssid at index to the front of the list
    auto item = ssid_list_[index];
    ssid_list_.erase(ssid_list_.begin() + index);
    ssid_list_.insert(ssid_list_.begin(), item);
    SaveToNvs();
}
