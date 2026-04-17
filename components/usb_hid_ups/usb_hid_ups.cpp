#if defined(USE_ESP32_VARIANT_ESP32P4) || \
    defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_hid_ups.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <new>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::usb_hid_ups {

static constexpr const char* POLL_INTERVAL_NAME = "poll";

static constexpr uint8_t USB_DESCRIPTOR_TYPE_HID = 0x21;
static constexpr uint8_t USB_DESCRIPTOR_TYPE_REPORT = 0x22;
static constexpr uint8_t USB_REQUEST_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t HID_REQUEST_GET_REPORT = 0x01;

static constexpr uint16_t USAGE_PAGE_POWER_DEVICE = 0x0084;
static constexpr uint16_t USAGE_PAGE_BATTERY_SYSTEM = 0x0085;
static constexpr uint16_t USAGE_PAGE_POWER_DEVICE_APC = 0xFF84;
static constexpr uint16_t USAGE_PAGE_BATTERY_SYSTEM_APC = 0xFF85;

static constexpr uint16_t POWER_USAGE_UPS = 0x0004;
static constexpr uint16_t POWER_USAGE_BATTERY_SYSTEM = 0x0010;
static constexpr uint16_t POWER_USAGE_BATTERY = 0x0012;
static constexpr uint16_t POWER_USAGE_INPUT = 0x001A;
static constexpr uint16_t POWER_USAGE_OUTPUT = 0x001C;
static constexpr uint16_t POWER_USAGE_VOLTAGE = 0x0030;
static constexpr uint16_t POWER_USAGE_PERCENT_LOAD = 0x0035;

static constexpr uint16_t BATTERY_USAGE_CHARGING = 0x0044;
static constexpr uint16_t BATTERY_USAGE_DISCHARGING = 0x0045;
static constexpr uint16_t BATTERY_USAGE_RELATIVE_STATE_OF_CHARGE = 0x0064;
static constexpr uint16_t BATTERY_USAGE_RUN_TIME_TO_EMPTY = 0x0068;
static constexpr uint16_t BATTERY_USAGE_AVERAGE_TIME_TO_EMPTY = 0x0069;
static constexpr uint16_t BATTERY_USAGE_AC_PRESENT = 0x00D0;

static constexpr uint8_t HID_MAIN_ITEM_INPUT = 0x08;
static constexpr uint8_t HID_MAIN_ITEM_COLLECTION = 0x0A;
static constexpr uint8_t HID_MAIN_ITEM_FEATURE = 0x0B;
static constexpr uint8_t HID_MAIN_ITEM_END_COLLECTION = 0x0C;

static constexpr uint8_t HID_GLOBAL_ITEM_USAGE_PAGE = 0x00;
static constexpr uint8_t HID_GLOBAL_ITEM_LOGICAL_MINIMUM = 0x01;
static constexpr uint8_t HID_GLOBAL_ITEM_LOGICAL_MAXIMUM = 0x02;
static constexpr uint8_t HID_GLOBAL_ITEM_UNIT_EXPONENT = 0x05;
static constexpr uint8_t HID_GLOBAL_ITEM_UNIT = 0x06;
static constexpr uint8_t HID_GLOBAL_ITEM_REPORT_SIZE = 0x07;
static constexpr uint8_t HID_GLOBAL_ITEM_REPORT_ID = 0x08;
static constexpr uint8_t HID_GLOBAL_ITEM_REPORT_COUNT = 0x09;
static constexpr uint8_t HID_GLOBAL_ITEM_PUSH = 0x0A;
static constexpr uint8_t HID_GLOBAL_ITEM_POP = 0x0B;

static constexpr uint8_t HID_LOCAL_ITEM_USAGE = 0x00;
static constexpr uint8_t HID_LOCAL_ITEM_USAGE_MINIMUM = 0x01;
static constexpr uint8_t HID_LOCAL_ITEM_USAGE_MAXIMUM = 0x02;

static constexpr size_t MAX_LOCAL_USAGES = 32;
static constexpr size_t MAX_COLLECTION_DEPTH = 12;
static constexpr size_t MAX_GLOBAL_DEPTH = 4;

struct UsageParts {
  uint16_t page;
  uint16_t usage;
};

struct LocalState {
  std::array<uint32_t, MAX_LOCAL_USAGES> usages{};
  size_t usage_count{0};
  bool has_usage_range{false};
  uint32_t usage_min{0};
  uint32_t usage_max{0};

  void clear() {
    this->usage_count = 0;
    this->has_usage_range = false;
    this->usage_min = 0;
    this->usage_max = 0;
  }
};

struct GlobalState {
  uint16_t usage_page{0};
  int32_t logical_min{0};
  int32_t logical_max{0};
  uint32_t unit{0};
  uint8_t report_size{0};
  uint8_t report_id{0};
  uint8_t report_count{0};
  int8_t unit_exponent{0};
};

struct CollectionState {
  uint16_t usage_page{0};
  uint16_t usage{0};
};

static const char* report_type_to_string(HIDReportType report_type) {
  switch (report_type) {
    case HIDReportType::INPUT:
      return "INPUT";
    case HIDReportType::FEATURE:
      return "FEATURE";
    default:
      return "UNKNOWN";
  }
}

static const char* role_to_string(HIDFieldRole role) {
  switch (role) {
    case HIDFieldRole::INPUT:
      return "input";
    case HIDFieldRole::OUTPUT:
      return "output";
    case HIDFieldRole::BATTERY:
      return "battery";
    case HIDFieldRole::BATTERY_SYSTEM:
      return "battery_system";
    default:
      return "unknown";
  }
}

static const char* metric_to_string(MetricKind metric) {
  switch (metric) {
    case MetricKind::BATTERY_LEVEL:
      return "battery_level";
    case MetricKind::RUNTIME:
      return "runtime";
    case MetricKind::LOAD_PERCENT:
      return "load_percent";
    case MetricKind::INPUT_VOLTAGE:
      return "input_voltage";
    case MetricKind::OUTPUT_VOLTAGE:
      return "output_voltage";
    case MetricKind::BATTERY_VOLTAGE:
      return "battery_voltage";
    default:
      return "unknown";
  }
}

static uint16_t normalize_usage_page(uint16_t usage_page,
                                     bool normalize_apc_pages) {
  if (!normalize_apc_pages) return usage_page;
  if (usage_page == USAGE_PAGE_POWER_DEVICE_APC) return USAGE_PAGE_POWER_DEVICE;
  if (usage_page == USAGE_PAGE_BATTERY_SYSTEM_APC)
    return USAGE_PAGE_BATTERY_SYSTEM;
  return usage_page;
}

static uint32_t read_unsigned_le(const uint8_t* data, size_t length) {
  uint32_t value = 0;
  for (size_t index = 0; index < length; index++) {
    value |= static_cast<uint32_t>(data[index]) << (index * 8);
  }
  return value;
}

static int32_t read_signed_le(const uint8_t* data, size_t length) {
  if (length == 0) return 0;
  uint32_t value = read_unsigned_le(data, length);
  if (length >= sizeof(int32_t)) return static_cast<int32_t>(value);
  const uint32_t sign_bit = 1UL << (length * 8 - 1);
  if ((value & sign_bit) != 0) {
    const uint32_t mask = ~((1UL << (length * 8)) - 1);
    value |= mask;
  }
  return static_cast<int32_t>(value);
}

static int8_t decode_unit_exponent(int32_t raw_value) {
  int8_t exponent = static_cast<int8_t>(raw_value & 0x0F);
  if ((exponent & 0x08) != 0) {
    exponent = static_cast<int8_t>(exponent - 0x10);
  }
  return exponent;
}

static float apply_exponent(int32_t value, int8_t exponent) {
  float scaled = static_cast<float>(value);
  while (exponent > 0) {
    scaled *= 10.0f;
    exponent--;
  }
  while (exponent < 0) {
    scaled /= 10.0f;
    exponent++;
  }
  return scaled;
}

static int32_t sign_extend(uint32_t value, uint8_t bit_size) {
  if (bit_size == 0 || bit_size >= 32) return static_cast<int32_t>(value);
  const uint32_t sign_bit = 1UL << (bit_size - 1);
  if ((value & sign_bit) == 0) return static_cast<int32_t>(value);
  const uint32_t mask = ~((1UL << bit_size) - 1);
  return static_cast<int32_t>(value | mask);
}

static uint32_t extract_bits(const uint8_t* data, size_t data_len,
                             uint16_t bit_offset, uint8_t bit_size) {
  uint32_t value = 0;
  for (uint8_t bit_index = 0; bit_index < bit_size; bit_index++) {
    const size_t absolute_bit = static_cast<size_t>(bit_offset) + bit_index;
    const size_t byte_index = absolute_bit / 8;
    const uint8_t mask = 1U << (absolute_bit % 8);
    if (byte_index >= data_len) break;
    if ((data[byte_index] & mask) != 0) {
      value |= 1UL << bit_index;
    }
  }
  return value;
}

static bool is_variable_data_main_item(uint32_t flags) {
  return (flags & 0x01) == 0 && (flags & 0x02) != 0;
}

void USBHIDUPS::setup() {
  USBClient::setup();
  if (this->is_failed()) {
    return;
  }

  this->report_descriptor_buffer_.reset(new (std::nothrow)
                                            uint8_t[MAX_CONTROL_DATA_BYTES]);
  if (!this->report_descriptor_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate HID report descriptor buffer");
    this->status_set_error(LOG_STR("HID descriptor buffer allocation failed"));
    this->mark_failed();
    return;
  }

  auto err = usb_host_transfer_alloc(
      MAX_CONTROL_DATA_BYTES + usb_host::SETUP_PACKET_SIZE, 0,
      &this->control_transfer_);
  if (err != ESP_OK || this->control_transfer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate control transfer buffer: %s",
             esp_err_to_name(err));
    this->status_set_error(LOG_STR("HID control transfer allocation failed"));
    this->mark_failed();
  }
}

void USBHIDUPS::loop() {
  bool had_work = this->process_usb_events_();

  if (this->report_descriptor_ready_.exchange(false,
                                              std::memory_order_acq_rel)) {
    had_work = true;
    this->handle_descriptor_ready_();
  }

  if (this->process_report_events_()) {
    had_work = true;
  }

  if (this->poll_requested_ && this->runtime_started_ &&
      !this->control_transfer_active_.load()) {
    had_work = true;
    this->submit_next_poll_request_();
  }

  if (!had_work) {
    this->disable_loop();
  }
}

bool USBHIDUPS::process_report_events_() {
  bool had_work = false;

  ReportEvent* event;
  while ((event = this->report_queue_.pop()) != nullptr) {
    had_work = true;
    this->handle_report_event_(*event);
    this->report_pool_.release(event);
  }

  const uint16_t dropped = this->report_queue_.get_and_reset_dropped_count();
  if (dropped > 0) {
    ESP_LOGW(TAG, "Dropped %u HID report events due to queue overflow",
             dropped);
  }

  return had_work;
}

bool USBHIDUPS::discover_hid_interface_() {
  const usb_config_desc_t* config_desc;
  if (usb_host_get_active_config_descriptor(this->device_handle_,
                                            &config_desc) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get active config descriptor");
    return false;
  }

  HIDInterfaceContext selected{};
  const uint8_t* config_start = reinterpret_cast<const uint8_t*>(config_desc);
  const uint8_t* config_end = config_start + config_desc->wTotalLength;

  for (uint8_t interface_index = 0;; interface_index++) {
    int conf_offset = 0;
    const auto* interface_desc = usb_parse_interface_descriptor(
        config_desc, interface_index, 0, &conf_offset);
    if (interface_desc == nullptr) {
      break;
    }

    if (this->has_forced_interface_number_ &&
        interface_desc->bInterfaceNumber != this->forced_interface_number_) {
      continue;
    }
    if (interface_desc->bInterfaceClass != USB_CLASS_HID) {
      continue;
    }

    HIDInterfaceContext candidate{};
    candidate.interface_number = interface_desc->bInterfaceNumber;

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(interface_desc) +
                         interface_desc->bLength;
    while (ptr + 2 <= config_end) {
      const uint8_t descriptor_length = ptr[0];
      const uint8_t descriptor_type = ptr[1];
      if (descriptor_length < 2 || ptr + descriptor_length > config_end) {
        break;
      }
      if (descriptor_type == USB_B_DESCRIPTOR_TYPE_INTERFACE ||
          descriptor_type == USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION) {
        break;
      }

      if (descriptor_type == USB_DESCRIPTOR_TYPE_HID &&
          descriptor_length >= 9) {
        const uint8_t num_descriptors = ptr[5];
        for (uint8_t descriptor_index = 0; descriptor_index < num_descriptors;
             descriptor_index++) {
          const size_t entry_offset = 6 + descriptor_index * 3;
          if (entry_offset + 2 >= descriptor_length) {
            break;
          }
          if (ptr[entry_offset] == USB_DESCRIPTOR_TYPE_REPORT) {
            candidate.report_descriptor_length = static_cast<uint16_t>(
                ptr[entry_offset + 1] | (ptr[entry_offset + 2] << 8));
            break;
          }
        }
      } else if (descriptor_type == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
        const auto* endpoint_desc = reinterpret_cast<const usb_ep_desc_t*>(ptr);
        const uint8_t transfer_type =
            endpoint_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
        if (transfer_type == USB_BM_ATTRIBUTES_XFER_INT &&
            (endpoint_desc->bEndpointAddress & usb_host::USB_DIR_IN)) {
          if (!this->has_forced_interrupt_in_endpoint_ ||
              endpoint_desc->bEndpointAddress ==
                  this->forced_interrupt_in_endpoint_) {
            candidate.interrupt_in_endpoint = endpoint_desc->bEndpointAddress;
            candidate.interrupt_packet_size =
                std::min<uint16_t>(endpoint_desc->wMaxPacketSize, 64);
          }
        }
      }

      ptr += descriptor_length;
    }

    if (candidate.report_descriptor_length == 0) {
      ESP_LOGW(TAG,
               "HID interface %u did not advertise a report descriptor length",
               candidate.interface_number);
    }

    if (this->has_forced_interrupt_in_endpoint_ &&
        candidate.interrupt_in_endpoint == 0) {
      candidate.interrupt_in_endpoint = this->forced_interrupt_in_endpoint_;
      candidate.interrupt_packet_size = 64;
      ESP_LOGW(TAG,
               "Forced interrupt IN endpoint 0x%02X was not found in "
               "descriptors, using it anyway",
               this->forced_interrupt_in_endpoint_);
    }

    selected = candidate;
    break;
  }

  if (selected.interface_number == 0xFF) {
    return false;
  }

  this->hid_interface_ = selected;
  return true;
}

bool USBHIDUPS::fetch_report_descriptor_() {
  uint16_t request_length = this->hid_interface_.report_descriptor_length;
  if (request_length == 0 || request_length > MAX_CONTROL_DATA_BYTES) {
    if (request_length > MAX_CONTROL_DATA_BYTES) {
      ESP_LOGW(TAG,
               "Report descriptor is %u bytes, truncating request to %zu bytes",
               request_length, MAX_CONTROL_DATA_BYTES);
    }
    request_length = MAX_CONTROL_DATA_BYTES;
  }

  ESP_LOGD(TAG, "Requesting HID report descriptor on interface %u (%u bytes)",
           this->hid_interface_.interface_number, request_length);

  return this->submit_control_transfer_(
      usb_host::USB_TYPE_STANDARD | usb_host::USB_RECIP_INTERFACE |
          usb_host::USB_DIR_IN,
      USB_REQUEST_GET_DESCRIPTOR,
      static_cast<uint16_t>((USB_DESCRIPTOR_TYPE_REPORT << 8) |
                            this->report_descriptor_index_),
      this->hid_interface_.interface_number, request_length,
      ControlAction::REPORT_DESCRIPTOR);
}

bool USBHIDUPS::parse_report_descriptor_() {
  if (this->report_descriptor_length_ == 0) {
    ESP_LOGE(TAG, "Received an empty HID report descriptor");
    return false;
  }

  this->field_count_ = 0;
  this->report_layout_count_ = 0;
  this->report_target_count_ = 0;
  for (auto& binding : this->metric_bindings_) {
    binding.available = false;
    binding.field_index = 0xFF;
  }

  GlobalState global;
  LocalState local;
  std::array<GlobalState, MAX_GLOBAL_DEPTH> global_stack{};
  size_t global_stack_depth = 0;
  std::array<CollectionState, MAX_COLLECTION_DEPTH> collection_stack{};
  size_t collection_depth = 0;

  auto normalize_page = [this](uint16_t usage_page) {
    return normalize_usage_page(usage_page, this->normalize_apc_pages_);
  };

  auto split_usage = [&](uint32_t raw_usage,
                         uint16_t default_page) -> UsageParts {
    if (raw_usage > 0xFFFF) {
      return {normalize_page(static_cast<uint16_t>(raw_usage >> 16)),
              static_cast<uint16_t>(raw_usage & 0xFFFF)};
    }
    return {normalize_page(default_page),
            static_cast<uint16_t>(raw_usage & 0xFFFF)};
  };

  auto current_role = [&]() -> HIDFieldRole {
    for (size_t depth = collection_depth; depth > 0; depth--) {
      const auto& collection = collection_stack[depth - 1];
      if (collection.usage_page != USAGE_PAGE_POWER_DEVICE) continue;
      switch (collection.usage) {
        case POWER_USAGE_INPUT:
          return HIDFieldRole::INPUT;
        case POWER_USAGE_OUTPUT:
          return HIDFieldRole::OUTPUT;
        case POWER_USAGE_BATTERY:
          return HIDFieldRole::BATTERY;
        case POWER_USAGE_BATTERY_SYSTEM:
          return HIDFieldRole::BATTERY_SYSTEM;
        default:
          break;
      }
    }
    return HIDFieldRole::UNKNOWN;
  };

  auto resolve_usage = [&](uint8_t field_index) -> UsageParts {
    if (local.usage_count > 0) {
      const size_t usage_index =
          local.usage_count == 1
              ? 0
              : std::min<size_t>(field_index, local.usage_count - 1);
      return split_usage(local.usages[usage_index], global.usage_page);
    }
    if (local.has_usage_range && local.usage_max >= local.usage_min) {
      const uint32_t raw_usage =
          local.usage_min +
          std::min<uint32_t>(field_index, local.usage_max - local.usage_min);
      return split_usage(raw_usage, global.usage_page);
    }
    return {normalize_page(global.usage_page), 0};
  };

  auto collection_usage = [&]() -> UsageParts {
    if (local.usage_count > 0) {
      return split_usage(local.usages[0], global.usage_page);
    }
    if (local.has_usage_range) {
      return split_usage(local.usage_min, global.usage_page);
    }
    return {normalize_page(global.usage_page), 0};
  };

  const uint8_t* descriptor = this->report_descriptor_buffer_.get();
  size_t offset = 0;
  while (offset < this->report_descriptor_length_) {
    const uint8_t prefix = descriptor[offset++];
    if (prefix == 0xFE) {
      if (offset + 1 >= this->report_descriptor_length_) break;
      const uint8_t long_item_length = descriptor[offset++];
      offset++;
      offset = std::min(offset + static_cast<size_t>(long_item_length),
                        this->report_descriptor_length_);
      continue;
    }

    const uint8_t size_code = prefix & 0x03;
    const size_t item_length = size_code == 3 ? 4 : size_code;
    const uint8_t type = (prefix >> 2) & 0x03;
    const uint8_t tag = (prefix >> 4) & 0x0F;
    if (offset + item_length > this->report_descriptor_length_) {
      ESP_LOGW(TAG, "Truncated HID item at byte %u",
               static_cast<unsigned>(offset - 1));
      break;
    }

    const uint32_t unsigned_value =
        read_unsigned_le(descriptor + offset, item_length);
    const int32_t signed_value =
        read_signed_le(descriptor + offset, item_length);
    offset += item_length;

    if (type == 0) {
      if (tag == HID_MAIN_ITEM_INPUT || tag == HID_MAIN_ITEM_FEATURE) {
        const auto report_type = tag == HID_MAIN_ITEM_INPUT
                                     ? HIDReportType::INPUT
                                     : HIDReportType::FEATURE;
        auto* layout =
            this->find_or_create_report_layout_(report_type, global.report_id);
        if (layout == nullptr) {
          local.clear();
          continue;
        }
        const uint16_t base_bit_offset = layout->bit_length;
        const uint16_t item_bit_length =
            global.report_size * global.report_count;

        if (is_variable_data_main_item(unsigned_value) &&
            global.report_size != 0 && global.report_count != 0) {
          for (uint8_t report_index = 0; report_index < global.report_count;
               report_index++) {
            if (this->field_count_ == MAX_FIELDS) {
              ESP_LOGW(
                  TAG,
                  "Reached HID field limit (%zu), descriptor parsing truncated",
                  MAX_FIELDS);
              break;
            }

            const UsageParts usage = resolve_usage(report_index);
            auto& field = this->fields_[this->field_count_++];
            field.usage_page = usage.page;
            field.usage = usage.usage;
            field.role = current_role();
            field.report_type = report_type;
            field.report_id = global.report_id;
            field.bit_offset =
                base_bit_offset + report_index * global.report_size;
            field.bit_size = global.report_size;
            field.logical_min = global.logical_min;
            field.logical_max = global.logical_max;
            field.unit = global.unit;
            field.unit_exponent = global.unit_exponent;
            field.is_signed = global.logical_min < 0;
          }
        }

        layout->bit_length = static_cast<uint16_t>(
            std::min<uint32_t>(0xFFFF, base_bit_offset + item_bit_length));
      } else if (tag == HID_MAIN_ITEM_COLLECTION) {
        if (collection_depth < MAX_COLLECTION_DEPTH) {
          const UsageParts usage = collection_usage();
          collection_stack[collection_depth++] = {usage.page, usage.usage};
        }
      } else if (tag == HID_MAIN_ITEM_END_COLLECTION) {
        if (collection_depth > 0) {
          collection_depth--;
        }
      }
      local.clear();
      continue;
    }

    if (type == 1) {
      switch (tag) {
        case HID_GLOBAL_ITEM_USAGE_PAGE:
          global.usage_page =
              normalize_page(static_cast<uint16_t>(unsigned_value));
          break;
        case HID_GLOBAL_ITEM_LOGICAL_MINIMUM:
          global.logical_min = signed_value;
          break;
        case HID_GLOBAL_ITEM_LOGICAL_MAXIMUM:
          global.logical_max = global.logical_min < 0
                                   ? signed_value
                                   : static_cast<int32_t>(unsigned_value);
          break;
        case HID_GLOBAL_ITEM_UNIT_EXPONENT:
          global.unit_exponent = decode_unit_exponent(signed_value);
          break;
        case HID_GLOBAL_ITEM_UNIT:
          global.unit = unsigned_value;
          break;
        case HID_GLOBAL_ITEM_REPORT_SIZE:
          global.report_size = static_cast<uint8_t>(unsigned_value & 0xFF);
          break;
        case HID_GLOBAL_ITEM_REPORT_ID:
          global.report_id = static_cast<uint8_t>(unsigned_value & 0xFF);
          break;
        case HID_GLOBAL_ITEM_REPORT_COUNT:
          global.report_count = static_cast<uint8_t>(unsigned_value & 0xFF);
          break;
        case HID_GLOBAL_ITEM_PUSH:
          if (global_stack_depth < global_stack.size()) {
            global_stack[global_stack_depth++] = global;
          }
          break;
        case HID_GLOBAL_ITEM_POP:
          if (global_stack_depth > 0) {
            global = global_stack[--global_stack_depth];
          }
          break;
        default:
          break;
      }
      continue;
    }

    if (type == 2) {
      switch (tag) {
        case HID_LOCAL_ITEM_USAGE:
          if (local.usage_count < local.usages.size()) {
            local.usages[local.usage_count++] = unsigned_value;
          }
          break;
        case HID_LOCAL_ITEM_USAGE_MINIMUM:
          local.has_usage_range = true;
          local.usage_min = unsigned_value;
          break;
        case HID_LOCAL_ITEM_USAGE_MAXIMUM:
          local.has_usage_range = true;
          local.usage_max = unsigned_value;
          break;
        default:
          break;
      }
    }
  }

  ESP_LOGD(TAG, "Parsed %u HID fields from a %u byte report descriptor",
           static_cast<unsigned>(this->field_count_),
           static_cast<unsigned>(this->report_descriptor_length_));
  return true;
}

bool USBHIDUPS::build_metric_bindings_() {
  std::array<int, static_cast<size_t>(MetricKind::COUNT)> best_scores{};
  best_scores.fill(-1);
  for (auto& binding : this->metric_bindings_) {
    binding.available = false;
    binding.field_index = 0xFF;
  }

  for (size_t field_index = 0; field_index < this->field_count_;
       field_index++) {
    const auto& field = this->fields_[field_index];
    for (size_t metric_index = 0;
         metric_index < static_cast<size_t>(MetricKind::COUNT);
         metric_index++) {
      const auto metric = static_cast<MetricKind>(metric_index);
      if (this->get_metric_sensor_(metric) == nullptr) {
        continue;
      }
      const int score = this->score_field_(metric, field);
      if (score > best_scores[metric_index]) {
        best_scores[metric_index] = score;
        this->metric_bindings_[metric_index].available = true;
        this->metric_bindings_[metric_index].field_index =
            static_cast<uint8_t>(field_index);
      }
    }
  }

  bool any_bound = false;
  for (size_t metric_index = 0;
       metric_index < static_cast<size_t>(MetricKind::COUNT); metric_index++) {
    const auto metric = static_cast<MetricKind>(metric_index);
    if (this->get_metric_sensor_(metric) == nullptr) {
      continue;
    }

    auto& binding = this->metric_bindings_[metric_index];
    if (!binding.available || best_scores[metric_index] < 0) {
      binding.available = false;
      binding.field_index = 0xFF;
      ESP_LOGW(TAG, "No HID field matched configured %s sensor",
               metric_to_string(metric));
      continue;
    }

    const auto& field = this->fields_[binding.field_index];
    const bool has_interrupt_input =
        field.report_type == HIDReportType::INPUT &&
        this->hid_interface_.interrupt_in_endpoint != 0;
    if (!this->register_report_target_(field) && !has_interrupt_input) {
      binding.available = false;
      binding.field_index = 0xFF;
      ESP_LOGW(TAG,
               "Skipping %s sensor because report id %u is too large to queue "
               "safely",
               metric_to_string(metric), field.report_id);
      continue;
    }

    any_bound = true;
    ESP_LOGD(TAG, "Mapped %s to page 0x%04X usage 0x%04X (%s, %s report id %u)",
             metric_to_string(metric), field.usage_page, field.usage,
             role_to_string(field.role),
             report_type_to_string(field.report_type), field.report_id);
  }

  return any_bound;
}

void USBHIDUPS::handle_descriptor_ready_() {
  ESP_LOGD(TAG, "Received HID report descriptor (%u bytes)",
           static_cast<unsigned>(this->report_descriptor_length_));
  if (!this->parse_report_descriptor_()) {
    this->status_set_error(LOG_STR("Failed to parse HID report descriptor"));
    this->disconnect();
    return;
  }

  const bool any_mapped = this->build_metric_bindings_();
  if (!any_mapped) {
    this->status_set_warning(
        LOG_STR("No requested HID UPS metrics were found"));
  } else {
    this->status_clear_warning();
  }

  this->finalize_runtime_();
}

void USBHIDUPS::finalize_runtime_() {
  if (this->explore_) {
    this->dump_parsed_fields_();
  }

  this->runtime_started_ = true;
  this->set_interval(POLL_INTERVAL_NAME, this->update_interval_ms_,
                     [this]() { this->request_poll_cycle_(); });

  if (this->hid_interface_.interrupt_in_endpoint != 0) {
    this->start_interrupt_in_transfer_();
  }

  this->request_poll_cycle_();
}

void USBHIDUPS::request_poll_cycle_() {
  if (!this->runtime_started_ ||
      this->state_ != usb_host::USB_CLIENT_CONNECTED ||
      this->report_target_count_ == 0) {
    return;
  }
  this->poll_requested_ = true;
  this->next_poll_target_index_ = 0;
  this->enable_loop();
}

void USBHIDUPS::submit_next_poll_request_() {
  if (!this->poll_requested_ || this->control_transfer_active_.load()) {
    return;
  }
  if (this->next_poll_target_index_ >= this->report_target_count_) {
    this->poll_requested_ = false;
    this->next_poll_target_index_ = 0;
    return;
  }

  const auto& target = this->report_targets_[this->next_poll_target_index_++];
  if (!this->submit_get_report_(target)) {
    ESP_LOGW(TAG, "Failed to queue GET_REPORT for %s report id %u",
             report_type_to_string(target.report_type), target.report_id);
  }
}

bool USBHIDUPS::submit_get_report_(const ReportTarget& target) {
  const uint16_t request_length =
      std::clamp<uint16_t>(target.expected_length, 1, MAX_CONTROL_DATA_BYTES);
  return this->submit_control_transfer_(
      usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE |
          usb_host::USB_DIR_IN,
      HID_REQUEST_GET_REPORT,
      static_cast<uint16_t>((static_cast<uint16_t>(target.report_type) << 8) |
                            target.report_id),
      this->hid_interface_.interface_number, request_length,
      ControlAction::GET_REPORT, &target);
}

bool USBHIDUPS::submit_control_transfer_(uint8_t request_type, uint8_t request,
                                         uint16_t value, uint16_t index,
                                         uint16_t length, ControlAction action,
                                         const ReportTarget* target) {
  if (this->control_transfer_ == nullptr) {
    return false;
  }
  if (this->control_transfer_active_.exchange(true)) {
    return false;
  }
  if (length > MAX_CONTROL_DATA_BYTES) {
    this->control_transfer_active_.store(false);
    return false;
  }

  this->control_action_ = action;
  this->active_report_target_ = target != nullptr ? *target : ReportTarget{};

  this->control_transfer_->context = this;
  this->control_transfer_->device_handle = this->device_handle_;
  this->control_transfer_->bEndpointAddress =
      request_type & usb_host::USB_DIR_MASK;
  this->control_transfer_->num_bytes = length + usb_host::SETUP_PACKET_SIZE;
  this->control_transfer_->callback =
      reinterpret_cast<usb_transfer_cb_t>(control_transfer_callback_);

  uint8_t* buffer = this->control_transfer_->data_buffer;
  buffer[0] = request_type;
  buffer[1] = request;
  buffer[2] = static_cast<uint8_t>(value & 0xFF);
  buffer[3] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buffer[4] = static_cast<uint8_t>(index & 0xFF);
  buffer[5] = static_cast<uint8_t>((index >> 8) & 0xFF);
  buffer[6] = static_cast<uint8_t>(length & 0xFF);
  buffer[7] = static_cast<uint8_t>((length >> 8) & 0xFF);

  const auto err =
      usb_host_transfer_submit_control(this->handle_, this->control_transfer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to submit control transfer: %s",
             esp_err_to_name(err));
    this->control_action_ = ControlAction::NONE;
    this->control_transfer_active_.store(false);
    return false;
  }
  return true;
}

void USBHIDUPS::control_transfer_callback_(const usb_transfer_t* transfer) {
  auto* component = static_cast<USBHIDUPS*>(transfer->context);
  const auto action = component->control_action_;
  const auto target = component->active_report_target_;

  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    ESP_LOGW(
        TAG, "Control transfer failed while fetching %s, status=%d",
        action == ControlAction::REPORT_DESCRIPTOR ? "descriptor" : "report",
        transfer->status);
  } else if (action == ControlAction::REPORT_DESCRIPTOR) {
    const size_t copy_length =
        std::min<size_t>(transfer->actual_num_bytes, MAX_CONTROL_DATA_BYTES);
    memcpy(component->report_descriptor_buffer_.get(), transfer->data_buffer,
           copy_length);
    component->report_descriptor_length_ = copy_length;
    component->report_descriptor_ready_.store(true, std::memory_order_release);
  } else if (action == ControlAction::GET_REPORT) {
    ReportEvent* event = component->report_pool_.allocate();
    if (event == nullptr) {
      component->report_queue_.increment_dropped_count();
    } else {
      event->kind = EventKind::POLLED_REPORT;
      event->report_type = target.report_type;
      event->truncated =
          transfer->actual_num_bytes > ReportEvent::MAX_REPORT_BYTES;
      event->length = static_cast<uint16_t>(std::min<size_t>(
          transfer->actual_num_bytes, ReportEvent::MAX_REPORT_BYTES));
      memcpy(event->data, transfer->data_buffer, event->length);
      component->report_queue_.push(event);
    }
  }

  component->control_action_ = ControlAction::NONE;
  component->control_transfer_active_.store(false, std::memory_order_release);
  component->enable_loop_soon_any_context();
  App.wake_loop_threadsafe();
}

bool USBHIDUPS::register_report_target_(const HIDField& field) {
  const uint16_t expected_length = this->report_length_for_field_(field);
  if (expected_length == 0 || expected_length > ReportEvent::MAX_REPORT_BYTES ||
      expected_length > MAX_CONTROL_DATA_BYTES) {
    return false;
  }

  for (size_t index = 0; index < this->report_target_count_; index++) {
    const auto& target = this->report_targets_[index];
    if (target.report_type == field.report_type &&
        target.report_id == field.report_id) {
      return true;
    }
  }

  if (this->report_target_count_ == MAX_REPORT_TARGETS) {
    ESP_LOGW(TAG, "Reached report target limit (%zu)", MAX_REPORT_TARGETS);
    return false;
  }

  this->report_targets_[this->report_target_count_++] = {
      field.report_type, field.report_id, expected_length};
  return true;
}

void USBHIDUPS::start_interrupt_in_transfer_() {
  if (!this->runtime_started_ ||
      this->hid_interface_.interrupt_in_endpoint == 0) {
    return;
  }

  bool expected = false;
  if (!this->interrupt_in_active_.compare_exchange_strong(expected, true)) {
    return;
  }

  const uint16_t transfer_length =
      std::min<uint16_t>(this->hid_interface_.interrupt_packet_size, 64);
  const bool submitted = this->transfer_in(
      this->hid_interface_.interrupt_in_endpoint,
      [this](const usb_host::TransferStatus& status) {
        this->interrupt_in_active_.store(false);
        if (!status.success) {
          ESP_LOGW(TAG, "Interrupt IN transfer failed: %s",
                   esp_err_to_name(status.error_code));
          return;
        }

        ReportEvent* event = this->report_pool_.allocate();
        if (event == nullptr) {
          this->report_queue_.increment_dropped_count();
        } else {
          event->kind = EventKind::INPUT_REPORT;
          event->report_type = HIDReportType::INPUT;
          event->truncated = status.data_len > ReportEvent::MAX_REPORT_BYTES;
          event->length = static_cast<uint16_t>(
              std::min<size_t>(status.data_len, ReportEvent::MAX_REPORT_BYTES));
          memcpy(event->data, status.data, event->length);
          this->report_queue_.push(event);
        }

        this->enable_loop_soon_any_context();
        App.wake_loop_threadsafe();

        if (this->runtime_started_ &&
            this->state_ == usb_host::USB_CLIENT_CONNECTED) {
          this->start_interrupt_in_transfer_();
        }
      },
      transfer_length);
  if (!submitted) {
    this->interrupt_in_active_.store(false);
  }
}

void USBHIDUPS::handle_report_event_(const ReportEvent& event) {
  uint8_t report_id = 0;
  size_t payload_offset = 0;
  if (!this->decode_report_id_(event.report_type, event.data, event.length,
                               &report_id, &payload_offset)) {
    return;
  }
  if (payload_offset > event.length) {
    return;
  }

  const uint8_t* payload = event.data + payload_offset;
  const size_t payload_length = event.length - payload_offset;

  if (this->explore_) {
    ESP_LOGV(TAG, "Handling %s report id %u (%u bytes)%s",
             report_type_to_string(event.report_type), report_id,
             static_cast<unsigned>(event.length),
             event.truncated ? " [truncated]" : "");
  }

  for (size_t metric_index = 0;
       metric_index < static_cast<size_t>(MetricKind::COUNT); metric_index++) {
    const auto metric = static_cast<MetricKind>(metric_index);
    const auto& binding = this->metric_bindings_[metric_index];
    if (!binding.available || binding.field_index == 0xFF) {
      continue;
    }

    const auto& field = this->fields_[binding.field_index];
    if (field.report_type != event.report_type ||
        field.report_id != report_id) {
      continue;
    }

    const float value =
        this->extract_field_value_(field, payload, payload_length);
    if (!std::isfinite(value)) {
      continue;
    }
    this->publish_metric_(metric, value);
  }
}

HIDReportLayout* USBHIDUPS::find_or_create_report_layout_(
    HIDReportType report_type, uint8_t report_id) {
  for (size_t index = 0; index < this->report_layout_count_; index++) {
    auto& layout = this->report_layouts_[index];
    if (layout.report_type == report_type && layout.report_id == report_id) {
      return &layout;
    }
  }

  if (this->report_layout_count_ == MAX_REPORT_LAYOUTS) {
    ESP_LOGW(TAG, "Reached HID report layout limit (%zu)", MAX_REPORT_LAYOUTS);
    return nullptr;
  }

  auto& layout = this->report_layouts_[this->report_layout_count_++];
  layout.report_type = report_type;
  layout.report_id = report_id;
  layout.bit_length = 0;
  return &layout;
}

const HIDReportLayout* USBHIDUPS::find_report_layout_(HIDReportType report_type,
                                                      uint8_t report_id) const {
  for (size_t index = 0; index < this->report_layout_count_; index++) {
    const auto& layout = this->report_layouts_[index];
    if (layout.report_type == report_type && layout.report_id == report_id) {
      return &layout;
    }
  }
  return nullptr;
}

sensor::Sensor* USBHIDUPS::get_metric_sensor_(MetricKind metric) const {
  switch (metric) {
    case MetricKind::BATTERY_LEVEL:
      return this->battery_level_sensor_;
    case MetricKind::RUNTIME:
      return this->runtime_sensor_;
    case MetricKind::LOAD_PERCENT:
      return this->load_percent_sensor_;
    case MetricKind::INPUT_VOLTAGE:
      return this->input_voltage_sensor_;
    case MetricKind::OUTPUT_VOLTAGE:
      return this->output_voltage_sensor_;
    case MetricKind::BATTERY_VOLTAGE:
      return this->battery_voltage_sensor_;
    default:
      return nullptr;
  }
}

void USBHIDUPS::publish_metric_(MetricKind metric, float value) {
  auto* sensor = this->get_metric_sensor_(metric);
  if (sensor == nullptr) {
    return;
  }
  sensor->publish_state(value);
}

uint16_t USBHIDUPS::report_length_for_field_(const HIDField& field) const {
  const auto* layout =
      this->find_report_layout_(field.report_type, field.report_id);
  if (layout == nullptr) {
    return 0;
  }
  uint16_t byte_length = static_cast<uint16_t>((layout->bit_length + 7) / 8);
  if (field.report_id != 0) {
    byte_length++;
  }
  return byte_length;
}

int USBHIDUPS::score_field_(MetricKind metric, const HIDField& field) const {
  switch (metric) {
    case MetricKind::BATTERY_LEVEL:
      if (field.usage_page != USAGE_PAGE_BATTERY_SYSTEM ||
          field.usage != BATTERY_USAGE_RELATIVE_STATE_OF_CHARGE) {
        return -1;
      }
      return field.role == HIDFieldRole::BATTERY ||
                     field.role == HIDFieldRole::BATTERY_SYSTEM
                 ? 120
                 : 90;

    case MetricKind::RUNTIME:
      if (field.usage_page != USAGE_PAGE_BATTERY_SYSTEM) {
        return -1;
      }
      if (field.usage == BATTERY_USAGE_RUN_TIME_TO_EMPTY) {
        return field.role == HIDFieldRole::BATTERY ||
                       field.role == HIDFieldRole::BATTERY_SYSTEM
                   ? 120
                   : 95;
      }
      if (field.usage == BATTERY_USAGE_AVERAGE_TIME_TO_EMPTY) {
        return field.role == HIDFieldRole::BATTERY ||
                       field.role == HIDFieldRole::BATTERY_SYSTEM
                   ? 110
                   : 85;
      }
      return -1;

    case MetricKind::LOAD_PERCENT:
      if (field.usage_page != USAGE_PAGE_POWER_DEVICE ||
          field.usage != POWER_USAGE_PERCENT_LOAD) {
        return -1;
      }
      return field.role == HIDFieldRole::OUTPUT ? 120 : 90;

    case MetricKind::INPUT_VOLTAGE:
      if (field.usage_page != USAGE_PAGE_POWER_DEVICE ||
          field.usage != POWER_USAGE_VOLTAGE) {
        return -1;
      }
      return field.role == HIDFieldRole::INPUT ? 120 : 80;

    case MetricKind::OUTPUT_VOLTAGE:
      if (field.usage_page != USAGE_PAGE_POWER_DEVICE ||
          field.usage != POWER_USAGE_VOLTAGE) {
        return -1;
      }
      return field.role == HIDFieldRole::OUTPUT ? 120 : 80;

    case MetricKind::BATTERY_VOLTAGE:
      if (field.usage_page != USAGE_PAGE_POWER_DEVICE ||
          field.usage != POWER_USAGE_VOLTAGE) {
        return -1;
      }
      return field.role == HIDFieldRole::BATTERY ||
                     field.role == HIDFieldRole::BATTERY_SYSTEM
                 ? 120
                 : 70;

    default:
      return -1;
  }
}

bool USBHIDUPS::decode_report_id_(HIDReportType report_type,
                                  const uint8_t* data, size_t data_len,
                                  uint8_t* report_id,
                                  size_t* payload_offset) const {
  bool has_nonzero_report_id = false;
  for (size_t index = 0; index < this->report_layout_count_; index++) {
    const auto& layout = this->report_layouts_[index];
    if (layout.report_type == report_type && layout.report_id != 0) {
      has_nonzero_report_id = true;
      break;
    }
  }

  if (!has_nonzero_report_id) {
    *report_id = 0;
    *payload_offset = 0;
    return true;
  }
  if (data_len == 0) {
    return false;
  }
  *report_id = data[0];
  *payload_offset = 1;
  return true;
}

float USBHIDUPS::extract_field_value_(const HIDField& field,
                                      const uint8_t* payload,
                                      size_t payload_len) const {
  if (field.bit_size == 0 || field.bit_size > 32) {
    return NAN;
  }
  if (static_cast<size_t>(field.bit_offset) + field.bit_size >
      payload_len * 8U) {
    return NAN;
  }

  const uint32_t raw_value =
      extract_bits(payload, payload_len, field.bit_offset, field.bit_size);
  const int32_t signed_value = field.is_signed
                                   ? sign_extend(raw_value, field.bit_size)
                                   : static_cast<int32_t>(raw_value);
  return apply_exponent(signed_value, field.unit_exponent);
}

void USBHIDUPS::dump_parsed_fields_() const {
  ESP_LOGI(TAG, "Discovered %u HID UPS fields",
           static_cast<unsigned>(this->field_count_));
  for (size_t index = 0; index < this->field_count_; index++) {
    const auto& field = this->fields_[index];
    ESP_LOGI(TAG,
             "Field %u: page=0x%04X usage=0x%04X role=%s type=%s id=%u "
             "offset=%u bits=%u exp=%d",
             static_cast<unsigned>(index), field.usage_page, field.usage,
             role_to_string(field.role),
             report_type_to_string(field.report_type), field.report_id,
             field.bit_offset, field.bit_size, field.unit_exponent);
  }
}

void USBHIDUPS::on_connected() {
  this->runtime_started_ = false;
  this->poll_requested_ = false;
  this->next_poll_target_index_ = 0;
  this->field_count_ = 0;
  this->report_layout_count_ = 0;
  this->report_target_count_ = 0;
  this->report_descriptor_length_ = 0;
  this->report_descriptor_ready_.store(false, std::memory_order_release);
  this->interrupt_in_active_.store(false);
  this->control_transfer_active_.store(false);
  this->cancel_interval(POLL_INTERVAL_NAME);

  if (!this->discover_hid_interface_()) {
    this->status_set_error(
        LOG_STR("No HID interface found for the configured UPS"));
    this->disconnect();
    return;
  }

  ESP_LOGD(TAG, "Using HID interface %u%s",
           this->hid_interface_.interface_number,
           this->hid_interface_.interrupt_in_endpoint != 0
               ? " with interrupt IN"
               : " without interrupt IN");

  const auto err =
      usb_host_interface_claim(this->handle_, this->device_handle_,
                               this->hid_interface_.interface_number, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to claim HID interface %u: %s",
             this->hid_interface_.interface_number, esp_err_to_name(err));
    this->status_set_error(LOG_STR("Failed to claim HID interface"));
    this->disconnect();
    return;
  }

  if (!this->fetch_report_descriptor_()) {
    this->status_set_error(LOG_STR("Failed to fetch HID report descriptor"));
    this->disconnect();
  }
}

void USBHIDUPS::on_disconnected() {
  this->cancel_interval(POLL_INTERVAL_NAME);
  this->runtime_started_ = false;
  this->poll_requested_ = false;
  this->next_poll_target_index_ = 0;
  this->report_descriptor_ready_.store(false, std::memory_order_release);
  this->control_transfer_active_.store(false);
  this->interrupt_in_active_.store(false);
  this->field_count_ = 0;
  this->report_layout_count_ = 0;
  this->report_target_count_ = 0;

  if (this->hid_interface_.interrupt_in_endpoint != 0) {
    usb_host_endpoint_halt(this->device_handle_,
                           this->hid_interface_.interrupt_in_endpoint);
    usb_host_endpoint_flush(this->device_handle_,
                            this->hid_interface_.interrupt_in_endpoint);
  }
  if (this->hid_interface_.interface_number != 0xFF) {
    usb_host_interface_release(this->handle_, this->device_handle_,
                               this->hid_interface_.interface_number);
  }
  this->hid_interface_ = {};

  USBClient::on_disconnected();
}

void USBHIDUPS::dump_config() {
  ESP_LOGCONFIG(TAG, "USB HID UPS:");
  USBClient::dump_config();
  ESP_LOGCONFIG(TAG, "  Update interval: %" PRIu32 " ms",
                this->update_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Explore mode: %s", YESNO(this->explore_));
  ESP_LOGCONFIG(TAG, "  Normalize APC pages: %s",
                YESNO(this->normalize_apc_pages_));
  if (this->has_forced_interface_number_) {
    ESP_LOGCONFIG(TAG, "  Forced interface number: %u",
                  this->forced_interface_number_);
  }
  if (this->has_forced_interrupt_in_endpoint_) {
    ESP_LOGCONFIG(TAG, "  Forced interrupt IN endpoint: 0x%02X",
                  this->forced_interrupt_in_endpoint_);
  }
  ESP_LOGCONFIG(TAG, "  Report descriptor index: %u",
                this->report_descriptor_index_);
  LOG_SENSOR("  ", "Battery level", this->battery_level_sensor_);
  LOG_SENSOR("  ", "Runtime", this->runtime_sensor_);
  LOG_SENSOR("  ", "Load percent", this->load_percent_sensor_);
  LOG_SENSOR("  ", "Input voltage", this->input_voltage_sensor_);
  LOG_SENSOR("  ", "Output voltage", this->output_voltage_sensor_);
  LOG_SENSOR("  ", "Battery voltage", this->battery_voltage_sensor_);
}

}  // namespace esphome::usb_hid_ups

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 ||
        // USE_ESP32_VARIANT_ESP32S3