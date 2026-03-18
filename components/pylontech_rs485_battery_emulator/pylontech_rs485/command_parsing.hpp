#pragma once

#include <etl/span.h>
#include <etl/vector.h>
#include <etl/optional.h>

#include <cstddef>
#include <cstdint>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

#define READ_BYTE(value, at) \
  static_cast<uint8_t>(((value) >> (8 * (at))) & 0xFFU)

namespace pylontech_rs485 {

namespace commands {

constexpr size_t CHARGE_DISCHARGE_MANAGEMENT_INFO_RESPONSE_INFO_SIZE = 9;
constexpr size_t MAX_CELL_COUNT = 16;
constexpr size_t MAX_CELL_TEMPERATURE_PROBE_COUNT = 8;

struct charge_discharge_status_flags {
  bool charge_enable;
  bool discharge_enable;
  bool force_charge_1;
  bool force_charge_2;
  bool full_charge_request;
};

struct charge_discharge_management_info_response {
  uint8_t pack_address;
  uint16_t charge_voltage_upper_limit_millivolt;
  uint16_t discharge_voltage_lower_limit_millivolt;
  uint16_t max_charge_current_deci_ampere;
  uint16_t max_discharge_current_deci_ampere;
  charge_discharge_status_flags status_flags;
};

struct analog_values_response {
  etl::vector<uint16_t, MAX_CELL_COUNT> cell_voltages_millivolt;
  uint16_t bms_temperature_deci_kelvin;
  etl::vector<uint16_t, MAX_CELL_TEMPERATURE_PROBE_COUNT>
      cell_temperatures_deci_kelvin;
  int16_t current_deci_ampere;
  uint16_t voltage_millivolt;
  uint32_t remaining_capacity_milliampere_hour;
  uint32_t total_capacity_milliampere_hour;
  uint16_t cycle_count;
};

inline bool parse_protocol_version_request(etl::span<const uint8_t> info) {
  return info.empty();
}

inline void write_u16_be(etl::span<uint8_t, 2> output, uint16_t value) {
  output[0] = READ_BYTE(value, 1);
  output[1] = READ_BYTE(value, 0);
}

inline void write_i16_be(etl::span<uint8_t, 2> output, int16_t value) {
  write_u16_be(output, static_cast<uint16_t>(value));
}

[[nodiscard]] inline uint8_t write_u24_be(etl::span<uint8_t, 3> output,
                                          uint32_t value) {
  output[0] = READ_BYTE(value, 2);
  output[1] = READ_BYTE(value, 1);
  output[2] = READ_BYTE(value, 0);
  return READ_BYTE(value, 3);
}

[[nodiscard]] inline etl::optional<uint8_t> pop_addr_from_info(
    etl::span<const uint8_t>& info) {
  if (info.size() < 1) {
    return {};
  }
  uint8_t addr = info.front();
  info = etl::span<const uint8_t>(info.begin() + 1, info.end());
  return addr;
}

[[nodiscard]] inline bool encode_charge_discharge_management_info(
    const charge_discharge_management_info_response& response,
    etl::span<uint8_t> output_without_addr) {
  if (output_without_addr.size() !=
      CHARGE_DISCHARGE_MANAGEMENT_INFO_RESPONSE_INFO_SIZE) {
    return false;
  }

  write_u16_be(output_without_addr.subspan<0, 2>(),
               response.charge_voltage_upper_limit_millivolt);
  write_u16_be(output_without_addr.subspan<2, 2>(),
               response.discharge_voltage_lower_limit_millivolt);
  write_u16_be(output_without_addr.subspan<4, 2>(),
               response.max_charge_current_deci_ampere);
  write_u16_be(output_without_addr.subspan<6, 2>(),
               response.max_discharge_current_deci_ampere);
  const auto& flags = response.status_flags;
  output_without_addr[8] =
      (BIT(7) * flags.charge_enable) | (BIT(6) * flags.discharge_enable) |
      (BIT(5) * flags.force_charge_1) | (BIT(4) * flags.force_charge_2) |
      (BIT(3) * flags.full_charge_request);
  return true;
}

inline constexpr bool uses_extended_capacity_fields(const analog_values_response& response) {
  // Check for off by one errors here
  return (response.remaining_capacity_milliampere_hour > 0xFFFFU) ||
         (response.total_capacity_milliampere_hour > 0xFFFFU);
}

inline size_t analog_values_response_size(const analog_values_response& response,
                                   etl::span<uint8_t> output) {
  return (1 /*number of cells*/ + response.cell_voltages_millivolt.size() * 2 +
          1 /*number of probes*/ + 2 /*bms temperature*/ +
          response.cell_temperatures_deci_kelvin.size() * 2 +
          2 /*module current*/ + 2 /*voltage*/ + 2 /*remaining capacity*/ +
          1 /* user defined item count (?) */ + 2 /* total capcity */ +
          2 /* cycle count */ +
          (3 /*remaining capacity*/ + 3 /*total capacity*/) *
              uses_extended_capacity_fields(response));
}

enum class EncodeAnalogValuesResponseError {
  OutputBufferSizeMismatch,
  CapacityOverflow,
};

[[nodiscard]] inline etl::optional<EncodeAnalogValuesResponseError>
encode_analog_values_response(
    const analog_values_response& response,
    etl::span<uint8_t> output_without_info_flag_or_pack_addr) {
  if (analog_values_response_size(response,
                                  output_without_info_flag_or_pack_addr) !=
      output_without_info_flag_or_pack_addr.size()) {
    return EncodeAnalogValuesResponseError::OutputBufferSizeMismatch;
  }
  const auto encode_list_without_size =
      []<size_t N>(const etl::vector<uint16_t, N>& input,
                   const etl::span<uint8_t> output, size_t offset = 1) {
        for (size_t i = 0; i < input.size(); ++i) {
          write_u16_be(
              etl::span<uint8_t, 2>(output.begin() + i * 2 + offset, size_t(2)),
              input[i]);
        }
        return etl::span<uint8_t>(output.begin() + input.size() * 2 + offset,
                                  output.end());
      };

  output_without_info_flag_or_pack_addr[0] =
      response.cell_voltages_millivolt.size();
  const auto output_after_voltages = encode_list_without_size(
      response.cell_voltages_millivolt, output_without_info_flag_or_pack_addr);

  output_after_voltages[0] = response.cell_temperatures_deci_kelvin.size() + 1;
  write_u16_be(output_after_voltages.subspan<1, 2>(),
               response.bms_temperature_deci_kelvin);
  const auto output_after_temps = encode_list_without_size(
      response.cell_temperatures_deci_kelvin, output_after_voltages, 3);

  write_i16_be(output_after_temps.subspan<0, 2>(),
               response.current_deci_ampere);
  write_u16_be(output_after_temps.subspan<2, 2>(), response.voltage_millivolt);
  bool uses_extended_cap = uses_extended_capacity_fields(response);
  write_u16_be(output_after_temps.subspan<4, 2>(),
               uses_extended_cap
                   ? 0xFFFFU
                   : static_cast<uint16_t>(
                         response.remaining_capacity_milliampere_hour));
  // "user defined item count", set to 4 if using extended capacity, otherwise 2
  output_after_temps[6] = uses_extended_cap ? 4 : 2;
  write_u16_be(
      output_after_temps.subspan<7, 2>(),
      uses_extended_cap
          ? 0xFFFFU
          : static_cast<uint16_t>(response.total_capacity_milliampere_hour));
  write_u16_be(output_after_temps.subspan<9, 2>(), response.cycle_count);
  if (!uses_extended_cap) {
    return {};
  }
  const auto cap_remainder =
      write_u24_be(output_after_temps.subspan<11, 3>(),
                   response.remaining_capacity_milliampere_hour);
  const auto tot_cap_remainder =
      write_u24_be(output_after_temps.subspan<14, 3>(),
                   response.total_capacity_milliampere_hour);
  if (cap_remainder || tot_cap_remainder) {
    return EncodeAnalogValuesResponseError::CapacityOverflow;
  }
  return {};
}

}  // namespace commands

}  // namespace pylontech_rs485
