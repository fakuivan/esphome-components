# Thermal estimator

Generic thermal estimator for devices where a temperature sensor is thermally
coupled to a known heat-producing input. For example, it can be used with smart
bulbs, power supplies, enclosed relays, motor drivers, or battery chargers.

The estimator uses the internal temperature sensor plus a normalized thermal
power lambda, where `0.0` means no load and `1.0` means the hottest expected
load. This value does not need to be calibrated in watts, but it
should be roughly proportional to real heat-producing output power across the
light modes you use.

It learns ambient temperature while thermal output is near zero and learns
thermal gain after the requested thermal output and measured temperature have
been stable long enough.

The intended long-term model is split in two:

- channel maps convert per-channel output levels into normalized thermal output
- the estimator consumes the total thermal output and learns how heat propagates
  through the device to the MCU temperature sensor

For now, the channel map is provided by the `thermal_output` lambda. Future
offline channel calibration can freeze online estimator learning, use the
estimator as a measuring instrument, and update those channel maps separately.

## Algorithm

The current implementation is a one-input steady-state thermal estimator:

```text
estimated_steady_state_temperature =
  estimated_ambient_temperature + thermal_gain * normalized_thermal_output
```

`normalized_thermal_output` is sampled from the configured lambda and clamped to
`0.0..1.0`. The estimator does not try to know real watts; `thermal_gain` learns
how many degrees of measured temperature rise correspond to one unit of
normalized thermal output.

The estimator keeps a filtered temperature slope:

```text
raw_slope = (temperature_now - temperature_previous) / elapsed_minutes
filtered_slope += alpha * (raw_slope - filtered_slope)
alpha = 1 - exp(-dt / slope_filter_time_constant)
```

Thermal output is considered stable after it has stayed within
`thermal_output_stable_tolerance` for `thermal_output_stable_time`. Learning is
allowed only when both thermal output and measured temperature are stable enough.

Ambient learning runs when thermal output is near zero:

```text
if normalized_thermal_output <= ambient_thermal_output_threshold:
  estimated_ambient += alpha * (temperature - estimated_ambient)
```

Thermal-gain learning runs when thermal output is high enough:

```text
observed_gain =
  (temperature - estimated_ambient_temperature) / normalized_thermal_output

thermal_gain += alpha * (clamp(observed_gain) - thermal_gain)
```

The confidence sensor is the lower of ambient-learning confidence and
thermal-gain-learning confidence. Freezing learning stops ambient/gain updates,
but the component still samples temperature, tracks slope/stability, and
publishes estimates.

## Example

```yaml
thermal_estimator:
  id: bulb_estimator
  temperature: internal_temperature_sensor
  thermal_output: |-
    float red;
    float green;
    float blue;
    float cold_white;
    float warm_white;
    id(light_rgbww).current_values_as_rgbww(
        &red, &green, &blue, &cold_white, &warm_white, false);

    // Relative thermal-power proxy. Tune the weights for the device if needed.
    const float rgb_power = 0.35f * (red + green + blue) / 3.0f;
    const float white_power = cold_white + warm_white;
    const float thermal_output = rgb_power > white_power ? rgb_power : white_power;
    return thermal_output > 1.0f ? 1.0f : thermal_output;
  ambient_temperature:
    name: Estimated Ambient Temperature
  estimated_steady_state_temperature:
    name: Estimated Steady-State Temperature
  thermal_gain:
    name: Thermal Gain
  confidence:
    name: Thermal Model Confidence
  normalized_thermal_output:
    name: Normalized Thermal Output
```

## Learning control

Online learning can be frozen while an offline calibration routine is driving
the bulb:

```yaml
- thermal_estimator.freeze_learning: bulb_estimator
- thermal_estimator.resume_learning: bulb_estimator
```
