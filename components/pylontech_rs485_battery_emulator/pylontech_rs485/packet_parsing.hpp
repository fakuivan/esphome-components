#pragma once

#include <etl/algorithm.h>
#include <etl/expected.h>
#include <etl/optional.h>
#include <etl/span.h>
#include <etl/vector.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace pylontech_rs485 {

namespace packet_parsing {

constexpr uint8_t SOI = '~';
constexpr uint8_t EOI = '\r';
constexpr size_t ASCII_BYTES_PER_ENCODED_BYTE = 2;
constexpr size_t MIN_FRAME_SIZE =
    1 /* SOI */ +
    (ASCII_BYTES_PER_ENCODED_BYTE * 4 /* VER, ADR, CID1, CID2 */) +
    ((2 * ASCII_BYTES_PER_ENCODED_BYTE) * 2 /* LENGTH, CHKSUM */) + 1 /* EOI */;
constexpr size_t MAX_ASCII_INFO_LENGTH =
    (1U << 12U) - 1U /* 12-bit LENID field, max value 0x0FFF */;
constexpr size_t MAX_BINARY_INFO_LENGTH =
    MAX_ASCII_INFO_LENGTH / ASCII_BYTES_PER_ENCODED_BYTE;

constexpr etl::optional<size_t> packet_size_from_info_size(size_t info_size) {
  if (info_size > MAX_BINARY_INFO_LENGTH) {
    return etl::nullopt;
  }
  return MIN_FRAME_SIZE + (info_size * ASCII_BYTES_PER_ENCODED_BYTE);
}

enum class PacketSizeError { PacketTooShort, PacketLengthInvalid };

etl::expected<size_t, PacketSizeError> info_size_from_packet_size(
    size_t packet_size) {
  if (packet_size < MIN_FRAME_SIZE) {
    return etl::unexpected{PacketSizeError::PacketTooShort};
  }

  const size_t ascii_info_length = packet_size - MIN_FRAME_SIZE;
  if ((ascii_info_length > MAX_ASCII_INFO_LENGTH) ||
      ((ascii_info_length % ASCII_BYTES_PER_ENCODED_BYTE) != 0U)) {
    return etl::unexpected{PacketSizeError::PacketLengthInvalid};
  }

  return ascii_info_length / ASCII_BYTES_PER_ENCODED_BYTE;
}

enum class Cid1 : uint8_t {
  BatteryData = 0x46,
};

enum class Cid2 : uint8_t {
  // Response codes
  Normal = 0x00,
  VersionError = 0x01,
  ChecksumError = 0x02,
  LengthChecksumError = 0x03,
  InvalidCid2 = 0x04,
  CommandFormatError = 0x05,
  InvalidData = 0x06,
  AddressError = 0x90,
  CommunicationError = 0x91,

  // Command codes
  GetAnalogValues = 0x42,
  GetAlarmInfo = 0x44,
  GetSystemParameters = 0x47,
  GetProtocolVersion = 0x4F,
  GetManufacturerInfo = 0x51,
  GetBatterySystemBasicInformation = 0x60,
  GetBatterySystemAnalogData = 0x61,
  GetBatterySystemAlarmInformation = 0x62,
  GetBatterySystemChargeDischargeManagementInformation = 0x63,
  ControlBatterySystemShutdown = 0x64,
  GetChargeDischargeManagementInfo = 0x92,
  GetModuleSerialNumber = 0x93,
  SetChargeDischargeManagementInfo = 0x94,
  TurnOffModule = 0x95,
  GetSoftwareVersion = 0x96,
};

etl::expected<Cid1, uint8_t> try_parse_cid1(uint8_t cid1) {
  switch (cid1) {
    case static_cast<uint8_t>(Cid1::BatteryData):
      return Cid1::BatteryData;
    default:
      return etl::unexpected{cid1};
  }
}

#ifdef PYLONTECH_RS485_CID2_CASE
#error PYLONTECH_RS485_CID2_CASE is already defined
#endif

#define PYLONTECH_RS485_CID2_CASE(value)  \
  case static_cast<uint8_t>(Cid2::value): \
    return Cid2::value;

etl::expected<Cid2, uint8_t> try_parse_cid2(uint8_t cid2) {
  switch (cid2) {
    PYLONTECH_RS485_CID2_CASE(Normal)
    PYLONTECH_RS485_CID2_CASE(VersionError)
    PYLONTECH_RS485_CID2_CASE(ChecksumError)
    PYLONTECH_RS485_CID2_CASE(LengthChecksumError)
    PYLONTECH_RS485_CID2_CASE(InvalidCid2)
    PYLONTECH_RS485_CID2_CASE(CommandFormatError)
    PYLONTECH_RS485_CID2_CASE(InvalidData)
    PYLONTECH_RS485_CID2_CASE(AddressError)
    PYLONTECH_RS485_CID2_CASE(CommunicationError)
    PYLONTECH_RS485_CID2_CASE(GetAnalogValues)
    PYLONTECH_RS485_CID2_CASE(GetAlarmInfo)
    PYLONTECH_RS485_CID2_CASE(GetSystemParameters)
    PYLONTECH_RS485_CID2_CASE(GetProtocolVersion)
    PYLONTECH_RS485_CID2_CASE(GetManufacturerInfo)
    PYLONTECH_RS485_CID2_CASE(GetBatterySystemBasicInformation)
    PYLONTECH_RS485_CID2_CASE(GetBatterySystemAnalogData)
    PYLONTECH_RS485_CID2_CASE(GetBatterySystemAlarmInformation)
    PYLONTECH_RS485_CID2_CASE(
        GetBatterySystemChargeDischargeManagementInformation)
    PYLONTECH_RS485_CID2_CASE(ControlBatterySystemShutdown)
    PYLONTECH_RS485_CID2_CASE(GetChargeDischargeManagementInfo)
    PYLONTECH_RS485_CID2_CASE(GetModuleSerialNumber)
    PYLONTECH_RS485_CID2_CASE(SetChargeDischargeManagementInfo)
    PYLONTECH_RS485_CID2_CASE(TurnOffModule)
    PYLONTECH_RS485_CID2_CASE(GetSoftwareVersion)
    default:
      return etl::unexpected{cid2};
  }
}

#undef PYLONTECH_RS485_CID2_CASE

uint8_t cid1_to_u8(etl::expected<Cid1, uint8_t> cid1) {
  return cid1 ? static_cast<uint8_t>(*cid1) : cid1.error();
}

uint8_t cid2_to_u8(etl::expected<Cid2, uint8_t> cid2) {
  return cid2 ? static_cast<uint8_t>(*cid2) : cid2.error();
}

struct frame {
  uint8_t version;
  uint8_t address;
  etl::expected<Cid1, uint8_t> cid1;
  etl::expected<Cid2, uint8_t> cid2;
  etl::span<uint8_t> info;
};

etl::optional<uint8_t> decode_hex_byte(uint8_t high, uint8_t low) {
  const auto decode_hex = [](uint8_t hex) -> int8_t {
    if ((hex >= '0') && (hex <= '9')) {
      return static_cast<int8_t>(hex - '0');
    }

    if ((hex >= 'A') && (hex <= 'F')) {
      return static_cast<int8_t>(10U + (hex - 'A'));
    }

    if ((hex >= 'a') && (hex <= 'f')) {
      return static_cast<int8_t>(10U + (hex - 'a'));
    }
    return -1;
  };
  const auto low_nibble = decode_hex(low);
  const auto high_nibble = decode_hex(high);
  if (low_nibble < 0 || high_nibble < 0) {
    return etl::nullopt;
  }
  return static_cast<uint8_t>((high_nibble << 4) | low_nibble);
}

uint16_t make_u16_be(uint8_t hi, uint8_t lo) {
  return (static_cast<uint16_t>(hi) << 8) | static_cast<uint16_t>(lo);
}

struct length_checksum_split {
  uint8_t checksum;
  uint16_t length;
};

length_checksum_split split_checksum(uint16_t len_checksum) {
  return {static_cast<uint8_t>((len_checksum >> (4 * 3)) & 0x0f),
          static_cast<uint16_t>(len_checksum & 0x0fff)};
}

bool length_checksum_split_valid(length_checksum_split split) {
  const uint16_t length_masked = (split.length & 0x0fff);
  const uint8_t checksum_masked = (split.checksum & 0x0f);
  return length_masked == split.length && checksum_masked == split.checksum;
}

etl::optional<uint16_t> combine_checksum(length_checksum_split split) {
  if (!length_checksum_split_valid(split)) {
    return etl::nullopt;
  }
  return (static_cast<uint16_t>(split.checksum) << (4 * 3)) | split.length;
}

etl::optional<length_checksum_split> add_length_checksum(
    length_checksum_split split) {
  if (!length_checksum_split_valid(split)) {
    return etl::nullopt;
  }
  const uint8_t nibble_sum = static_cast<uint8_t>(((split.length >> 8) & 0x0f) +
                                                  ((split.length >> 4) & 0x0f) +
                                                  (split.length & 0x0f));

  split.checksum = static_cast<uint8_t>((0U - nibble_sum) & 0x0FU);
  return split;
}

enum class HexArrayParsingError {
  InputOutputSizeMismatch,
  BadInputCharacters,
};

[[nodiscard]] etl::optional<HexArrayParsingError> parse_hex_string(
    etl::span<const uint8_t> input, etl::span<uint8_t> output) {
  if (input.size() != (output.size() * ASCII_BYTES_PER_ENCODED_BYTE)) {
    return HexArrayParsingError::InputOutputSizeMismatch;
  }
  for (size_t i = 0; i < output.size(); ++i) {
    const auto maybe_byte = decode_hex_byte(input[i * 2], input[i * 2 + 1]);
    if (!maybe_byte) {
      return HexArrayParsingError::BadInputCharacters;
    }
    output[i] = *maybe_byte;
  }
  return etl::nullopt;
}

// rename this to ParsingError
enum class HeaderParsingError {
  FrameTooShort,
  FrameLengthInvalid,
  InfoBufferSizeMismatch,
  IncorrectStartByte,
  IncorrectEndByte,
  InvalidCharactersOnHexString,
  InvalidInfoLengthChecksum,
  InvalidInfoLength,
  InvalidChecksum,
};

constexpr uint16_t hex_string_checksum(etl::span<const uint8_t> hex_string) {
  return (
      (~etl::accumulate(hex_string.begin(), hex_string.end(), uint16_t(0))) +
      1U);
}

etl::expected<frame, HeaderParsingError> parse_frame(
    etl::span<const uint8_t> frame, etl::span<uint8_t> info_buffer) {
  const auto maybe_size = info_size_from_packet_size(frame.size());
  if (!maybe_size) {
    const auto error = maybe_size.error();
    if (error == PacketSizeError::PacketTooShort) {
      return etl::unexpected{HeaderParsingError::FrameTooShort};
    }
    assert(error == PacketSizeError::PacketLengthInvalid);
    return etl::unexpected{HeaderParsingError::FrameLengthInvalid};
  }
  const auto expected_info_size = maybe_size.value();
  if (expected_info_size != info_buffer.size()) {
    return etl::unexpected{HeaderParsingError::InfoBufferSizeMismatch};
  }
  if (frame.front() != SOI) {
    return etl::unexpected{HeaderParsingError::IncorrectStartByte};
  }
  if (frame.back() != EOI) {
    return etl::unexpected{HeaderParsingError::IncorrectEndByte};
  }

  size_t i = 1;
  // DO NOT CHANGE ORDER OR REMOVE/ADD ELEMENTS. This expression depends on
  // i++ being run in the correct order
  const auto maybe_version = decode_hex_byte(frame[i++], frame[i++]);
  const auto maybe_address = decode_hex_byte(frame[i++], frame[i++]);
  const auto maybe_cid1 = decode_hex_byte(frame[i++], frame[i++]);
  const auto maybe_cid2 = decode_hex_byte(frame[i++], frame[i++]);
  const auto maybe_length_hi = decode_hex_byte(frame[i++], frame[i++]);
  const auto maybe_length_lo = decode_hex_byte(frame[i++], frame[i++]);

  const auto info_hex_len = expected_info_size * ASCII_BYTES_PER_ENCODED_BYTE;
  const auto info_hex =
      etl::span<const uint8_t>(frame.begin() + i, info_hex_len);
  i += info_hex_len;
  const auto maybe_checksum_hi = decode_hex_byte(frame[i++], frame[i++]);
  const auto maybe_checksum_lo = decode_hex_byte(frame[i++], frame[i++]);

  if (!maybe_version || !maybe_address || !maybe_cid1 || !maybe_cid2 ||
      !maybe_length_lo || !maybe_length_hi || !maybe_checksum_hi ||
      !maybe_checksum_lo) {
    return etl::unexpected{HeaderParsingError::InvalidCharactersOnHexString};
  }
  const auto version = *maybe_version;
  const auto address = *maybe_address;
  const auto cid1 = *maybe_cid1;
  const auto cid2 = *maybe_cid2;

  const auto checksum = make_u16_be(*maybe_checksum_hi, *maybe_checksum_lo);
  const auto hex_length =
      split_checksum(make_u16_be(*maybe_length_hi, *maybe_length_lo));
  if (add_length_checksum(hex_length)->checksum != hex_length.checksum) {
    return etl::unexpected{HeaderParsingError::InvalidInfoLengthChecksum};
  }
  if (expected_info_size * ASCII_BYTES_PER_ENCODED_BYTE != hex_length.length) {
    return etl::unexpected{HeaderParsingError::InvalidInfoLength};
  }

  if (hex_string_checksum(etl::span<const uint8_t>(
          frame.begin() + 1 /*skip SOI*/,
          frame.end() - 5 /*skip checksum and EOI*/)) != checksum) {
    return etl::unexpected{HeaderParsingError::InvalidChecksum};
  }

  const auto maybe_parse_error = parse_hex_string(info_hex, info_buffer);
  if (maybe_parse_error) {
    const auto error = *maybe_parse_error;
    // Since we checked the sizes earlier we should not worry about this
    assert(error != HexArrayParsingError::InputOutputSizeMismatch);
    assert(error == HexArrayParsingError::BadInputCharacters);
    return etl::unexpected{HeaderParsingError::InvalidCharactersOnHexString};
  }

  return pylontech_rs485::frame{
      version, address, try_parse_cid1(cid1), try_parse_cid2(cid2), info_buffer,
  };
}

void write_u8_hex(uint8_t input, etl::span<uint8_t, 2> output) {
  constexpr auto char_map = "0123456789ABCDEF";
  output[0] = char_map[(input >> 4) & 0xf];
  output[1] = char_map[input & 0xf];
}

void write_u16_be_hex(uint16_t input, etl::span<uint8_t, 4> output) {
  write_u8_hex((input >> 8) & 0xff, output.subspan<0, 2>());
  write_u8_hex(input & 0xff, output.subspan<2, 2>());
}

[[nodiscard]] bool write_hex_string(etl::span<const uint8_t> input,
                                    etl::span<uint8_t> output) {
  if (input.size() * 2 != output.size()) {
    return false;
  }
  for (size_t i = 0; i < input.size(); ++i) {
    write_u8_hex(input[i],
                 etl::span<uint8_t, 2>(output.begin() + i * 2, size_t(2)));
  }
  return true;
}

enum struct EncodingError {
  OutputBufferSizeMismatch,
  InfoTooLarge,
};

[[nodiscard]] etl::optional<EncodingError> encode_frame(
    etl::span<uint8_t> output, const frame& frame) {
  const auto maybe_required_size =
      packet_size_from_info_size(frame.info.size());
  if (!maybe_required_size) {
    return EncodingError::InfoTooLarge;
  }
  const auto required_size = *maybe_required_size;
  if (output.size() != required_size) {
    return EncodingError::OutputBufferSizeMismatch;
  }
  output.front() = SOI;
  output.back() = EOI;
  write_u8_hex(frame.version, output.subspan<1, 2>());
  write_u8_hex(frame.address, output.subspan<3, 2>());
  write_u8_hex(cid1_to_u8(frame.cid1), output.subspan<5, 2>());
  write_u8_hex(cid2_to_u8(frame.cid2), output.subspan<7, 2>());
  // These should not fail since we checked the size of the info payload and
  // the checksum is zero
  write_u16_be_hex(
      combine_checksum(
          add_length_checksum(
              {0, static_cast<uint16_t>(frame.info.size() *
                                        ASCII_BYTES_PER_ENCODED_BYTE)})
              .value())
          .value(),
      output.subspan<9, 4>());
  assert(write_hex_string(
      frame.info, etl::span<uint8_t>{
                      output.begin() + 13 /* SOI + all the fields above */,
                      output.end() - 5 /* EOI + checksum */
                  }));
  const auto checksum = hex_string_checksum(etl::span<const uint8_t>{
      output.begin() + 1 /* SOI */, output.end() - 5 /* EOI + checksum */
  });
  write_u16_be_hex(checksum, etl::span<uint8_t, 4>{
                                 output.end() - 5 /* EOI + checksum */,
                                 output.end() - 1 /* EOI */
                             });
  return etl::nullopt;
}

}  // namespace packet_parsing

}  // namespace pylontech_rs485
