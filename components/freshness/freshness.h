#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome::freshness {

class Freshness : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_timeout(uint32_t timeout_ms) {
    this->timeout_ms_ = timeout_ms;
    this->has_timeout_ = true;
  }
  void reserve_dependencies(size_t dependency_count) {
    this->dependencies_.init(dependency_count);
  }
  void add_dependency(Freshness* dependency) {
    this->dependencies_.push_back(dependency);
  }
  void feed();
  bool is_stale() const { return this->stale_; }
  void add_on_state_callback(std::function<void(bool)>&& callback) {
    this->state_callbacks_.add(std::move(callback));
  }

 protected:
  void refresh_from_dependencies_();
  void refresh_leaf_state_();
  void set_stale_(bool stale);

  FixedVector<Freshness*> dependencies_;
  LazyCallbackManager<void(bool)> state_callbacks_;
  uint32_t timeout_ms_{0};
  uint32_t last_feed_millis_{0};
  bool stale_{true};
  bool has_timeout_{false};
  bool fed_{false};
};

class StateChangeTrigger : public Trigger<bool> {
 public:
  explicit StateChangeTrigger(Freshness* parent) {
    parent->add_on_state_callback([this](bool stale) { this->trigger(stale); });
  }
};

template <typename... Ts>
class BaseAction : public Action<Ts...>, public Parented<Freshness> {};

template <typename... Ts>
class FeedAction : public BaseAction<Ts...> {
 public:
  void play(const Ts&... x) override { this->parent_->feed(); }
};

template <typename... Ts>
class FreshCondition : public Condition<Ts...>, public Parented<Freshness> {
 public:
  explicit FreshCondition(Freshness* parent, bool stale)
      : Parented<Freshness>(parent), stale_(stale) {}

 protected:
  bool check(const Ts&... x) override {
    return this->parent_->is_stale() == this->stale_;
  }

  bool stale_;
};

}  // namespace esphome::freshness