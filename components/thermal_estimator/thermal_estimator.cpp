#include "thermal_estimator.h"

#include <algorithm>
#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::thermal_estimator {

static const char* const TAG = "thermal_estimator";

static bool is_finite(float value) { return std::isfinite(value); }

static float clamp_float(float value, float min_value, float max_value) {
  return std::min(std::max(value, min_value), max_value);
}

void ThermalEstimator::setup() {
  ESPHOME_DEBUG_ASSERT(this->temperature_sensor_ != nullptr);
  ESPHOME_DEBUG_ASSERT(this->thermal_output_lambda_.has_value());
  ESPHOME_DEBUG_ASSERT(this->min_thermal_gain_ > 0.0f);
  ESPHOME_DEBUG_ASSERT(this->max_thermal_gain_ >= this->min_thermal_gain_);
  ESPHOME_DEBUG_ASSERT(this->initial_thermal_gain_ >= this->min_thermal_gain_);
  ESPHOME_DEBUG_ASSERT(this->initial_thermal_gain_ <= this->max_thermal_gain_);
}

void ThermalEstimator::update() {
  const optional<Sample> sample = this->read_sample_();
  if (!sample.has_value()) {
    return;
  }

  const uint32_t now = millis();
  if (!this->initialized_) {
    this->initialize_(*sample, now);
    this->publish_estimates_(*sample);
    return;
  }

  const uint32_t dt_ms = now - this->last_update_ms_;
  if (dt_ms == 0) {
    return;
  }

  this->update_temperature_slope_(sample->temperature,
                                  static_cast<float>(dt_ms));
  this->update_thermal_output_stability_(sample->thermal_output, dt_ms);
  if (this->learning_enabled_) {
    this->update_ambient_estimate_(*sample, static_cast<float>(dt_ms));
    this->update_thermal_gain_estimate_(*sample, static_cast<float>(dt_ms));
  }
  this->publish_estimates_(*sample);

  this->last_update_ms_ = now;
  this->last_temperature_ = sample->temperature;
  this->last_thermal_output_ = sample->thermal_output;
}

void ThermalEstimator::dump_config() {
  ESP_LOGCONFIG(TAG, "Thermal Estimator:");
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Initial thermal gain: %.2f °C/thermal_output",
                this->initial_thermal_gain_);
  ESP_LOGCONFIG(TAG, "  Thermal gain range: %.2f-%.2f °C/thermal_output",
                this->min_thermal_gain_, this->max_thermal_gain_);
  ESP_LOGCONFIG(TAG, "  Learning enabled: %s",
                YESNO(this->learning_enabled_));
  ESP_LOGCONFIG(TAG, "  Ambient learning thermal output threshold: %.3f",
                this->ambient_thermal_output_threshold_);
  ESP_LOGCONFIG(TAG, "  Gain learning thermal output threshold: %.3f",
                this->gain_learning_thermal_output_threshold_);
  ESP_LOGCONFIG(TAG, "  Thermal output stable tolerance: %.3f",
                this->thermal_output_stable_tolerance_);
  ESP_LOGCONFIG(TAG, "  Stable temperature slope: %.3f °C/min",
                this->stable_temperature_slope_);
  ESP_LOGCONFIG(TAG, "  Thermal output stable time: %u ms",
                this->thermal_output_stable_time_ms_);
  ESP_LOGCONFIG(TAG, "  Ambient learning time constant: %u ms",
                this->ambient_learning_time_constant_ms_);
  ESP_LOGCONFIG(TAG, "  Thermal gain learning time constant: %u ms",
                this->thermal_gain_learning_time_constant_ms_);
  ESP_LOGCONFIG(TAG, "  Slope filter time constant: %u ms",
                this->slope_filter_time_constant_ms_);
  ESP_LOGCONFIG(TAG, "  Confidence time: %u ms", this->confidence_time_ms_);
  LOG_SENSOR("  ", "Estimated ambient temperature",
             this->ambient_temperature_sensor_);
  LOG_SENSOR("  ", "Estimated steady-state temperature",
             this->estimated_steady_state_temperature_sensor_);
  LOG_SENSOR("  ", "Thermal gain", this->thermal_gain_sensor_);
  LOG_SENSOR("  ", "Confidence", this->confidence_sensor_);
  LOG_SENSOR("  ", "Normalized thermal output", this->thermal_output_sensor_);
  LOG_SENSOR("  ", "Temperature slope", this->temperature_slope_sensor_);
}

optional<ThermalEstimator::Sample> ThermalEstimator::read_sample_() {
  const float temperature = this->temperature_sensor_->state;
  if (!is_finite(temperature)) {
    ESP_LOGW(TAG, "Skipping estimator update because temperature is not finite");
    return {};
  }

  const optional<float> maybe_thermal_output = this->thermal_output_lambda_();
  if (!maybe_thermal_output.has_value() ||
      !is_finite(*maybe_thermal_output)) {
    ESP_LOGW(TAG,
             "Skipping estimator update because thermal output is not finite");
    return {};
  }

  const float thermal_output =
      clamp_float(*maybe_thermal_output, 0.0f, 1.0f);
  if (thermal_output != *maybe_thermal_output) {
    ESP_LOGW(TAG, "Thermal output %.3f was clamped to %.3f",
             *maybe_thermal_output, thermal_output);
  }

  return Sample{temperature, thermal_output};
}

void ThermalEstimator::initialize_(const Sample& sample, uint32_t now) {
  this->last_update_ms_ = now;
  this->stable_thermal_output_ms_ = 0;
  this->ambient_temperature_ =
      sample.temperature -
      this->initial_thermal_gain_ * sample.thermal_output;
  this->thermal_gain_ = this->initial_thermal_gain_;
  this->last_temperature_ = sample.temperature;
  this->last_thermal_output_ = sample.thermal_output;
  this->temperature_slope_c_per_min_ = 0.0f;
  this->ambient_confidence_ =
      sample.thermal_output <= this->ambient_thermal_output_threshold_ ? 0.05f
                                                                       : 0.0f;
  this->gain_confidence_ = 0.0f;
  this->initialized_ = true;
}

void ThermalEstimator::update_temperature_slope_(float temperature, float dt_ms) {
  const float dt_min = dt_ms / 60000.0f;
  const float raw_slope = (temperature - this->last_temperature_) / dt_min;
  const float alpha =
      this->filtered_alpha_(dt_ms, this->slope_filter_time_constant_ms_);
  this->temperature_slope_c_per_min_ +=
      alpha * (raw_slope - this->temperature_slope_c_per_min_);
}

void ThermalEstimator::update_thermal_output_stability_(float thermal_output,
                                                    uint32_t dt_ms) {
  if (std::fabs(thermal_output - this->last_thermal_output_) <=
      this->thermal_output_stable_tolerance_) {
    const uint32_t previous = this->stable_thermal_output_ms_;
    this->stable_thermal_output_ms_ += dt_ms;
    if (this->stable_thermal_output_ms_ < previous) {
      this->stable_thermal_output_ms_ = UINT32_MAX;
    }
    return;
  }

  this->stable_thermal_output_ms_ = 0;
}

void ThermalEstimator::update_ambient_estimate_(const Sample& sample, float dt_ms) {
  if (sample.thermal_output > this->ambient_thermal_output_threshold_ ||
      !this->is_thermally_stable_()) {
    return;
  }

  const float alpha =
      this->filtered_alpha_(dt_ms, this->ambient_learning_time_constant_ms_);
  this->ambient_temperature_ +=
      alpha * (sample.temperature - this->ambient_temperature_);

  const float confidence_alpha =
      this->filtered_alpha_(dt_ms, this->confidence_time_ms_);
  this->ambient_confidence_ +=
      confidence_alpha * (1.0f - this->ambient_confidence_);
}

void ThermalEstimator::update_thermal_gain_estimate_(const Sample& sample,
                                                 float dt_ms) {
  if (sample.thermal_output < this->gain_learning_thermal_output_threshold_ ||
      !this->is_thermally_stable_()) {
    return;
  }

  const float observed_gain =
      (sample.temperature - this->ambient_temperature_) / sample.thermal_output;
  if (!is_finite(observed_gain)) {
    return;
  }

  const float clamped_gain =
      clamp_float(observed_gain, this->min_thermal_gain_,
                  this->max_thermal_gain_);
  const float alpha = this->filtered_alpha_(
      dt_ms, this->thermal_gain_learning_time_constant_ms_);
  this->thermal_gain_ += alpha * (clamped_gain - this->thermal_gain_);

  const float confidence_alpha =
      this->filtered_alpha_(dt_ms, this->confidence_time_ms_);
  this->gain_confidence_ += confidence_alpha * (1.0f - this->gain_confidence_);
}

void ThermalEstimator::publish_estimates_(const Sample& sample) {
  const float steady_state_temperature =
      this->ambient_temperature_ + this->thermal_gain_ * sample.thermal_output;

  if (this->ambient_temperature_sensor_ != nullptr) {
    this->ambient_temperature_sensor_->publish_state(this->ambient_temperature_);
  }
  if (this->estimated_steady_state_temperature_sensor_ != nullptr) {
    this->estimated_steady_state_temperature_sensor_->publish_state(
        steady_state_temperature);
  }
  if (this->thermal_gain_sensor_ != nullptr) {
    this->thermal_gain_sensor_->publish_state(this->thermal_gain_);
  }
  if (this->confidence_sensor_ != nullptr) {
    this->confidence_sensor_->publish_state(this->confidence_percent_());
  }
  if (this->thermal_output_sensor_ != nullptr) {
    this->thermal_output_sensor_->publish_state(sample.thermal_output);
  }
  if (this->temperature_slope_sensor_ != nullptr) {
    this->temperature_slope_sensor_->publish_state(
        this->temperature_slope_c_per_min_);
  }
}

bool ThermalEstimator::is_thermally_stable_() const {
  return this->stable_thermal_output_ms_ >=
             this->thermal_output_stable_time_ms_ &&
         std::fabs(this->temperature_slope_c_per_min_) <=
             this->stable_temperature_slope_;
}

float ThermalEstimator::filtered_alpha_(float dt_ms,
                                    uint32_t time_constant_ms) const {
  ESPHOME_DEBUG_ASSERT(time_constant_ms > 0);
  return 1.0f - std::exp(-dt_ms / static_cast<float>(time_constant_ms));
}

float ThermalEstimator::confidence_percent_() const {
  return 100.0f * std::min(this->ambient_confidence_, this->gain_confidence_);
}

}  // namespace esphome::thermal_estimator
