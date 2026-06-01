#pragma once

#include <cstdint>
#include <utility>

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/optional.h"
#include "esphome/core/template_lambda.h"

namespace esphome::thermal_estimator {

class ThermalEstimator : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;

  void set_temperature_sensor(sensor::Sensor* temperature_sensor) {
    this->temperature_sensor_ = temperature_sensor;
  }
  template<typename F> void set_thermal_output_lambda(F&& f) {
    this->thermal_output_lambda_.set(std::forward<F>(f));
  }
  void set_learning_enabled(bool learning_enabled) {
    this->learning_enabled_ = learning_enabled;
  }
  void set_initial_thermal_gain(float initial_thermal_gain) {
    this->initial_thermal_gain_ = initial_thermal_gain;
  }
  void set_thermal_gain_limits(float min_thermal_gain, float max_thermal_gain) {
    this->min_thermal_gain_ = min_thermal_gain;
    this->max_thermal_gain_ = max_thermal_gain;
  }
  void set_ambient_thermal_output_threshold(float threshold) {
    this->ambient_thermal_output_threshold_ = threshold;
  }
  void set_gain_learning_thermal_output_threshold(float threshold) {
    this->gain_learning_thermal_output_threshold_ = threshold;
  }
  void set_thermal_output_stable_tolerance(float tolerance) {
    this->thermal_output_stable_tolerance_ = tolerance;
  }
  void set_stable_temperature_slope(float stable_temperature_slope) {
    this->stable_temperature_slope_ = stable_temperature_slope;
  }
  void set_thermal_output_stable_time(uint32_t stable_time_ms) {
    this->thermal_output_stable_time_ms_ = stable_time_ms;
  }
  void set_ambient_learning_time_constant(uint32_t time_constant_ms) {
    this->ambient_learning_time_constant_ms_ = time_constant_ms;
  }
  void set_thermal_gain_learning_time_constant(uint32_t time_constant_ms) {
    this->thermal_gain_learning_time_constant_ms_ = time_constant_ms;
  }
  void set_slope_filter_time_constant(uint32_t time_constant_ms) {
    this->slope_filter_time_constant_ms_ = time_constant_ms;
  }
  void set_confidence_time(uint32_t confidence_time_ms) {
    this->confidence_time_ms_ = confidence_time_ms;
  }

  void set_ambient_temperature_sensor(sensor::Sensor* sensor) {
    this->ambient_temperature_sensor_ = sensor;
  }
  void set_estimated_steady_state_temperature_sensor(sensor::Sensor* sensor) {
    this->estimated_steady_state_temperature_sensor_ = sensor;
  }
  void set_thermal_gain_sensor(sensor::Sensor* sensor) {
    this->thermal_gain_sensor_ = sensor;
  }
  void set_confidence_sensor(sensor::Sensor* sensor) {
    this->confidence_sensor_ = sensor;
  }
  void set_normalized_thermal_output_sensor(sensor::Sensor* sensor) {
    this->thermal_output_sensor_ = sensor;
  }
  void set_temperature_slope_sensor(sensor::Sensor* sensor) {
    this->temperature_slope_sensor_ = sensor;
  }

 protected:
  struct Sample {
    float temperature;
    float thermal_output;
  };

  optional<Sample> read_sample_();
  void initialize_(const Sample& sample, uint32_t now);
  void update_temperature_slope_(float temperature, float dt_ms);
  void update_thermal_output_stability_(float thermal_output, uint32_t dt_ms);
  void update_ambient_estimate_(const Sample& sample, float dt_ms);
  void update_thermal_gain_estimate_(const Sample& sample, float dt_ms);
  void publish_estimates_(const Sample& sample);
  bool is_thermally_stable_() const;
  float filtered_alpha_(float dt_ms, uint32_t time_constant_ms) const;
  float confidence_percent_() const;

  sensor::Sensor* temperature_sensor_{nullptr};
  TemplateLambda<float> thermal_output_lambda_;

  sensor::Sensor* ambient_temperature_sensor_{nullptr};
  sensor::Sensor* estimated_steady_state_temperature_sensor_{nullptr};
  sensor::Sensor* thermal_gain_sensor_{nullptr};
  sensor::Sensor* confidence_sensor_{nullptr};
  sensor::Sensor* thermal_output_sensor_{nullptr};
  sensor::Sensor* temperature_slope_sensor_{nullptr};

  float initial_thermal_gain_{25.0f};
  float min_thermal_gain_{1.0f};
  float max_thermal_gain_{150.0f};
  float ambient_thermal_output_threshold_{0.02f};
  float gain_learning_thermal_output_threshold_{0.35f};
  float thermal_output_stable_tolerance_{0.03f};
  float stable_temperature_slope_{0.03f};

  uint32_t thermal_output_stable_time_ms_{20UL * 60UL * 1000UL};
  uint32_t ambient_learning_time_constant_ms_{6UL * 60UL * 60UL * 1000UL};
  uint32_t thermal_gain_learning_time_constant_ms_{24UL * 60UL * 60UL * 1000UL};
  uint32_t slope_filter_time_constant_ms_{5UL * 60UL * 1000UL};
  uint32_t confidence_time_ms_{24UL * 60UL * 60UL * 1000UL};

  uint32_t last_update_ms_{0};
  uint32_t stable_thermal_output_ms_{0};

  float ambient_temperature_{0.0f};
  float thermal_gain_{0.0f};
  float last_temperature_{0.0f};
  float last_thermal_output_{0.0f};
  float temperature_slope_c_per_min_{0.0f};
  float ambient_confidence_{0.0f};
  float gain_confidence_{0.0f};
  bool learning_enabled_{true};
  bool initialized_{false};
};

template <typename... Ts>
class SetLearningEnabledAction : public Action<Ts...> {
 public:
  explicit SetLearningEnabledAction(ThermalEstimator* parent) : parent_(parent) {}

  void set_learning_enabled(bool learning_enabled) {
    this->learning_enabled_ = learning_enabled;
  }

  void play(Ts... x) override {
    this->parent_->set_learning_enabled(this->learning_enabled_);
  }

 protected:
  ThermalEstimator* parent_;
  bool learning_enabled_{true};
};

}  // namespace esphome::thermal_estimator
