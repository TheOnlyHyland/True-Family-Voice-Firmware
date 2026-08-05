#pragma once

#include "follow_up_safety.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace va_client {

enum class PilotAuthMode : uint8_t { NONE = 0, LEGACY_ZERO, NONCE };
enum class PilotPhase : uint8_t { IDLE = 0, WAITING, LISTENING, THINKING, REPLYING, ENROLLING };
enum class FollowUpStage : uint8_t { NONE = 0, PREPARED, READY, OPEN };
enum class HelloAdmission : uint8_t { REJECTED = 0, FRESH, RECOVERY };
enum class PhaseApplyStatus : uint8_t { APPLIED = 0, STALE, REJECTED };

struct PhaseApplyResult {
  PhaseApplyStatus status{PhaseApplyStatus::REJECTED};
  PilotPhase previous{PilotPhase::IDLE};
  PilotPhase target{PilotPhase::IDLE};
  bool mic_closed{false};
  bool follow_up_input_ended{false};
  bool follow_up_window_completed{false};
  bool active_after{false};
  uint32_t connection_generation{0};
  uint32_t session_nonce{0};
  uint32_t wake_generation{0};
  uint32_t effect_epoch{0};
};

struct FollowUpCredentials {
  uint32_t token{0};
  uint32_t session_nonce{0};
  uint32_t wake_generation{0};
  uint32_t ready_nonce{0};
  uint32_t audio_generation{0};
};

class MicSendFence {
 public:
  void acquire() { this->in_flight_.fetch_add(1, std::memory_order_acq_rel); }

  void release() {
    uint32_t current = this->in_flight_.load(std::memory_order_acquire);
    while (current != 0 &&
           !this->in_flight_.compare_exchange_weak(
               current, current - 1, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
  }

  uint32_t in_flight() const {
    return this->in_flight_.load(std::memory_order_acquire);
  }

  static bool lease_is_current(uint32_t lease_epoch, uint32_t current_epoch,
                               bool mic_open) {
    return mic_open && lease_epoch == current_epoch;
  }

 private:
  std::atomic_uint32_t in_flight_{0};
};

// Authoritative pilot lifecycle. VaClient serializes every call with
// followup_mux_; the host tests call the same transitions directly.
class FollowUpLifecycle {
 public:
  void on_connected() {
    this->connection_generation_ = next_generation_(this->connection_generation_);
    this->connected_ = true;
    this->auth_mode_ = PilotAuthMode::NONE;
    this->session_nonce_ = 0;
    this->enrollment_ = false;
    this->revoke_wake_();
    this->phase_ = PilotPhase::IDLE;
  }

  void on_disconnected() {
    this->connected_ = false;
    this->auth_mode_ = PilotAuthMode::NONE;
    this->session_nonce_ = 0;
    this->enrollment_ = false;
    this->revoke_wake_();
    this->phase_ = PilotPhase::IDLE;
  }

  HelloAdmission admit_trusted_hello(uint32_t nonce) {
    this->revoke_wake_();
    if (!this->connected_ || nonce == 0 || nonce > kProtocolTokenMax)
      return HelloAdmission::REJECTED;

    if (nonce == this->last_trusted_session_nonce_) {
      this->auth_mode_ = PilotAuthMode::NONCE;
      this->session_nonce_ = nonce;
      return HelloAdmission::RECOVERY;
    }
    if (contains_(this->session_nonce_history_, this->session_nonce_history_count_, nonce) ||
        this->session_nonce_history_count_ >= this->session_nonce_history_.size()) {
      this->auth_mode_ = PilotAuthMode::NONE;
      this->session_nonce_ = 0;
      return HelloAdmission::REJECTED;
    }

    this->session_nonce_history_[this->session_nonce_history_count_++] = nonce;
    this->last_trusted_session_nonce_ = nonce;
    this->auth_mode_ = PilotAuthMode::NONCE;
    this->session_nonce_ = nonce;
    this->follow_up_token_history_count_ = 0;
    this->ready_nonce_history_count_ = 0;
    return HelloAdmission::FRESH;
  }

  bool admit_legacy_zero_hello() {
    this->revoke_wake_();
    if (!this->connected_ || this->last_trusted_session_nonce_ != 0) {
      this->auth_mode_ = PilotAuthMode::NONE;
      this->session_nonce_ = 0;
      return false;
    }
    this->auth_mode_ = PilotAuthMode::LEGACY_ZERO;
    this->session_nonce_ = 0;
    return true;
  }

  void reject_hello() {
    this->auth_mode_ = PilotAuthMode::NONE;
    this->session_nonce_ = 0;
    this->revoke_wake_();
  }

  void set_muted(bool muted) {
    this->muted_ = muted;
    if (muted)
      this->revoke_wake_();
  }

  bool start_enrollment() {
    this->revoke_wake_();
    if (!this->connected_ || this->auth_mode_ == PilotAuthMode::NONE || this->muted_) {
      this->enrollment_ = false;
      return false;
    }
    this->enrollment_ = true;
    this->mic_open_ = true;
    this->phase_ = PilotPhase::ENROLLING;
    return true;
  }

  void stop_enrollment() {
    this->enrollment_ = false;
    this->revoke_wake_();
    this->phase_ = PilotPhase::IDLE;
  }

  uint32_t prepare_local_wake() {
    if (!this->base_safe_())
      return 0;
    this->revoke_wake_();
    this->reservation_generation_ = next_generation_(this->reservation_generation_);
    this->pending_reservation_ = this->reservation_generation_;
    this->last_reserved_wake_ = this->pending_reservation_;
    this->reserved_protocol_wake_generation_ = next_generation_(this->wake_generation_);
    this->pending_wake_ = true;
    this->one_shot_spent_ = false;
    this->post_stop_ = false;
    this->phase_ = PilotPhase::WAITING;
    return this->pending_reservation_;
  }

  bool pending_wake_is_safe(uint32_t reservation) const {
    return this->base_safe_() && !this->post_stop_ && this->pending_wake_ &&
           !this->active_wake_ && !this->mic_open_ && reservation != 0 &&
           reservation == this->pending_reservation_ &&
           this->reserved_protocol_wake_generation_ == next_generation_(this->wake_generation_) &&
           this->follow_up_stage_ == FollowUpStage::NONE;
  }

  uint32_t pending_protocol_wake_generation(uint32_t reservation) const {
    return this->pending_wake_is_safe(reservation)
               ? this->reserved_protocol_wake_generation_
               : 0;
  }

  bool record_wake_transmitted(uint32_t reservation,
                               uint32_t protocol_wake_generation) {
    if (reservation == 0 || reservation != this->last_reserved_wake_ ||
        protocol_wake_generation == 0 ||
        protocol_wake_generation != this->reserved_protocol_wake_generation_ ||
        protocol_wake_generation != next_generation_(this->wake_generation_))
      return false;

    const bool can_open = this->pending_wake_is_safe(reservation);
    this->wake_generation_ = protocol_wake_generation;
    this->pending_wake_ = false;
    this->pending_reservation_ = 0;
    this->transmitted_reservation_ = can_open ? reservation : 0;
    return true;
  }

  bool open_transmitted_wake(uint32_t reservation) {
    if (!this->base_safe_() || this->post_stop_ || reservation == 0 ||
        reservation != this->transmitted_reservation_ || this->active_wake_ ||
        this->mic_open_ || this->follow_up_stage_ != FollowUpStage::NONE) {
      this->transmitted_reservation_ = 0;
      this->revoke_wake_();
      return false;
    }
    this->transmitted_reservation_ = 0;
    this->active_wake_ = true;
    this->mic_open_ = true;
    this->phase_ = PilotPhase::WAITING;
    return true;
  }

  // Host-test convenience: model one successful bounded wake transmission.
  bool commit_local_wake(uint32_t reservation) {
    const uint32_t protocol_wake_generation =
        this->pending_protocol_wake_generation(reservation);
    return protocol_wake_generation != 0 &&
           this->record_wake_transmitted(reservation, protocol_wake_generation) &&
           this->open_transmitted_wake(reservation);
  }

  bool on_phase_listening() {
    if (!this->base_safe_() || this->post_stop_ || !this->active_wake_ ||
        this->pending_wake_ || !this->mic_open_) {
      this->revoke_wake_();
      return false;
    }
    this->phase_ = PilotPhase::LISTENING;
    return true;
  }

  bool on_phase_thinking(bool close_mic = true) {
    if (!this->base_safe_() || this->post_stop_ || !this->active_wake_ || this->pending_wake_) {
      this->revoke_wake_();
      return false;
    }
    if (close_mic)
      this->close_mic_();
    this->phase_ = PilotPhase::THINKING;
    return true;
  }

  bool on_phase_replying(bool keep_mic_open = false,
                          bool *follow_up_window_completed = nullptr) {
    if (!this->base_safe_() || this->post_stop_ || !this->active_wake_ ||
        this->pending_wake_) {
      this->revoke_wake_();
      return false;
    }
    const bool keep_open = keep_mic_open && this->base_safe_() && !this->post_stop_ &&
                            this->active_wake_ && !this->pending_wake_;
    if (!keep_open)
      this->close_mic_();
    const bool completed = this->follow_up_stage_ == FollowUpStage::OPEN;
    if (completed) {
      this->follow_up_stage_ = FollowUpStage::NONE;
      this->credentials_ = {};
    }
    if (follow_up_window_completed != nullptr)
      *follow_up_window_completed = completed;
    this->phase_ = PilotPhase::REPLYING;
    return true;
  }

  void on_phase_idle() {
    this->close_mic_();
    this->phase_ = PilotPhase::IDLE;
    if (this->follow_up_stage_ != FollowUpStage::PREPARED &&
        this->follow_up_stage_ != FollowUpStage::READY) {
      this->revoke_wake_();
    }
  }

  bool prepare_follow_up(uint32_t token, uint32_t session_nonce) {
    FollowUpAdmissionContext admission;
    admission.token = token;
    admission.request_session_nonce = session_nonce;
    admission.active_session_nonce = this->session_nonce_;
    admission.message_shape_valid = true;
    admission.token_replayed_or_history_full =
        contains_(this->follow_up_token_history_, this->follow_up_token_history_count_, token) ||
        this->follow_up_token_history_count_ >= this->follow_up_token_history_.size();
    admission.physical_wake_active = this->active_wake_;
    admission.one_shot_consumed = this->one_shot_spent_;
    admission.replying = this->phase_ == PilotPhase::REPLYING;
    admission.closed_single_turn = true;
    admission.connected = this->connected_ && this->auth_mode_ == PilotAuthMode::NONCE;
    admission.microphone_muted = this->muted_;
    admission.microphone_streaming = this->mic_open_;
    admission.enrollment_active = this->enrollment_;
    admission.competing_control_active = false;
    admission.active_request = this->follow_up_stage_ != FollowUpStage::NONE;
    admission.callback_in_flight = false;
    if (!should_accept_follow_up(admission)) {
      this->revoke_wake_();
      return false;
    }

    this->follow_up_token_history_[this->follow_up_token_history_count_++] = token;
    this->one_shot_spent_ = true;
    this->follow_up_stage_ = FollowUpStage::PREPARED;
    this->credentials_ = {token, session_nonce, this->wake_generation_, 0, 0};
    return true;
  }

  bool ready_nonce_available(uint32_t ready_nonce) const {
    return ready_nonce != 0 && ready_nonce <= kProtocolTokenMax &&
           ready_nonce != this->credentials_.token && ready_nonce != this->session_nonce_ &&
           !contains_(this->ready_nonce_history_, this->ready_nonce_history_count_, ready_nonce) &&
           this->ready_nonce_history_count_ < this->ready_nonce_history_.size();
  }

  bool mark_follow_up_ready(uint32_t token, uint32_t session_nonce, uint32_t ready_nonce,
                            uint32_t audio_generation) {
    // A delayed local callback from an older transaction is a no-op. It must
    // not revoke a newer wake that now owns different credentials.
    if (token != this->credentials_.token ||
        session_nonce != this->credentials_.session_nonce)
      return false;
    if (!this->base_safe_() || this->post_stop_ || !this->active_wake_ || this->mic_open_ ||
        this->phase_ != PilotPhase::IDLE || this->follow_up_stage_ != FollowUpStage::PREPARED ||
        session_nonce != this->session_nonce_ || !this->ready_nonce_available(ready_nonce)) {
      this->revoke_wake_();
      return false;
    }
    this->ready_nonce_history_[this->ready_nonce_history_count_++] = ready_nonce;
    this->credentials_.ready_nonce = ready_nonce;
    this->credentials_.audio_generation = audio_generation;
    this->follow_up_stage_ = FollowUpStage::READY;
    return true;
  }

  bool commit_is_safe(uint32_t token, uint32_t session_nonce, uint32_t ready_nonce,
                      uint32_t audio_generation,
                      bool announcement_path_clear = true) const {
    return this->base_safe_() && !this->post_stop_ && this->active_wake_ && !this->pending_wake_ &&
           !this->mic_open_ && this->phase_ == PilotPhase::IDLE &&
           this->follow_up_stage_ == FollowUpStage::READY && token == this->credentials_.token &&
           session_nonce == this->credentials_.session_nonce && session_nonce == this->session_nonce_ &&
            ready_nonce == this->credentials_.ready_nonce &&
            audio_generation == this->credentials_.audio_generation &&
            announcement_path_clear;
  }

  bool open_follow_up_after_commit(uint32_t token, uint32_t session_nonce, uint32_t ready_nonce,
                                    uint32_t audio_generation,
                                    bool announcement_path_clear = true) {
    if (!this->commit_is_safe(token, session_nonce, ready_nonce, audio_generation,
                              announcement_path_clear)) {
      this->revoke_wake_();
      return false;
    }
    this->follow_up_stage_ = FollowUpStage::OPEN;
    this->mic_open_ = true;
    this->phase_ = PilotPhase::WAITING;
    return true;
  }

  bool hard_timeout_matches(uint32_t wake_generation, uint32_t ready_nonce) const {
    return this->follow_up_stage_ == FollowUpStage::OPEN && this->active_wake_ &&
           wake_generation != 0 && wake_generation == this->credentials_.wake_generation &&
           ready_nonce != 0 && ready_nonce == this->credentials_.ready_nonce;
  }

  bool silent_wake_timeout_matches(uint32_t wake_generation) const {
    return this->active_wake_ && this->mic_open_ && wake_generation != 0 &&
           wake_generation == this->wake_generation_ && this->phase_ == PilotPhase::WAITING;
  }

  bool absolute_session_timeout_matches(uint32_t wake_generation) const {
    return this->active_wake_ && wake_generation != 0 &&
           wake_generation == this->wake_generation_;
  }

  bool trusted_phase_matches(uint32_t session_nonce,
                              uint32_t wake_generation) const {
    return this->connected_ && this->auth_mode_ == PilotAuthMode::NONCE &&
           this->active_wake_ && session_nonce != 0 &&
           session_nonce == this->session_nonce_ && wake_generation != 0 &&
           wake_generation == this->wake_generation_;
  }

  PhaseApplyResult apply_trusted_phase(PilotPhase target,
                                       uint32_t session_nonce,
                                       uint32_t wake_generation,
                                       bool microphone_muted) {
    if (!this->connected_ || this->auth_mode_ != PilotAuthMode::NONCE ||
        session_nonce == 0 || session_nonce != this->session_nonce_)
      return {};
    // A delayed phase from an older wake on the same admitted session cannot
    // mutate the current owner. This comparison happens before mute or any
    // other lifecycle state is touched.
    if (!this->active_wake_ || wake_generation == 0 ||
        wake_generation != this->wake_generation_) {
      PhaseApplyResult stale;
      stale.status = PhaseApplyStatus::STALE;
      stale.previous = this->phase_;
      stale.target = target;
      stale.active_after = this->active_wake_;
      stale.connection_generation = this->connection_generation_;
      stale.session_nonce = this->session_nonce_;
      stale.wake_generation = this->wake_generation_;
      stale.effect_epoch = this->effect_epoch_;
      return stale;
    }
    this->set_muted(microphone_muted);
    if (!this->active_wake_)
      return {};
    return this->apply_owned_phase_(target, false);
  }

  PhaseApplyResult apply_legacy_phase(PilotPhase target,
                                      bool microphone_muted,
                                      bool keep_mic_open) {
    if (!this->connected_ || this->auth_mode_ != PilotAuthMode::LEGACY_ZERO)
      return {};
    if (!this->active_wake_ && target == PilotPhase::IDLE) {
      const PilotPhase previous = this->phase_;
      this->phase_ = PilotPhase::IDLE;
      this->effect_epoch_ = next_generation_(this->effect_epoch_);
      return this->make_phase_result_(previous, target, false, false, false);
    }
    this->set_muted(microphone_muted);
    if (!this->active_wake_)
      return {};
    return this->apply_owned_phase_(target, keep_mic_open);
  }

  bool assistant_audio(uint32_t &generation) {
    this->audio_generation_ = next_generation_(this->audio_generation_);
    generation = this->audio_generation_;
    const bool invalidates_follow_up =
        this->follow_up_stage_ == FollowUpStage::READY ||
        (this->follow_up_stage_ == FollowUpStage::OPEN &&
         this->phase_ != PilotPhase::THINKING &&
         this->phase_ != PilotPhase::REPLYING) ||
        (this->follow_up_stage_ == FollowUpStage::PREPARED && this->phase_ != PilotPhase::REPLYING);
    if (invalidates_follow_up)
      this->revoke_wake_();
    return invalidates_follow_up;
  }

  void revoke() { this->revoke_wake_(); }

  void stop() {
    this->post_stop_ = true;
    this->revoke_wake_();
  }

  bool close_if_wake_generation(uint32_t wake_generation) {
    if (!this->active_wake_ || wake_generation == 0 ||
        wake_generation != this->wake_generation_)
      return false;
    this->revoke_wake_();
    return true;
  }

  bool abort_pending_wake(uint32_t reservation) {
    if (!this->pending_wake_ || reservation == 0 ||
        reservation != this->pending_reservation_)
      return false;
    this->revoke_wake_();
    return true;
  }

  bool connected() const { return this->connected_; }
  bool authorized() const { return this->auth_mode_ != PilotAuthMode::NONE; }
  bool trusted() const { return this->auth_mode_ == PilotAuthMode::NONCE; }
  bool legacy_zero() const { return this->auth_mode_ == PilotAuthMode::LEGACY_ZERO; }
  bool muted() const { return this->muted_; }
  bool mic_open() const { return this->mic_open_; }
  bool active_wake() const { return this->active_wake_; }
  bool enrollment() const { return this->enrollment_; }
  bool pending_wake() const { return this->pending_wake_; }
  bool one_shot_spent() const { return this->one_shot_spent_; }
  bool post_stop() const { return this->post_stop_; }
  uint32_t session_nonce() const { return this->session_nonce_; }
  uint32_t wake_generation() const { return this->wake_generation_; }
  uint32_t mic_epoch() const { return this->mic_epoch_; }
  uint32_t connection_generation() const { return this->connection_generation_; }
  uint32_t audio_generation() const { return this->audio_generation_; }
  uint32_t effect_epoch() const { return this->effect_epoch_; }
  bool effect_epoch_matches(uint32_t effect_epoch) const {
    return effect_epoch != 0 && effect_epoch == this->effect_epoch_;
  }
  bool phase_effect_plan_is_current(const PhaseApplyResult &plan) const {
    return plan.status == PhaseApplyStatus::APPLIED && plan.effect_epoch != 0 &&
           plan.connection_generation == this->connection_generation_ &&
           plan.session_nonce == this->session_nonce_ &&
           plan.wake_generation == this->wake_generation_ &&
           plan.effect_epoch == this->effect_epoch_;
  }
  PilotAuthMode auth_mode() const { return this->auth_mode_; }
  PilotPhase phase() const { return this->phase_; }
  FollowUpStage follow_up_stage() const { return this->follow_up_stage_; }
  const FollowUpCredentials &credentials() const { return this->credentials_; }

 private:
  static constexpr size_t kHistorySize = 256;

  bool base_safe_() const {
    return this->connected_ && this->auth_mode_ != PilotAuthMode::NONE && !this->muted_ &&
           !this->enrollment_;
  }

  PhaseApplyResult apply_owned_phase_(PilotPhase target,
                                      bool keep_mic_open) {
    const PilotPhase previous = this->phase_;
    const bool mic_was_open = this->mic_open_;
    const bool follow_up_was_open =
        this->follow_up_stage_ == FollowUpStage::OPEN;
    bool applied = false;
    bool follow_up_window_completed = false;
    switch (target) {
      case PilotPhase::LISTENING:
        applied = this->on_phase_listening();
        break;
      case PilotPhase::THINKING:
        // Trusted callers always pass keep_mic_open=false. Legacy zero mode
        // retains its previous barge-in behavior for backend 0.20.6.
        applied = this->on_phase_thinking(!keep_mic_open);
        break;
      case PilotPhase::REPLYING:
        applied = this->on_phase_replying(
            keep_mic_open, &follow_up_window_completed);
        break;
      case PilotPhase::IDLE:
        this->on_phase_idle();
        applied = true;
        follow_up_window_completed = follow_up_was_open;
        break;
      default:
        this->revoke_wake_();
        break;
    }
    if (!applied)
      return {};
    this->effect_epoch_ = next_generation_(this->effect_epoch_);
    const bool mic_closed = mic_was_open && !this->mic_open_;
    return this->make_phase_result_(
        previous, target, mic_closed, follow_up_was_open && mic_closed,
        follow_up_window_completed);
  }

  PhaseApplyResult make_phase_result_(
      PilotPhase previous, PilotPhase target, bool mic_closed,
      bool follow_up_input_ended, bool follow_up_window_completed) const {
    PhaseApplyResult result;
    result.status = PhaseApplyStatus::APPLIED;
    result.previous = previous;
    result.target = target;
    result.mic_closed = mic_closed;
    result.follow_up_input_ended = follow_up_input_ended;
    result.follow_up_window_completed = follow_up_window_completed;
    result.active_after = this->active_wake_;
    result.connection_generation = this->connection_generation_;
    result.session_nonce = this->session_nonce_;
    result.wake_generation = this->wake_generation_;
    result.effect_epoch = this->effect_epoch_;
    return result;
  }

  void revoke_wake_() {
    this->effect_epoch_ = next_generation_(this->effect_epoch_);
    this->close_mic_();
    this->pending_wake_ = false;
    this->pending_reservation_ = 0;
    this->transmitted_reservation_ = 0;
    this->active_wake_ = false;
    this->follow_up_stage_ = FollowUpStage::NONE;
    this->credentials_ = {};
  }

  void close_mic_() {
    if (this->mic_open_)
      this->mic_epoch_ = next_generation_(this->mic_epoch_);
    this->mic_open_ = false;
  }

  static uint32_t next_generation_(uint32_t current) {
    return current == 0 || current >= kProtocolTokenMax ? 1 : current + 1;
  }

  static bool contains_(const std::array<uint32_t, kHistorySize> &values, size_t count,
                        uint32_t value) {
    for (size_t i = 0; i < count; i++) {
      if (values[i] == value)
        return true;
    }
    return false;
  }

  bool connected_{false};
  bool muted_{false};
  bool enrollment_{false};
  bool post_stop_{false};
  bool pending_wake_{false};
  bool active_wake_{false};
  bool mic_open_{false};
  bool one_shot_spent_{true};
  PilotAuthMode auth_mode_{PilotAuthMode::NONE};
  PilotPhase phase_{PilotPhase::IDLE};
  FollowUpStage follow_up_stage_{FollowUpStage::NONE};
  uint32_t session_nonce_{0};
  uint32_t last_trusted_session_nonce_{0};
  uint32_t connection_generation_{0};
  uint32_t reservation_generation_{0};
  uint32_t pending_reservation_{0};
  uint32_t transmitted_reservation_{0};
  uint32_t last_reserved_wake_{0};
  uint32_t reserved_protocol_wake_generation_{0};
  uint32_t wake_generation_{0};
  uint32_t mic_epoch_{0};
  uint32_t audio_generation_{0};
  uint32_t effect_epoch_{0};
  FollowUpCredentials credentials_{};
  std::array<uint32_t, kHistorySize> session_nonce_history_{};
  size_t session_nonce_history_count_{0};
  std::array<uint32_t, kHistorySize> follow_up_token_history_{};
  size_t follow_up_token_history_count_{0};
  std::array<uint32_t, kHistorySize> ready_nonce_history_{};
  size_t ready_nonce_history_count_{0};
};

}  // namespace va_client
}  // namespace esphome
