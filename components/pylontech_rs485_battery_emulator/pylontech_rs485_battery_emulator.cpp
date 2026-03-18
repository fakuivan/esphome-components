#include "pylontech_rs485_battery_emulator.h"

#include "esphome/core/log.h"

namespace esphome::pylontech_rs485_battery_emulator {

static const char *const TAG = "pylontech_rs485_battery_emulator";

optional<pylontech_rs485::commands::analog_values_response>
PylontechRS485BatteryPack::build_analog_values_response() {
  if (!this->cell_voltages_.has_value() || !this->bms_temperature_.has_value() ||
      !this->cell_temperatures_.has_value() || !this->current_.has_value() ||
      !this->voltage_.has_value() || !this->module_capacity_.has_value() ||
      !this->cycles_.has_value()) {
    // log error the component is configured to respond to this packet but
    // only a few functions are defined.
    return {};
  }

  const auto maybe_cell_voltages = this->cell_voltages_();
  const auto maybe_bms_temperature = this->bms_temperature_();
  const auto maybe_cell_temperatures = this->cell_temperatures_();
  const auto maybe_current = this->current_();
  const auto maybe_voltage = this->voltage_();
  const auto maybe_total_capacity = this->module_capacity_();
  const auto maybe_cycles = this->cycles_();
  // This default seems fishy, idk if we want that
  const auto maybe_remaining_capacity = this->remaining_capacity_.has_value()
                                            ? this->remaining_capacity_()
                                            : maybe_total_capacity;

  if (!maybe_cell_voltages || !maybe_bms_temperature || !maybe_cell_temperatures ||
      !maybe_current || !maybe_voltage || !maybe_total_capacity ||
      !maybe_remaining_capacity || !maybe_cycles) {
    // log error here
    return {};
  }

  if (maybe_cell_voltages->empty()) {
    // here too
    return {};
  }

  pylontech_rs485::commands::analog_values_response response;
  response.bms_temperature_deci_kelvin = *maybe_bms_temperature;
  response.current_deci_ampere = *maybe_current;
  response.voltage_millivolt = *maybe_voltage;
  response.remaining_capacity_milliampere_hour = *maybe_remaining_capacity;
  response.total_capacity_milliampere_hour = *maybe_total_capacity;
  response.cycle_count = *maybe_cycles;

  for (const auto cell_voltage : *maybe_cell_voltages) {
    response.cell_voltages_millivolt.push_back(cell_voltage);
  }

  for (const auto temperature : *maybe_cell_temperatures) {
    response.cell_temperatures_deci_kelvin.push_back(temperature);
  }

  return response;
}

optional<pylontech_rs485::commands::charge_discharge_management_info_response>
PylontechRS485BatteryPack::build_charge_discharge_management_info_response() {
  if (!this->charge_voltage_upper_limit_.has_value() ||
      !this->discharge_voltage_lower_limit_.has_value() ||
      !this->max_charge_current_.has_value() ||
      !this->max_discharge_current_.has_value() || !this->charge_enable_.has_value() ||
      !this->discharge_enable_.has_value() || !this->force_charge_1_.has_value() ||
      !this->force_charge_2_.has_value() || !this->full_charge_request_.has_value()) {
    return {};
  }

  const auto maybe_charge_voltage = this->charge_voltage_upper_limit_();
  const auto maybe_discharge_voltage = this->discharge_voltage_lower_limit_();
  const auto maybe_max_charge_current = this->max_charge_current_();
  const auto maybe_max_discharge_current = this->max_discharge_current_();
  const auto maybe_charge_enable = this->charge_enable_();
  const auto maybe_discharge_enable = this->discharge_enable_();
  const auto maybe_force_charge_1 = this->force_charge_1_();
  const auto maybe_force_charge_2 = this->force_charge_2_();
  const auto maybe_full_charge_request = this->full_charge_request_();

  if (!maybe_charge_voltage || !maybe_discharge_voltage || !maybe_max_charge_current ||
      !maybe_max_discharge_current || !maybe_charge_enable || !maybe_discharge_enable ||
      !maybe_force_charge_1 || !maybe_force_charge_2 || !maybe_full_charge_request) {
    return {};
  }

  pylontech_rs485::commands::charge_discharge_management_info_response response;
  response.pack_address = this->get_address();
  response.charge_voltage_upper_limit_millivolt = *maybe_charge_voltage;
  response.discharge_voltage_lower_limit_millivolt = *maybe_discharge_voltage;
  response.max_charge_current_deci_ampere = *maybe_max_charge_current;
  response.max_discharge_current_deci_ampere = *maybe_max_discharge_current;
  response.status_flags.charge_enable = *maybe_charge_enable;
  response.status_flags.discharge_enable = *maybe_discharge_enable;
  response.status_flags.force_charge_1 = *maybe_force_charge_1;
  response.status_flags.force_charge_2 = *maybe_force_charge_2;
  response.status_flags.full_charge_request = *maybe_full_charge_request;
  return response;
}

float PylontechRS485BatteryEmulator::get_setup_priority() const {
  return setup_priority::LATE;
}

void PylontechRS485BatteryEmulator::dump_config() {
  ESP_LOGCONFIG(TAG, "Pylontech RS485 Battery Emulator:");
  ESP_LOGCONFIG(TAG, "  Protocol version: 0x%02X", this->protocol_version_);
  ESP_LOGCONFIG(TAG, "  Configured packs: %u", static_cast<unsigned>(this->packs_.size()));
  for (const auto *pack : this->packs_) {
    ESP_LOGCONFIG(TAG, "  Pack %u -> address 0x%02X", pack->get_battery_number(),
                  pack->get_address());
  }
}

void PylontechRS485BatteryEmulator::loop() {
  while (this->available()) {
    uint8_t byte = 0;
    if (!this->read_byte(&byte)) {
      break;
    }

    if (byte == pylontech_rs485::packet_parsing::SOI) {
      this->rx_buffer_.clear();
      this->rx_buffer_overflow_ = false;
      this->rx_buffer_.push_back(byte);
      continue;
    }

    if (this->rx_buffer_.empty()) {
      continue;
    }

    if (this->rx_buffer_.size() >= MAX_REQUEST_FRAME_SIZE) {
      this->rx_buffer_overflow_ = true;
      if (byte == pylontech_rs485::packet_parsing::EOI) {
        this->rx_buffer_.clear();
        this->rx_buffer_overflow_ = false;
      }
      continue;
    }

    this->rx_buffer_.push_back(byte);
    if (byte == pylontech_rs485::packet_parsing::EOI) {
      if (!this->rx_buffer_overflow_) {
        this->handle_frame_();
      }
      this->rx_buffer_.clear();
      this->rx_buffer_overflow_ = false;
    }
  }
}

PylontechRS485BatteryPack *PylontechRS485BatteryEmulator::find_pack_(
    uint8_t address) {
  for (auto *pack : this->packs_) {
    if (pack->get_address() == address) {
      return pack;
    }
  }
  return nullptr;
}

void PylontechRS485BatteryEmulator::send_status_response_(uint8_t address,
                                                          pylontech_rs485::packet_parsing::Cid2 status) {
  std::array<uint8_t, 1> empty_info{};
  this->send_info_response_(address, status,
                            etl::span<uint8_t>(empty_info.data(), size_t(0)));
}

void PylontechRS485BatteryEmulator::send_info_response_(
  uint8_t address, pylontech_rs485::packet_parsing::Cid2 response_code,
    etl::span<uint8_t> info) {
  const auto maybe_packet_size =
      pylontech_rs485::packet_parsing::packet_size_from_info_size(info.size());
  if (!maybe_packet_size) {
    ESP_LOGW(TAG, "Failed to encode response frame, info too large");
    return;
  }

  std::array<uint8_t, MAX_RESPONSE_FRAME_SIZE> frame_buffer{};
  pylontech_rs485::packet_parsing::frame frame{
      this->protocol_version_,
      address,
      pylontech_rs485::packet_parsing::Cid1::BatteryData,
      response_code,
      info,
  };
  const auto maybe_error = pylontech_rs485::packet_parsing::encode_frame(
      etl::span<uint8_t>(frame_buffer.data(), *maybe_packet_size), frame);
  if (maybe_error) {
    ESP_LOGW(TAG, "Failed to encode response frame, error=%u",
             static_cast<unsigned>(*maybe_error));
    return;
  }

  this->write_array(frame_buffer.data(), *maybe_packet_size);
}

void PylontechRS485BatteryEmulator::handle_frame_() {
  const auto maybe_info_size =
      pylontech_rs485::packet_parsing::info_size_from_packet_size(
          this->rx_buffer_.size());
  if (!maybe_info_size.has_value()) {
    ESP_LOGV(TAG, "Ignoring frame with invalid packet size");
    return;
  }
  if (maybe_info_size.value() > MAX_REQUEST_INFO_SIZE) {
    ESP_LOGV(TAG, "Ignoring frame with unsupported info size %u",
             static_cast<unsigned>(maybe_info_size.value()));
    return;
  }

  const auto decoded = pylontech_rs485::packet_parsing::parse_frame(
      etl::span<const uint8_t>(this->rx_buffer_.data(), this->rx_buffer_.size()),
      etl::span<uint8_t>(this->request_info_buffer_.data(), maybe_info_size.value()));
  if (!decoded.has_value()) {
    ESP_LOGV(TAG, "Ignoring invalid frame, parse error=%u",
             static_cast<unsigned>(decoded.error()));
    return;
  }

  const auto &packet = decoded.value();
  if (!packet.cid1.has_value() ||
      packet.cid1.value() != pylontech_rs485::packet_parsing::Cid1::BatteryData) {
    return;
  }
  if (!packet.cid2.has_value()) {
    this->send_status_response_(
        packet.address, pylontech_rs485::packet_parsing::Cid2::InvalidCid2);
    return;
  }

  switch (packet.cid2.value()) {
    case pylontech_rs485::packet_parsing::Cid2::GetProtocolVersion: {
      if (!pylontech_rs485::commands::parse_protocol_version_request(packet.info)) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommandFormatError);
        return;
      }
      this->send_status_response_(packet.address,
                                  pylontech_rs485::packet_parsing::Cid2::Normal);
      return;
    }
    case pylontech_rs485::packet_parsing::Cid2::GetAnalogValues: {
      auto request_info = etl::span<const uint8_t>(packet.info.begin(), packet.info.end());
      const auto request_address = pylontech_rs485::commands::pop_addr_from_info(request_info);
      if (!request_address || !request_info.empty()) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommandFormatError);
        return;
      }
      if (*request_address != packet.address) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::AddressError);
        return;
      }

      auto *pack = this->find_pack_(packet.address);
      if (pack == nullptr) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::AddressError);
        return;
      }

      const auto response = pack->build_analog_values_response();
      if (!response.has_value()) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommunicationError);
        return;
      }

        std::array<uint8_t, MAX_ANALOG_VALUES_RESPONSE_INFO_SIZE> response_info{};
        response_info[0] = 0;
        response_info[1] = packet.address;
      const auto encoded = pylontech_rs485::commands::encode_analog_values_response(
          *response, etl::span<uint8_t>(response_info.data() + 2, response_info.size() - 2));
      if (encoded.has_value()) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommunicationError);
        return;
      }
        const auto response_info_size =
          pylontech_rs485::commands::analog_values_response_size(
            *response,
            etl::span<uint8_t>(response_info.data() + 2, response_info.size() - 2)) +
          2;
      this->send_info_response_(packet.address,
                                pylontech_rs485::packet_parsing::Cid2::Normal,
                                etl::span<uint8_t>(
                      response_info.data(), response_info_size));
      return;
    }
    case pylontech_rs485::packet_parsing::Cid2::GetChargeDischargeManagementInfo: {
        auto request_info = etl::span<const uint8_t>(packet.info.begin(), packet.info.end());
        const auto request_address = pylontech_rs485::commands::pop_addr_from_info(request_info);
        if (!request_address || !request_info.empty()) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommandFormatError);
        return;
      }
      if (*request_address != packet.address) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::AddressError);
        return;
      }

      auto *pack = this->find_pack_(packet.address);
      if (pack == nullptr) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::AddressError);
        return;
      }

      const auto response = pack->build_charge_discharge_management_info_response();
      if (!response.has_value()) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommunicationError);
        return;
      }

        std::array<uint8_t, MAX_CHARGE_DISCHARGE_MANAGEMENT_RESPONSE_INFO_SIZE> response_info{};
        response_info[0] = packet.address;
      const bool encoded = pylontech_rs485::commands::encode_charge_discharge_management_info(
          *response,
          etl::span<uint8_t>(response_info.data() + 1, response_info.size() - 1));
      if (!encoded) {
        this->send_status_response_(packet.address,
                                    pylontech_rs485::packet_parsing::Cid2::CommunicationError);
        return;
      }
      this->send_info_response_(packet.address,
                                pylontech_rs485::packet_parsing::Cid2::Normal,
                                etl::span<uint8_t>(response_info.data(), response_info.size()));
      return;
    }
    default:
      this->send_status_response_(packet.address,
                                  pylontech_rs485::packet_parsing::Cid2::InvalidCid2);
      return;
  }
}

}  // namespace esphome::pylontech_rs485_battery_emulator