#include "freshness.h"

#include "esphome/core/log.h"

namespace esphome::freshness {

static const char* const TAG = "freshness";

void Freshness::setup() {
  for (Freshness* dependency : this->dependencies_) {
    dependency->add_on_state_callback([this](bool stale) {
      (void)stale;
      this->refresh_from_dependencies_();
    });
  }

  if (this->dependencies_.empty()) {
    this->refresh_leaf_state_();
  } else {
    this->refresh_from_dependencies_();
  }
}

void Freshness::loop() {
  if (!this->dependencies_.empty()) {
    return;
  }

  this->refresh_leaf_state_();
}

void Freshness::dump_config() {
  ESP_LOGCONFIG(TAG, "Freshness '%s':", LOG_STR_ARG(this->name_for_log_()));
  if (this->has_timeout_) {
    ESP_LOGCONFIG(TAG, "  Timeout: %u ms", this->timeout_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  Timeout: none");
  }
  ESP_LOGCONFIG(TAG, "  Dependencies: %zu", this->dependencies_.size());
  ESP_LOGCONFIG(TAG, "  Stale: %s", this->stale_ ? "YES" : "NO");
}

void Freshness::feed() {
  const uint32_t now = millis();
  ESP_LOGD(TAG, "Freshness '%s' bumped at %u ms",
           LOG_STR_ARG(this->name_for_log_()), now);
  this->last_feed_millis_ = now;
  this->fed_ = true;

  if (this->dependencies_.empty()) {
    this->set_stale_(false);
    return;
  }

  this->refresh_from_dependencies_();
}

void Freshness::refresh_from_dependencies_() {
  for (Freshness* dependency : this->dependencies_) {
    if (dependency->is_stale()) {
      this->set_stale_(true);
      return;
    }
  }

  this->set_stale_(false);
}

void Freshness::refresh_leaf_state_() {
  if (!this->has_timeout_ || !this->fed_) {
    this->set_stale_(true);
    return;
  }

  if (this->stale_) {
    return;
  }

  const uint32_t elapsed = millis() - this->last_feed_millis_;
  if (elapsed >= this->timeout_ms_) {
    ESP_LOGD(TAG, "Freshness '%s' timed out after %u ms (timeout=%u ms)",
             LOG_STR_ARG(this->name_for_log_()), elapsed, this->timeout_ms_);
    this->set_stale_(true);
  }
}

void Freshness::set_stale_(bool stale) {
  if (this->stale_ == stale) {
    return;
  }

  this->stale_ = stale;
  this->state_callbacks_.call(stale);
}

}  // namespace esphome::freshness