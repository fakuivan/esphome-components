#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || \
    defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/lock_free_queue.h"

namespace esphome::usb_hid_ups {

static const char* const TAG = "usb_hid_ups";

enum class HIDReportType : uint8_t {
  INPUT = 1,
  FEATURE = 3,
};

enum class HIDFieldRole : uint8_t {
  UNKNOWN = 0,
  INPUT,
  OUTPUT,
  BATTERY,
  BATTERY_SYSTEM,
};

enum class MetricKind : uint8_t {
  BATTERY_LEVEL = 0,
  RUNTIME,
  LOAD_PERCENT,
  INPUT_VOLTAGE,
  OUTPUT_VOLTAGE,
  BATTERY_VOLTAGE,
  COUNT,
};

enum class ControlAction : uint8_t {
  NONE = 0,
  REPORT_DESCRIPTOR,
  GET_REPORT,
};

enum class EventKind : uint8_t {
  INPUT_REPORT = 0,
  POLLED_REPORT,
};

struct HIDInterfaceContext {
  uint8_t interface_number{0xFF};
  uint8_t interrupt_in_endpoint{0};
  uint16_t interrupt_packet_size{0};
  uint16_t report_descriptor_length{0};
};

struct HIDReportLayout {
  HIDReportType report_type{HIDReportType::INPUT};
  uint8_t report_id{0};
  uint16_t bit_length{0};
};

struct HIDField {
  uint16_t usage_page{0};
  uint16_t usage{0};
  HIDFieldRole role{HIDFieldRole::UNKNOWN};
  HIDReportType report_type{HIDReportType::INPUT};
  uint16_t bit_offset{0};
  int32_t logical_min{0};
  int32_t logical_max{0};
  uint32_t unit{0};
  uint8_t report_id{0};
  uint8_t bit_size{0};
  int8_t unit_exponent{0};
  bool is_signed{false};
};

struct MetricBinding {
  bool available{false};
  uint8_t field_index{0xFF};
};

struct ReportTarget {
  HIDReportType report_type{HIDReportType::INPUT};
  uint8_t report_id{0};
  uint16_t expected_length{0};
};

struct ReportEvent {
  static constexpr size_t MAX_REPORT_BYTES = 512;

  EventKind kind{EventKind::INPUT_REPORT};
  HIDReportType report_type{HIDReportType::INPUT};
  uint16_t length{0};
  bool truncated{false};
  uint8_t data[MAX_REPORT_BYTES]{};

  void release() {}
};

class USBHIDUPS : public usb_host::USBClient {
 public:
  static constexpr size_t MAX_FIELDS = 128;
  static constexpr size_t MAX_REPORT_LAYOUTS = 32;
  static constexpr size_t MAX_REPORT_TARGETS = 32;
  static constexpr size_t MAX_CONTROL_DATA_BYTES = 1024;
  static constexpr size_t REPORT_EVENT_QUEUE_SIZE = 4;

  USBHIDUPS(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_update_interval(uint32_t update_interval_ms) {
    this->update_interval_ms_ = update_interval_ms;
  }
  void set_explore(bool explore) { this->explore_ = explore; }
  void set_normalize_apc_pages(bool normalize_apc_pages) {
    this->normalize_apc_pages_ = normalize_apc_pages;
  }
  void set_interface_number(uint8_t interface_number) {
    this->has_forced_interface_number_ = true;
    this->forced_interface_number_ = interface_number;
  }
  void set_interrupt_in_endpoint(uint8_t interrupt_in_endpoint) {
    this->has_forced_interrupt_in_endpoint_ = true;
    this->forced_interrupt_in_endpoint_ = interrupt_in_endpoint;
  }
  void set_report_descriptor_index(uint8_t report_descriptor_index) {
    this->report_descriptor_index_ = report_descriptor_index;
  }

  void set_battery_level_sensor(sensor::Sensor* battery_level_sensor) {
    this->battery_level_sensor_ = battery_level_sensor;
  }
  void set_runtime_sensor(sensor::Sensor* runtime_sensor) {
    this->runtime_sensor_ = runtime_sensor;
  }
  void set_load_percent_sensor(sensor::Sensor* load_percent_sensor) {
    this->load_percent_sensor_ = load_percent_sensor;
  }
  void set_input_voltage_sensor(sensor::Sensor* input_voltage_sensor) {
    this->input_voltage_sensor_ = input_voltage_sensor;
  }
  void set_output_voltage_sensor(sensor::Sensor* output_voltage_sensor) {
    this->output_voltage_sensor_ = output_voltage_sensor;
  }
  void set_battery_voltage_sensor(sensor::Sensor* battery_voltage_sensor) {
    this->battery_voltage_sensor_ = battery_voltage_sensor;
  }

 protected:
  void on_connected() override;
  void on_disconnected() override;

  bool process_report_events_();
  bool discover_hid_interface_();
  bool fetch_report_descriptor_();
  bool parse_report_descriptor_();
  bool build_metric_bindings_();
  void handle_descriptor_ready_();
  void handle_report_event_(const ReportEvent& event);
  void finalize_runtime_();
  void request_poll_cycle_();
  void submit_next_poll_request_();
  bool submit_get_report_(const ReportTarget& target);
  bool submit_control_transfer_(uint8_t request_type, uint8_t request,
                                uint16_t value, uint16_t index, uint16_t length,
                                ControlAction action,
                                const ReportTarget* target = nullptr);
  bool register_report_target_(const HIDField& field);
  void start_interrupt_in_transfer_();
  void dump_parsed_fields_() const;

  HIDReportLayout* find_or_create_report_layout_(HIDReportType report_type,
                                                 uint8_t report_id);
  const HIDReportLayout* find_report_layout_(HIDReportType report_type,
                                             uint8_t report_id) const;
  sensor::Sensor* get_metric_sensor_(MetricKind metric) const;
  void publish_metric_(MetricKind metric, float value);
  uint16_t report_length_for_field_(const HIDField& field) const;
  int score_field_(MetricKind metric, const HIDField& field) const;
  bool decode_report_id_(HIDReportType report_type, const uint8_t* data,
                         size_t data_len, uint8_t* report_id,
                         size_t* payload_offset) const;
  float extract_field_value_(const HIDField& field, const uint8_t* payload,
                             size_t payload_len) const;

  static void control_transfer_callback_(const usb_transfer_t* transfer);

  LockFreeQueue<ReportEvent, REPORT_EVENT_QUEUE_SIZE> report_queue_;
  EventPool<ReportEvent, REPORT_EVENT_QUEUE_SIZE - 1> report_pool_;
  std::unique_ptr<uint8_t[]> report_descriptor_buffer_;
  std::array<HIDField, MAX_FIELDS> fields_{};
  std::array<HIDReportLayout, MAX_REPORT_LAYOUTS> report_layouts_{};
  std::array<MetricBinding, static_cast<size_t>(MetricKind::COUNT)>
      metric_bindings_{};
  std::array<ReportTarget, MAX_REPORT_TARGETS> report_targets_{};

  usb_transfer_t* control_transfer_{nullptr};
  HIDInterfaceContext hid_interface_{};
  ReportTarget active_report_target_{};

  size_t field_count_{0};
  size_t report_layout_count_{0};
  size_t report_target_count_{0};
  size_t next_poll_target_index_{0};
  size_t report_descriptor_length_{0};
  uint32_t update_interval_ms_{30000};

  std::atomic<bool> interrupt_in_active_{false};
  std::atomic<bool> control_transfer_active_{false};
  std::atomic<bool> report_descriptor_ready_{false};

  bool explore_{false};
  bool normalize_apc_pages_{true};
  bool has_forced_interface_number_{false};
  bool has_forced_interrupt_in_endpoint_{false};
  bool poll_requested_{false};
  bool runtime_started_{false};
  uint8_t forced_interface_number_{0};
  uint8_t forced_interrupt_in_endpoint_{0};
  uint8_t report_descriptor_index_{0};
  ControlAction control_action_{ControlAction::NONE};

  sensor::Sensor* battery_level_sensor_{nullptr};
  sensor::Sensor* runtime_sensor_{nullptr};
  sensor::Sensor* load_percent_sensor_{nullptr};
  sensor::Sensor* input_voltage_sensor_{nullptr};
  sensor::Sensor* output_voltage_sensor_{nullptr};
  sensor::Sensor* battery_voltage_sensor_{nullptr};
};

}  // namespace esphome::usb_hid_ups

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 ||
        // USE_ESP32_VARIANT_ESP32S3