// add includes

namespace commands {
// For each command have an encoder an a struct with the binary representation
// for each of the fields

enum class ChargeDischargeStatus : uint8_t {
  //...
}

struct charge_discharge_management_info {
  uint16_t recommended_charge_voltage_upper_limit_milli_volt;
  uint16_t recommended_discharge_voltage_lower_limit_milli_volt;
  uint16_t max_charge_current_deci_ampere;
  uint16_t max_discharge_current_deci_ampere;
  ChargeDischargeStatus charge_discharge_status;
}

// info_output is encoded in regular binary, that is then encoded
// as hex by encode_frame
void encode_charge_discharge_management_info(
    const charge_discharge_management_info& data,
    etl::span<uint8_t> info_output) {
  // ...
}

}  // namespace commands