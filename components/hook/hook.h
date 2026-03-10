#pragma once

#include "esphome/components/signal/signal.h"
#include "esphome/core/automation.h"

namespace esphome::hook {

template<class C> class HookTrigger : public Trigger<typename C::value_type> {
 public:
  using T = typename C::value_type;

  explicit HookTrigger(C *signal) {
    signal->add_on_value_callback([this](const T &value) { this->trigger(value); });
  }
};

}  // namespace esphome::hook