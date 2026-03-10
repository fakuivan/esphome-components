#pragma once

#include <cstring>

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

namespace esphome::signal {

class SignalBase {
 public:
  virtual ~SignalBase() = default;
};

template<typename T> class Signal : public SignalBase {
 public:
  using value_type = T;

  explicit Signal() = default;
  explicit Signal(T initial_value) : value_(initial_value) {}
  explicit Signal(std::array<typename std::remove_extent<T>::type, std::extent<T>::value> initial_value) {
    memcpy(this->value_, initial_value.data(), sizeof(T));
  }

  const T &value() const { return this->value_; }

  void set_value(const T &value) {
    this->value_ = value;
    this->callbacks_.call(this->value_);
  }

  void add_on_value_callback(std::function<void(const T &)> &&callback) { this->callbacks_.add(std::move(callback)); }

 protected:
  LazyCallbackManager<void(const T &)> callbacks_;
  T value_{};
};

template<class C, typename... Ts> class SignalSetAction : public Action<Ts...> {
 public:
  explicit SignalSetAction(C *parent) : parent_(parent) {}

  using T = typename C::value_type;

  TEMPLATABLE_VALUE(T, value)

  void play(const Ts &...x) override { this->parent_->set_value(this->value_.value(x...)); }

 protected:
  C *parent_;
};

template<typename T> Signal<T> &id(Signal<T> *value) { return *value; }

}  // namespace esphome::signal