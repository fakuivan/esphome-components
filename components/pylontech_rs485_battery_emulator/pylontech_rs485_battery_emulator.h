#pragma once

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"
#include "esphome/core/template_lambda.h"
#include "pylontech_rs485_transcode.hpp"

#include <array>
#include <string>

namespace esphome::pylontech_rs485_battery_emulator {

static constexpr size_t MAX_PACK_COUNT = 15;
static constexpr size_t MAX_REQUEST_INFO_SIZE = 1;
static constexpr size_t MAX_REQUEST_FRAME_SIZE =
  pylontech_rs485::packet_parsing::MIN_FRAME_SIZE +
  (MAX_REQUEST_INFO_SIZE * pylontech_rs485::packet_parsing::ASCII_BYTES_PER_ENCODED_BYTE);
static constexpr size_t MAX_ANALOG_VALUES_RESPONSE_INFO_SIZE = 71;
static constexpr size_t MAX_CHARGE_DISCHARGE_MANAGEMENT_RESPONSE_INFO_SIZE = 10;
static constexpr size_t MAX_RESPONSE_INFO_SIZE = MAX_ANALOG_VALUES_RESPONSE_INFO_SIZE;
static constexpr size_t MAX_RESPONSE_FRAME_SIZE =
  pylontech_rs485::packet_parsing::MIN_FRAME_SIZE +
  (MAX_RESPONSE_INFO_SIZE * pylontech_rs485::packet_parsing::ASCII_BYTES_PER_ENCODED_BYTE);

class PylontechRS485BatteryPack {
 public:
  explicit PylontechRS485BatteryPack(uint8_t battery_number)
      : battery_number_(battery_number) {}

  uint8_t get_battery_number() const { return this->battery_number_; }
  uint8_t get_address() const { return static_cast<uint8_t>(this->battery_number_ + 1U); }

  template<typename F> void set_cell_voltages(F value) { this->cell_voltages_.set(value); }
  template<typename F> void set_bms_temperature(F value) { this->bms_temperature_.set(value); }
  template<typename F> void set_cell_temperatures(F value) { this->cell_temperatures_.set(value); }
  template<typename F> void set_current(F value) { this->current_.set(value); }
  template<typename F> void set_voltage(F value) { this->voltage_.set(value); }
  template<typename F> void set_remaining_capacity(F value) { this->remaining_capacity_.set(value); }
  template<typename F> void set_module_capacity(F value) { this->module_capacity_.set(value); }
  template<typename F> void set_cycles(F value) { this->cycles_.set(value); }
  template<typename F> void set_charge_voltage_upper_limit(F value) {
    this->charge_voltage_upper_limit_.set(value);
  }
  template<typename F> void set_discharge_voltage_lower_limit(F value) {
    this->discharge_voltage_lower_limit_.set(value);
  }
  template<typename F> void set_max_charge_current(F value) { this->max_charge_current_.set(value); }
  template<typename F> void set_max_discharge_current(F value) {
    this->max_discharge_current_.set(value);
  }
  template<typename F> void set_charge_enable(F value) { this->charge_enable_.set(value); }
  template<typename F> void set_discharge_enable(F value) { this->discharge_enable_.set(value); }
  template<typename F> void set_force_charge_1(F value) { this->force_charge_1_.set(value); }
  template<typename F> void set_force_charge_2(F value) { this->force_charge_2_.set(value); }
  template<typename F> void set_full_charge_request(F value) { this->full_charge_request_.set(value); }

  optional<pylontech_rs485::commands::analog_values_response> build_analog_values_response();
  optional<pylontech_rs485::commands::charge_discharge_management_info_response>
  build_charge_discharge_management_info_response();

 protected:
  uint8_t battery_number_;
  TemplateLambda<StaticVector<uint16_t, ::pylontech_rs485::commands::MAX_CELL_COUNT>>
    cell_voltages_;
  TemplateLambda<uint16_t> bms_temperature_;
  TemplateLambda<StaticVector<uint16_t, ::pylontech_rs485::commands::MAX_CELL_TEMPERATURE_PROBE_COUNT>>
    cell_temperatures_;
  TemplateLambda<int16_t> current_;
  TemplateLambda<uint16_t> voltage_;
  TemplateLambda<uint32_t> remaining_capacity_;
  TemplateLambda<uint32_t> module_capacity_;
  TemplateLambda<uint16_t> cycles_;
  TemplateLambda<uint16_t> charge_voltage_upper_limit_;
  TemplateLambda<uint16_t> discharge_voltage_lower_limit_;
  TemplateLambda<uint16_t> max_charge_current_;
  TemplateLambda<uint16_t> max_discharge_current_;
  TemplateLambda<bool> charge_enable_;
  TemplateLambda<bool> discharge_enable_;
  TemplateLambda<bool> force_charge_1_;
  TemplateLambda<bool> force_charge_2_;
  TemplateLambda<bool> full_charge_request_;
};

class PylontechRS485BatteryEmulator : public Component, public uart::UARTDevice {
 public:
  void setup() override {}
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void add_pack(PylontechRS485BatteryPack *pack) {
    if (this->packs_.size() < MAX_PACK_COUNT) {
      this->packs_.push_back(pack);
    }
  }
  void set_protocol_version(uint8_t protocol_version) { this->protocol_version_ = protocol_version; }

 protected:
  void handle_frame_();
  PylontechRS485BatteryPack *find_pack_(uint8_t address);
  void send_status_response_(uint8_t address,
                             pylontech_rs485::packet_parsing::Cid2 status);
  void send_info_response_(uint8_t address,
                           pylontech_rs485::packet_parsing::Cid2 response_code,
                           etl::span<uint8_t> info);

  StaticVector<uint8_t, MAX_REQUEST_FRAME_SIZE> rx_buffer_{};
  bool rx_buffer_overflow_{false};
  std::array<uint8_t, MAX_REQUEST_INFO_SIZE> request_info_buffer_{};
  uint8_t protocol_version_{0x20};
  StaticVector<PylontechRS485BatteryPack *, MAX_PACK_COUNT> packs_{};
};

}  // namespace esphome::pylontech_rs485_battery_emulator