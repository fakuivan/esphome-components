#include "mi_stick_ble_remote.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_hid_common.h"
#include "esp_hidd_gatts.h"

namespace esphome::mi_stick_ble_remote {

static const char *const TAG = "mi_stick_ble_remote";
static MiStickBLERemote *global_remote = nullptr;

static constexpr uint8_t kHidReportMapIndex = 0;
static constexpr uint8_t kXiaomiRcReportId = 1;
static constexpr uint8_t kXiaomiRcPowerReport[3] = {0x20, 0x00, 0x00};
static constexpr uint8_t kXiaomiRcHomeReport[3] = {0x00, 0x00, 0x04};
static constexpr uint8_t kReleaseReport[3] = {0x00, 0x00, 0x00};
static constexpr uint8_t kBleFlags =
    ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
static constexpr uint8_t kHidServiceUuid[] = {0x12, 0x18};
static constexpr uint8_t kAdvertisedTxPower = 0x00;
static constexpr size_t kMaxBleAdvertisementLength = 31;
static constexpr size_t kMaxInputReportLength = 3;
static constexpr size_t kMaxBondedDevicesToClear = 8;
static constexpr uint32_t kWakeRetryWindowMs = 6000;
static constexpr uint32_t kWakeRetryDelayMs = 300;

// HID report map and report values were inferred from local btsnoop captures of
// the stock Xiaomi RC remote for this Mi Stick. The Report ID is explicit
// because Android bonded and woke reliably only after descriptor rediscovery
// with Report Reference ID 1.
static constexpr uint8_t XIAOMI_RC_REPORT_MAP[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)

    0x09, 0x41,        //   Usage (Menu Pick)
    0x09, 0x42,        //   Usage (Menu Up)
    0x09, 0x43,        //   Usage (Menu Down)
    0x09, 0x44,        //   Usage (Menu Left)
    0x09, 0x45,        //   Usage (Menu Right)
    0x09, 0x30,        //   Usage (Power)
    0x0A, 0xCF, 0x00,  //   Usage (Voice Command)
    0x0A, 0xE9, 0x00,  //   Usage (Volume Increment)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    0x0A, 0xEA, 0x00,  //   Usage (Volume Decrement)
    0x0A, 0xA2, 0x01,  //   Usage (Application Launch Button)
    0x0A, 0xB8, 0x01,  //   Usage (Media Select Computer)
    0x95, 0x03,        //   Report Count (3)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x03,        //   Input (Constant, Variable, Absolute)
    0x09, 0x6C,        //   Usage (Yellow)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x03,        //   Input (Constant, Variable, Absolute)

    0x95, 0x02,        //   Report Count (2)
    0x81, 0x03,        //   Input (Constant, Variable, Absolute)
    0x0A, 0x23, 0x02,  //   Usage (AC Home)
    0x0A, 0x24, 0x02,  //   Usage (AC Back)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x03,        //   Input (Constant, Variable, Absolute)
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t xiaomi_rc_report_maps[] = {
    {
        .data = XIAOMI_RC_REPORT_MAP,
        .len = sizeof(XIAOMI_RC_REPORT_MAP),
    },
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (global_remote != nullptr)
    global_remote->handle_gap_event(event, param);
}

static void hidd_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
  if (global_remote != nullptr)
    global_remote->handle_hidd_event(static_cast<esp_hidd_event_t>(id),
                                     static_cast<esp_hidd_event_data_t *>(event_data));
}

void MiStickBLERemote::setup() {
  ESP_LOGI(TAG, "Setting up Xiaomi RC BLE HID remote as %s", this->name_.c_str());
  global_remote = this;
  this->publish_connected_(false);
  this->publish_last_report_ok_(false);
  this->publish_suspended_(false);
  if (this->advertise_on_boot_)
    this->start_advertising();
}

void MiStickBLERemote::dump_config() {
  ESP_LOGCONFIG(TAG, "Mi Stick BLE HID Remote:");
  ESP_LOGCONFIG(TAG, "  Name: %s", this->name_.c_str());
  ESP_LOGCONFIG(TAG, "  VID/PID/version: 0x%04X/0x%04X/0x%04X", this->vendor_id_, this->product_id_, this->version_);
  ESP_LOGCONFIG(TAG, "  Static Random Address: %02X:%02X:%02X:%02X:%02X:%02X", this->static_random_address_[0],
                this->static_random_address_[1], this->static_random_address_[2], this->static_random_address_[3],
                this->static_random_address_[4], this->static_random_address_[5]);
  ESP_LOGCONFIG(TAG, "  Press Duration: %" PRIu32 " ms", this->press_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Advertise On Boot: %s", this->advertise_on_boot_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Ready: %s", this->ready_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Connected: %s", this->connected_ ? "YES" : "NO");
}

void MiStickBLERemote::start_advertising() {
  if (!this->init_ble_())
    return;
  this->try_start_advertising_();
}

void MiStickBLERemote::stop_advertising() {
  if (!this->advertising_)
    return;
  esp_err_t err = esp_ble_gap_stop_advertising();
  if (err != ESP_OK)
    ESP_LOGW(TAG, "esp_ble_gap_stop_advertising failed: %s", esp_err_to_name(err));
}

void MiStickBLERemote::clear_bonds() {
  if (!this->init_ble_())
    return;

  int dev_num = esp_ble_get_bond_device_num();
  if (dev_num <= 0) {
    ESP_LOGI(TAG, "No BLE bonds to clear");
    return;
  }

  std::array<esp_ble_bond_dev_t, kMaxBondedDevicesToClear> dev_list{};
  if (static_cast<size_t>(dev_num) > dev_list.size()) {
    ESP_LOGW(TAG, "Only clearing the first %zu of %d BLE bond(s)", dev_list.size(), dev_num);
    dev_num = dev_list.size();
  }

  esp_err_t err = esp_ble_get_bond_device_list(&dev_num, dev_list.data());
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_get_bond_device_list failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(TAG, "Clearing %d BLE bond(s)", dev_num);
  for (int i = 0; i < dev_num; i++) {
    const uint8_t *addr = dev_list[i].bd_addr;
    ESP_LOGI(TAG, "Removing BLE bond %02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5]);
    err = esp_ble_remove_bond_device(dev_list[i].bd_addr);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "esp_ble_remove_bond_device failed: %s", esp_err_to_name(err));
  }
}

void MiStickBLERemote::power() {
  (void) this->send_button_("XIAOMI RC POWER", kHidReportMapIndex, kXiaomiRcReportId, kXiaomiRcPowerReport,
                            sizeof(kXiaomiRcPowerReport));
}

void MiStickBLERemote::home() {
  (void) this->send_home_();
}

bool MiStickBLERemote::send_home_() {
  return this->send_button_("XIAOMI RC HOME", kHidReportMapIndex, kXiaomiRcReportId, kXiaomiRcHomeReport,
                            sizeof(kXiaomiRcHomeReport));
}

void MiStickBLERemote::wake() {
  if (this->send_home_())
    return;

  ESP_LOGW(TAG, "HOME report failed; restarting BLE HID service and retrying for %" PRIu32 " ms",
           kWakeRetryWindowMs);
  this->restart_hid_();

  const uint32_t start_ms = millis();
  while (millis() - start_ms < kWakeRetryWindowMs) {
    delay(kWakeRetryDelayMs);
    if (!this->connected_)
      continue;
    if (this->send_home_())
      return;
  }

  ESP_LOGW(TAG, "HOME wake retry window expired before an input report was accepted");
  this->publish_last_report_ok_(false);
}

void MiStickBLERemote::send_xiaomi_report(uint8_t byte0, uint8_t byte1, uint8_t byte2) {
  const uint8_t report[3] = {byte0, byte1, byte2};
  (void) this->send_button_("XIAOMI RC RAW", kHidReportMapIndex, kXiaomiRcReportId, report, sizeof(report));
}

bool MiStickBLERemote::send_button_(const char *label, uint8_t report_map_index, uint8_t report_id,
                                    const uint8_t *press_report, size_t report_len) {
  if (!this->connected_ || this->hid_dev_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send BLE %s because no HID host is connected", label);
    this->publish_last_report_ok_(false);
    return false;
  }
  ESP_LOGI(TAG, "Sending BLE %s report", label);
  const bool press_report_sent = this->send_report_(report_map_index, report_id, press_report, report_len);
  delay(this->press_duration_ms_);
  const bool release_report_sent = this->release_(report_map_index, report_id, report_len);
  const bool all_reports_sent = press_report_sent && release_report_sent;
  ESP_LOGI(TAG, "BLE %s report result: press=%s release=%s", label, press_report_sent ? "OK" : "FAILED",
           release_report_sent ? "OK" : "FAILED");
  this->publish_last_report_ok_(all_reports_sent);
  return all_reports_sent;
}

bool MiStickBLERemote::init_ble_() {
  if (this->ready_)
    return true;

  auto controller_status = esp_bt_controller_get_status();
  if (controller_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
      return false;
    }
    controller_status = esp_bt_controller_get_status();
  }

  if (controller_status != ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "esp_bt_controller_enable BLE failed: %s", esp_err_to_name(err));
      return false;
    }
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bluedroid_init_with_cfg failed: %s", esp_err_to_name(err));
      return false;
    }
  }

  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    esp_err_t err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
      return false;
    }
  }

  // Fixed static-random address keeps bonds stable while avoiding stale Android
  // cache entries for the ESP32 public address during descriptor experiments.
  esp_err_t random_addr_err = esp_ble_gap_set_rand_addr(this->static_random_address_);
  if (random_addr_err != ESP_OK && random_addr_err != ESP_ERR_INVALID_STATE)
    ESP_LOGW(TAG, "esp_ble_gap_set_rand_addr failed: %s", esp_err_to_name(random_addr_err));

  esp_err_t err = esp_ble_gap_register_callback(gap_callback);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gap_register_callback failed: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gatts_register_callback failed: %s", esp_err_to_name(err));
    return false;
  }

  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t key_size = 16;
  if (!this->set_security_param_(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req), "auth request") ||
      !this->set_security_param_(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap), "IO capabilities") ||
      !this->set_security_param_(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key), "init key mask") ||
      !this->set_security_param_(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key), "response key mask") ||
      !this->set_security_param_(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size), "max key size")) {
    return false;
  }

  err = esp_ble_gap_set_device_name(this->name_.c_str());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gap_set_device_name failed: %s", esp_err_to_name(err));
    return false;
  }

  if (!this->configure_advertising_())
    return false;

  esp_hid_device_config_t hid_config = {
      .vendor_id = this->vendor_id_,
      .product_id = this->product_id_,
      .version = this->version_,
      .device_name = this->name_.c_str(),
      .manufacturer_name = "ESPHome",
      .serial_number = "esphome-mi-stick-ble",
      .report_maps = xiaomi_rc_report_maps,
      .report_maps_len = 1,
  };
  err = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_callback, &this->hid_dev_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_hidd_dev_init BLE failed: %s", esp_err_to_name(err));
    return false;
  }

  this->ready_ = true;
  return true;
}

bool MiStickBLERemote::set_security_param_(esp_ble_sm_param_t param_type, void *value, uint8_t len, const char *name) {
  esp_err_t err = esp_ble_gap_set_security_param(param_type, value, len);
  if (err == ESP_OK)
    return true;

  ESP_LOGE(TAG, "esp_ble_gap_set_security_param failed for %s: %s", name, esp_err_to_name(err));
  return false;
}

bool MiStickBLERemote::configure_advertising_() {
  this->adv_data_len_ = 0;
  if (!this->append_adv_field_(ESP_BLE_AD_TYPE_FLAG, &kBleFlags, sizeof(kBleFlags)))
    return false;
  if (!this->append_adv_field_(ESP_BLE_AD_TYPE_16SRV_CMPL, kHidServiceUuid, sizeof(kHidServiceUuid)))
    return false;
  // Xiaomi's BleRemoteService extracts TX power while deciding whether to auto-pair.
  if (!this->append_adv_field_(ESP_BLE_AD_TYPE_TX_PWR, &kAdvertisedTxPower, sizeof(kAdvertisedTxPower)))
    return false;

  const size_t remaining = kMaxBleAdvertisementLength - this->adv_data_len_;
  if (remaining < 2) {
    ESP_LOGE(TAG, "BLE advertising data has no room for device name");
    return false;
  }

  const size_t max_name_len = remaining - 2;
  const size_t name_len = std::min(this->name_.size(), max_name_len);
  if (name_len < this->name_.size())
    ESP_LOGW(TAG, "BLE name '%s' truncated to %zu bytes in advertising data", this->name_.c_str(), name_len);

  if (!this->append_adv_field_(name_len == this->name_.size() ? ESP_BLE_AD_TYPE_NAME_CMPL : ESP_BLE_AD_TYPE_NAME_SHORT,
                               reinterpret_cast<const uint8_t *>(this->name_.data()), name_len)) {
    return false;
  }

  esp_err_t err = esp_ble_gap_config_adv_data_raw(this->adv_data_, this->adv_data_len_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gap_config_adv_data_raw failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool MiStickBLERemote::append_adv_field_(uint8_t field_type, const uint8_t *data, size_t data_len) {
  const size_t field_len = data_len + 2;
  if (this->adv_data_len_ + field_len > kMaxBleAdvertisementLength) {
    ESP_LOGE(TAG, "BLE advertising field 0x%02X does not fit (%zu bytes left, %zu needed)", field_type,
             kMaxBleAdvertisementLength - this->adv_data_len_, field_len);
    return false;
  }

  this->adv_data_[this->adv_data_len_++] = data_len + 1;
  this->adv_data_[this->adv_data_len_++] = field_type;
  memcpy(&this->adv_data_[this->adv_data_len_], data, data_len);
  this->adv_data_len_ += data_len;
  return true;
}

void MiStickBLERemote::try_start_advertising_() {
  if (!this->adv_configured_ || !this->hidd_started_ || this->connected_ || this->advertising_)
    return;

  ESP_LOGI(TAG, "Starting BLE HID advertising as %s", this->name_.c_str());
  esp_err_t err = esp_ble_gap_start_advertising(&adv_params);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "esp_ble_gap_start_advertising failed: %s", esp_err_to_name(err));
}

void MiStickBLERemote::publish_connected_(bool connected) {
  if (this->connected_sensor_ != nullptr)
    this->connected_sensor_->publish_state(connected);
}

void MiStickBLERemote::publish_last_report_ok_(bool report_sent) {
  if (this->last_report_ok_sensor_ != nullptr)
    this->last_report_ok_sensor_->publish_state(report_sent);
}

void MiStickBLERemote::publish_suspended_(bool suspended) {
  if (this->suspended_sensor_ != nullptr)
    this->suspended_sensor_->publish_state(suspended);
}

bool MiStickBLERemote::send_report_(uint8_t report_map_index, uint8_t report_id, const uint8_t *report,
                                    size_t report_len) {
  if (report_len > kMaxInputReportLength) {
    ESP_LOGE(TAG, "BLE HID input report is too long: %zu bytes", report_len);
    return false;
  }

  uint8_t buffer[kMaxInputReportLength]{};
  memcpy(buffer, report, report_len);
  esp_err_t err = esp_hidd_dev_input_set(this->hid_dev_, report_map_index, report_id, buffer, report_len);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_hidd_dev_input_set failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool MiStickBLERemote::release_(uint8_t report_map_index, uint8_t report_id, size_t report_len) {
  return this->send_report_(report_map_index, report_id, kReleaseReport, report_len);
}

void MiStickBLERemote::restart_hid_() {
  ESP_LOGI(TAG, "Restarting BLE HID service without clearing bonds");

  if (this->advertising_) {
    esp_err_t err = esp_ble_gap_stop_advertising();
    if (err != ESP_OK)
      ESP_LOGW(TAG, "esp_ble_gap_stop_advertising failed during restart: %s", esp_err_to_name(err));
  }

  if (this->hid_dev_ != nullptr) {
    esp_err_t err = esp_hidd_dev_deinit(this->hid_dev_);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "esp_hidd_dev_deinit failed during restart: %s", esp_err_to_name(err));
    this->hid_dev_ = nullptr;
  }

  this->ready_ = false;
  this->adv_configured_ = false;
  this->hidd_started_ = false;
  this->advertising_ = false;
  this->connected_ = false;
  this->publish_connected_(false);
  this->publish_suspended_(false);

  this->start_advertising();
}

void MiStickBLERemote::handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
      ESP_LOGI(TAG, "BLE advertising data configured, status=%d", param->adv_data_cmpl.status);
      this->adv_configured_ = param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
      this->try_start_advertising_();
      break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      ESP_LOGI(TAG, "BLE raw advertising data configured, status=%d", param->adv_data_raw_cmpl.status);
      this->adv_configured_ = param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS;
      this->try_start_advertising_();
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      this->advertising_ = param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
      ESP_LOGI(TAG, "BLE advertising start %s", this->advertising_ ? "succeeded" : "failed");
      break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      this->advertising_ = false;
      ESP_LOGI(TAG, "BLE advertising stopped, status=%d", param->adv_stop_cmpl.status);
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      ESP_LOGI(TAG, "BLE security request accepted");
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      ESP_LOGI(TAG, "BLE passkey notification: %06" PRIu32, param->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      ESP_LOGI(TAG, "BLE numeric comparison accepted: %06" PRIu32, param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      ESP_LOGI(TAG, "BLE authentication %s, fail_reason=%u",
               param->ble_security.auth_cmpl.success ? "succeeded" : "failed",
               param->ble_security.auth_cmpl.fail_reason);
      break;
    default:
      ESP_LOGD(TAG, "BLE GAP event: %d", event);
      break;
  }
}

void MiStickBLERemote::handle_hidd_event(esp_hidd_event_t event, esp_hidd_event_data_t *param) {
  switch (event) {
    case ESP_HIDD_START_EVENT:
      ESP_LOGI(TAG, "BLE HID started");
      this->hidd_started_ = true;
      this->try_start_advertising_();
      break;
    case ESP_HIDD_CONNECT_EVENT:
      ESP_LOGI(TAG, "BLE HID host connected");
      this->connected_ = true;
      this->advertising_ = false;
      this->publish_connected_(true);
      this->publish_suspended_(false);
      break;
    case ESP_HIDD_DISCONNECT_EVENT:
      ESP_LOGI(TAG, "BLE HID host disconnected, reason=%d", param->disconnect.reason);
      this->connected_ = false;
      this->publish_connected_(false);
      this->publish_suspended_(false);
      this->advertising_ = false;
      if (this->advertise_on_boot_)
        this->try_start_advertising_();
      break;
    case ESP_HIDD_CONTROL_EVENT:
      ESP_LOGI(TAG, "BLE HID control event: %u", param->control.control);
      if (param->control.control == ESP_HID_CONTROL_SUSPEND || param->control.control == ESP_HID_CONTROL_EXIT_SUSPEND)
        this->publish_suspended_(param->control.control == ESP_HID_CONTROL_SUSPEND);
      break;
    case ESP_HIDD_OUTPUT_EVENT:
      ESP_LOGD(TAG, "BLE HID output report id=%u len=%u", param->output.report_id, param->output.length);
      break;
    case ESP_HIDD_FEATURE_EVENT:
      ESP_LOGD(TAG, "BLE HID feature report id=%u len=%u", param->feature.report_id, param->feature.length);
      break;
    default:
      ESP_LOGD(TAG, "BLE HID event: %d", event);
      break;
  }
}

}  // namespace esphome::mi_stick_ble_remote
