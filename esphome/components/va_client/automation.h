#pragma once

#include "esphome/core/automation.h"
#include "va_client.h"

#include <string>

namespace esphome {
namespace va_client {

class OnPhaseTrigger : public Trigger<std::string> {
 public:
  explicit OnPhaseTrigger(VaClient *parent) { parent->add_on_phase_trigger(this); }
};

class OnRepeatedFailureTrigger : public Trigger<> {
 public:
  explicit OnRepeatedFailureTrigger(VaClient *parent) {
    parent->add_on_repeated_failure_trigger(this);
  }
};

// Fires after reply drainage but before the mic opens. YAML receives the bound
// token and session nonce, plays the chime, and returns both on commit.
class OnFollowupOpenedTrigger : public Trigger<uint32_t, uint32_t> {
 public:
  explicit OnFollowupOpenedTrigger(VaClient *parent) {
    parent->add_on_followup_opened_trigger(this);
  }
};

}  // namespace va_client
}  // namespace esphome
