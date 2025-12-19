#include "wifi_station.h"
#include <algorithm>
#include <cstring>

#include "nvs_flash.h"
#include "ssid_manager.h"
#include <esp_eap_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h> // 必须包含
#include <nvs.h>

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 5

WifiStation &WifiStation::GetInstance() {
  static WifiStation instance;
  return instance;
}

WifiStation::WifiStation() {
  event_group_ = xEventGroupCreate();

  nvs_handle_t nvs;
  esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "NVS open failed: %d", err);
  } else {
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK)
      max_tx_power_ = 0;

    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK)
      remember_bssid_ = 0;

    nvs_close(nvs);
  }
}

WifiStation::~WifiStation() {
  if (event_group_)
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string &&ssid,
                          const std::string &&password,
                          const std::string &&username) {
  SsidManager::GetInstance().AddSsid(ssid, password, username);
}

void WifiStation::Stop() {
  if (timer_handle_ != nullptr) {
    esp_timer_stop(timer_handle_);
    esp_timer_delete(timer_handle_);
    timer_handle_ = nullptr;
  }

  esp_wifi_scan_stop();

  if (instance_any_id_ != nullptr) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          instance_any_id_);
    instance_any_id_ = nullptr;
  }
  if (instance_got_ip_ != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          instance_got_ip_);
    instance_got_ip_ = nullptr;
  }

  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_sta_enterprise_disable(); // 确保清理 Enterprise 配置

  if (station_netif_ != nullptr) {
    esp_netif_destroy(station_netif_);
    station_netif_ = nullptr;
  }
  xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
  on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(
    std::function<void(const std::string &ssid)> on_connect) {
  on_connect_ = on_connect;
}

void WifiStation::OnConnected(
    std::function<void(const std::string &ssid)> on_connected) {
  on_connected_ = on_connected;
}

void WifiStation::Start() {
  if (station_netif_ == nullptr) {
      station_netif_ = esp_netif_create_default_wifi_sta();
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = false;
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::WifiEventHandler, this,
      &instance_any_id_));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::IpEventHandler, this,
      &instance_got_ip_));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  if (max_tx_power_ != 0) {
    esp_wifi_set_max_tx_power(max_tx_power_);
  }

  // 启动扫描定时器
  esp_timer_create_args_t timer_args = {.callback =
                                            [](void *arg) {
                                              // 仅仅触发扫描，不要在这里做重操作
                                              esp_wifi_scan_start(nullptr,
                                                                  false);
                                            },
                                        .arg = this,
                                        .dispatch_method = ESP_TIMER_TASK,
                                        .name = "WiFiScanTimer",
                                        .skip_unhandled_events = true};
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
  auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE,
                                  pdFALSE, pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_EVENT_CONNECTED) != 0;
}

// 该函数现在将在单独的 Task 中运行，拥有充足的栈空间
void WifiStation::HandleScanResult() {
  uint16_t ap_num = 0;
  esp_wifi_scan_get_ap_num(&ap_num);

  if (ap_num == 0) {
    ESP_LOGI(TAG, "No APs found, retry scan in 5s");
    esp_timer_start_once(timer_handle_, 5000000);
    return;
  }

  wifi_ap_record_t *ap_records =
      (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
  if (!ap_records)
    return;

  esp_wifi_scan_get_ap_records(&ap_num, ap_records);

  // 按信号强度排序
  std::sort(ap_records, ap_records + ap_num,
            [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) {
              return a.rssi > b.rssi;
            });

  auto &ssid_manager = SsidManager::GetInstance();
  auto ssid_list = ssid_manager.GetSsidList();

  // 清空旧队列
  connect_queue_.clear();

  for (int i = 0; i < ap_num; i++) {
    auto ap_record = ap_records[i];
    for (const auto &item : ssid_list) {
      if (item.ssid == (char *)ap_record.ssid) {
        ESP_LOGI(TAG, "Match AP: %s (RSSI: %d, Auth: %d)", ap_record.ssid,
                 ap_record.rssi, ap_record.authmode);
        WifiApRecord record = {.ssid = item.ssid,
                               .password = item.password,
                               .username = item.username,
                               .channel = ap_record.primary,
                               .authmode = ap_record.authmode};
        memcpy(record.bssid, ap_record.bssid, 6);
        connect_queue_.push_back(record);
        // 找到一个匹配的就够了，如果需要多SSID漫游策略可调整
        break;
      }
    }
  }
  free(ap_records);

  if (connect_queue_.empty()) {
    ESP_LOGI(TAG, "No known AP found, wait for next scan");
    esp_timer_start_once(timer_handle_, 10000000); // 10秒后重试
    return;
  }

  StartConnect();
}

void WifiStation::StartConnect() {
  if (connect_queue_.empty())
    return;

  auto ap_record = connect_queue_.front();
  connect_queue_.erase(connect_queue_.begin());

  ssid_ = ap_record.ssid;
  password_ = ap_record.password;
  username_ = ap_record.username;

  if (on_connect_) {
    on_connect_(ssid_);
  }

  ESP_LOGI(TAG, "Preparing to connect to %s...", ssid_.c_str());

  // 配置 WPA2 Enterprise (802.1x)
  if (!username_.empty()) {
    ESP_LOGI(TAG, "Configuring WPA2 Enterprise (User: %s)", username_.c_str());
    ESP_ERROR_CHECK(esp_eap_client_set_identity(
        (const uint8_t *)username_.c_str(), username_.length()));
    ESP_ERROR_CHECK(esp_eap_client_set_username(
        (const uint8_t *)username_.c_str(), username_.length()));
    ESP_ERROR_CHECK(esp_eap_client_set_password(
        (const uint8_t *)password_.c_str(), password_.length()));
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
  } else {
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_disable());
  }

  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  strlcpy((char *)wifi_config.sta.ssid, ssid_.c_str(),
          sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, password_.c_str(),
          sizeof(wifi_config.sta.password));

  if (remember_bssid_) {
    wifi_config.sta.channel = ap_record.channel;
    memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
    wifi_config.sta.bssid_set = true;
  }

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  reconnect_count_ = 0;
  ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return ap_info.rssi;
  }
  return -127;
}

uint8_t WifiStation::GetChannel() {
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return ap_info.primary;
  }
  return 0;
}

bool WifiStation::IsConnected() {
  return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
  esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void WifiStation::WifiEventHandler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data) {
  auto *this_ = static_cast<WifiStation *>(arg);

  if (event_id == WIFI_EVENT_STA_START) {
    esp_wifi_scan_start(nullptr, false);
    if (this_->on_scan_begin_)
      this_->on_scan_begin_();

  } else if (event_id == WIFI_EVENT_SCAN_DONE) {
    // [关键修复]：不要在 Event Handler 中直接调用 HandleScanResult
    // 创建一个临时 Task 来处理扫描结果和连接，给予 4KB 栈空间
    xTaskCreate(
        [](void *task_arg) {
          auto *station = static_cast<WifiStation *>(task_arg);
          station->HandleScanResult();
          vTaskDelete(NULL); // 任务完成后自杀
        },
        "wifi_logic", 4096, this_, 5, NULL);

  } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);

    if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
      this_->reconnect_count_++;
      ESP_LOGI(TAG, "Reconnecting... (%d/%d)", this_->reconnect_count_,
               MAX_RECONNECT_COUNT);
      esp_wifi_connect();
      return;
    }

    if (!this_->connect_queue_.empty()) {
      ESP_LOGI(TAG, "Connection failed, trying next AP in queue...");
      // 同样，连接逻辑建议在 Task 中运行，或者确保 StartConnect 不做太重的操作
      // 这里为了简单，我们复用上面的逻辑，或者简单调用
      xTaskCreate(
          [](void *task_arg) {
            auto *station = static_cast<WifiStation *>(task_arg);
            station->StartConnect();
            vTaskDelete(NULL);
          },
          "wifi_retry", 4096, this_, 5, NULL);
      return;
    }

    ESP_LOGI(TAG, "All attempts failed. Retry scan in 10s");
    esp_timer_start_once(this_->timer_handle_, 10000000);
  }
}

void WifiStation::IpEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
  auto *this_ = static_cast<WifiStation *>(arg);
  auto *event = static_cast<ip_event_got_ip_t *>(event_data);

  char ip_address[16];
  esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
  this_->ip_address_ = ip_address;
  ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());

  xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);

  if (this_->on_connected_) {
    this_->on_connected_(this_->ssid_);
  }

  this_->connect_queue_.clear();
  this_->reconnect_count_ = 0;
}
