#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_gap_ble_api.h"
#include "esp_hidd.h"

namespace esphome::mi_stick_ble_remote {

class MiStickBLERemote : public Component {
 public:
  void setup() override;
  void dump_config() override;

  void set_name(const std::string &name) { this->name_ = name; }
  void set_vendor_id(uint16_t vendor_id) { this->vendor_id_ = vendor_id; }
  void set_product_id(uint16_t product_id) { this->product_id_ = product_id; }
  void set_power_press_duration_ms(uint32_t duration_ms) { this->power_press_duration_ms_ = duration_ms; }
  void set_advertise_on_boot(bool advertise_on_boot) { this->advertise_on_boot_ = advertise_on_boot; }
  void set_connected_sensor(binary_sensor::BinarySensor *sensor) { this->connected_sensor_ = sensor; }
  void set_last_power_report_ok_sensor(binary_sensor::BinarySensor *sensor) { this->last_power_report_ok_sensor_ = sensor; }
  void set_suspended_sensor(binary_sensor::BinarySensor *sensor) { this->suspended_sensor_ = sensor; }

  void start_advertising();
  void stop_advertising();
  void power();
  void handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  void handle_hidd_event(esp_hidd_event_t event, esp_hidd_event_data_t *param);

 protected:
  bool init_ble_();
  bool set_security_param_(esp_ble_sm_param_t param_type, void *value, uint8_t len, const char *name);
  bool configure_advertising_();
  bool append_adv_field_(uint8_t field_type, const uint8_t *data, size_t data_len);
  void try_start_advertising_();
  void publish_connected_(bool connected);
  void publish_last_power_report_ok_(bool report_sent);
  void publish_suspended_(bool suspended);
  bool send_report_(const uint8_t report[3]);
  bool release_();

  std::string name_{"Xiaomi RC"};
  uint16_t vendor_id_{0x2717};
  uint16_t product_id_{0x32B9};
  uint16_t version_{0x0001};
  uint32_t power_press_duration_ms_{120};
  bool advertise_on_boot_{true};
  bool ready_{false};
  bool adv_configured_{false};
  bool hidd_started_{false};
  bool advertising_{false};
  bool connected_{false};
  uint8_t adv_data_[31]{};
  size_t adv_data_len_{0};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  binary_sensor::BinarySensor *last_power_report_ok_sensor_{nullptr};
  binary_sensor::BinarySensor *suspended_sensor_{nullptr};
  esp_hidd_dev_t *hid_dev_{nullptr};
};

template<typename... Ts> class StartAdvertisingAction : public Action<Ts...> {
 public:
  explicit StartAdvertisingAction(MiStickBLERemote *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->start_advertising(); }

 protected:
  MiStickBLERemote *parent_;
};

template<typename... Ts> class StopAdvertisingAction : public Action<Ts...> {
 public:
  explicit StopAdvertisingAction(MiStickBLERemote *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop_advertising(); }

 protected:
  MiStickBLERemote *parent_;
};

template<typename... Ts> class PowerAction : public Action<Ts...> {
 public:
  explicit PowerAction(MiStickBLERemote *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->power(); }

 protected:
  MiStickBLERemote *parent_;
};

}  // namespace esphome::mi_stick_ble_remote
