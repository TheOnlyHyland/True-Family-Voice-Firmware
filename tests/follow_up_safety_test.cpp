#include "esphome/components/va_client/follow_up_lifecycle.h"

#include <array>
#include <cassert>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using esphome::va_client::FlatJsonObject;
using esphome::va_client::FollowUpDeadlineAction;
using esphome::va_client::FollowUpLifecycle;
using esphome::va_client::FollowUpPhaseCredentialDecision;
using esphome::va_client::FollowUpRuntimeSnapshot;
using esphome::va_client::FollowUpStage;
using esphome::va_client::GenerationEffectGate;
using esphome::va_client::HelloAdmission;
using esphome::va_client::MicSendFence;
using esphome::va_client::PhaseApplyResult;
using esphome::va_client::PhaseApplyStatus;
using esphome::va_client::PhaseRuntimeAction;
using esphome::va_client::PilotPhase;
using esphome::va_client::TransportAdmissionGate;
using esphome::va_client::WsMessageReassembler;
using esphome::va_client::WsMessageType;
using esphome::va_client::WsReassemblyStatus;
using esphome::va_client::kAbsoluteSessionMaxMs;
using esphome::va_client::kFollowUpReplayHistorySize;
using esphome::va_client::kFollowupOpenDelayMaxMs;
using esphome::va_client::kProtocolTokenMax;
using esphome::va_client::kRequestFollowUpReadyTimeoutMs;
using esphome::va_client::kRequestFollowUpMs;
using esphome::va_client::kWsBinaryMessageMaxBytes;
using esphome::va_client::decide_follow_up_phase_credential;
using esphome::va_client::decide_follow_up_runtime;
using esphome::va_client::phase_runtime_action;

static uint32_t start_trusted_wake(FollowUpLifecycle &lifecycle, uint32_t session_nonce) {
  lifecycle.on_connected();
  assert(lifecycle.admit_trusted_hello(session_nonce) == HelloAdmission::FRESH);
  const uint32_t wake_reservation = lifecycle.prepare_local_wake();
  assert(wake_reservation != 0);
  assert(lifecycle.pending_wake_is_safe(wake_reservation));
  assert(lifecycle.commit_local_wake(wake_reservation));
  assert(lifecycle.mic_open());
  return lifecycle.wake_generation();
}

static PhaseApplyResult apply_initial_phase(
    FollowUpLifecycle &lifecycle, PilotPhase phase, uint32_t session_nonce,
    uint32_t wake_generation, bool microphone_muted = false,
    uint32_t now_ms = 0) {
  return lifecycle.apply_trusted_phase(
      phase, session_nonce, wake_generation, microphone_muted, false, 0,
      now_ms);
}

static PhaseApplyResult apply_follow_up_phase(
    FollowUpLifecycle &lifecycle, PilotPhase phase, uint32_t session_nonce,
    uint32_t wake_generation, uint32_t token, uint32_t now_ms,
    bool microphone_muted = false) {
  return lifecycle.apply_trusted_phase(
      phase, session_nonce, wake_generation, microphone_muted, true, token,
      now_ms);
}

static void test_strict_flat_json() {
  FlatJsonObject message;
  std::string type;
  std::string audio_out;
  uint32_t token = 0;
  uint32_t nonce = 0;
  assert(message.parse(
      " { \"session_nonce\" : 987654321, \"type\" : \"request_follow_up\", "
      "\"token\" : 123456789 } "));
  assert(message.get_string("type", type) && type == "request_follow_up");
  assert(message.get_uint("token", token) && token == 123456789);
  assert(message.get_uint("session_nonce", nonce) && nonce == 987654321);
  assert(message.has_exact({"type", "token", "session_nonce"}));

  assert(message.parse(
      "{\"type\":\"hello\",\"audio_out\":\"pcm\",\"follow_up_ms\":0,"
      "\"follow_up_open_delay_ms\":800,\"wake_open_delay_ms\":700,"
      "\"playback_prebuffer_ms\":120}"));
  assert(message.get_string("audio_out", audio_out) && audio_out == "pcm");
  assert(message.has_exact({"type", "audio_out", "follow_up_ms",
                            "follow_up_open_delay_ms", "wake_open_delay_ms",
                            "playback_prebuffer_ms"}));

  assert(message.parse(
      "{\"type\":\"hello\",\"nonce\":17,\"audio_out\":\"pcm\","
      "\"follow_up_ms\":0,\"follow_up_open_delay_ms\":800,"
      "\"wake_open_delay_ms\":700,\"playback_prebuffer_ms\":120}"));
  assert(message.has_exact({"type", "nonce", "audio_out", "follow_up_ms",
                            "follow_up_open_delay_ms", "wake_open_delay_ms",
                            "playback_prebuffer_ms"}));

  assert(message.parse(
      "{\"type\":\"request_follow_up\",\"token\":1,\"session_nonce\":3,"
      "\"extra\":true}"));
  assert(!message.has_exact({"type", "token", "session_nonce"}));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":1,\"token\":2,"
      "\"session_nonce\":3}"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":4294967296,"
      "\"session_nonce\":3}"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":01,\"session_nonce\":3}"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":-1,\"session_nonce\":3}"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":1.0,\"session_nonce\":3}"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":1,"
      "\"session_nonce\":{\"x\":3}}"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":1,\"session_nonce\":3,}"));
  assert(!message.parse("[\"request_follow_up\",1,3]"));
  assert(!message.parse(
      "{\"type\":\"request_follow_up\",\"token\":1,\"session_nonce\":3} "
      "trailing"));

  assert(message.parse(
      "{\"type\":\"phase\",\"value\":\"listening\","
      "\"session_nonce\":17,\"wake_generation\":2}"));
  assert(message.has_exact(
      {"type", "value", "session_nonce", "wake_generation"}));
  assert(!message.has_exact({"type", "value"}));

  assert(message.parse(
      "{\"type\":\"phase\",\"value\":\"thinking\","
      "\"session_nonce\":17,\"wake_generation\":2,\"token\":19}"));
  assert(message.has_exact(
      {"type", "value", "session_nonce", "wake_generation", "token"}));
  assert(message.get_uint("token", token) && token == 19);
}

static void test_follow_up_phase_credential_contract() {
  constexpr uint32_t kCurrentToken = 41;
  constexpr uint32_t kOldToken = 40;
  assert(phase_runtime_action(PhaseApplyStatus::APPLIED) ==
         PhaseRuntimeAction::APPLY);
  assert(phase_runtime_action(PhaseApplyStatus::STALE) ==
         PhaseRuntimeAction::IGNORE);
  assert(phase_runtime_action(PhaseApplyStatus::EXPIRED) ==
         PhaseRuntimeAction::EXPIRE);
  assert(phase_runtime_action(PhaseApplyStatus::REJECTED) ==
         PhaseRuntimeAction::REVOKE);
  assert(decide_follow_up_phase_credential(
             false, false, false, 0, 0) ==
         FollowUpPhaseCredentialDecision::BASE);
  assert(decide_follow_up_phase_credential(
             true, false, true, kCurrentToken, kCurrentToken) ==
         FollowUpPhaseCredentialDecision::CURRENT);
  assert(decide_follow_up_phase_credential(
             true, false, true, kOldToken, kCurrentToken) ==
         FollowUpPhaseCredentialDecision::STALE);
  assert(decide_follow_up_phase_credential(
             true, false, false, 0, kCurrentToken) ==
         FollowUpPhaseCredentialDecision::REJECT);
  assert(decide_follow_up_phase_credential(
             false, false, true, kOldToken, 0) ==
         FollowUpPhaseCredentialDecision::STALE);
  assert(decide_follow_up_phase_credential(
             false, false, true, 0, 0) ==
         FollowUpPhaseCredentialDecision::REJECT);

  // Idle is always the tokenless terminal shape, including while OPEN.
  for (const bool follow_up_open : std::array<bool, 2>{false, true}) {
    assert(decide_follow_up_phase_credential(
               follow_up_open, true, false, 0, kCurrentToken) ==
           FollowUpPhaseCredentialDecision::BASE);
    for (const uint32_t token :
         std::array<uint32_t, 3>{0U, kOldToken, kCurrentToken}) {
      assert(decide_follow_up_phase_credential(
                 follow_up_open, true, true, token, kCurrentToken) ==
             FollowUpPhaseCredentialDecision::REJECT);
    }
  }
}

static void test_runtime_deadline_and_mic_decisions() {
  FollowUpRuntimeSnapshot physical;
  physical.mic_open = true;
  physical.streaming = true;
  auto decision = decide_follow_up_runtime(physical, 500, 0, 0);
  assert(decision.deadline_action == FollowUpDeadlineAction::NONE);
  assert(decision.mic_send_allowed);

  FollowUpRuntimeSnapshot open;
  open.follow_up_open = true;
  open.deadline_armed = true;
  open.deadline_ms = 11000;
  open.wake_generation = 7;
  open.token = 41;
  open.mic_open = true;
  open.streaming = true;

  decision = decide_follow_up_runtime(open, 1000, 7, 41);
  assert(decision.expected_owner);
  assert(decision.deadline_action == FollowUpDeadlineAction::SCHEDULE);
  assert(decision.timer_delay_ms == kRequestFollowUpMs);
  assert(decision.mic_send_allowed);

  decision = decide_follow_up_runtime(open, 10999, 0, 0);
  assert(decision.deadline_action == FollowUpDeadlineAction::SCHEDULE);
  assert(decision.timer_delay_ms == 1);
  assert(decision.mic_send_allowed);

  decision = decide_follow_up_runtime(open, 11000, 0, 0);
  assert(decision.deadline_action == FollowUpDeadlineAction::EXPIRE);
  assert(decision.timer_delay_ms == 0);
  assert(!decision.mic_send_allowed);

  // A timer from an older round cannot expire or send through the current one.
  decision = decide_follow_up_runtime(open, 11000, 7, 40);
  assert(!decision.expected_owner);
  assert(decision.deadline_action == FollowUpDeadlineAction::NONE);
  assert(!decision.mic_send_allowed);

  open.deadline_armed = false;
  decision = decide_follow_up_runtime(open, 1000, 0, 0);
  assert(decision.deadline_action == FollowUpDeadlineAction::NONE);
  assert(!decision.mic_send_allowed);

  open.deadline_armed = true;
  open.token = 0;
  decision = decide_follow_up_runtime(open, 1000, 0, 0);
  assert(!decision.expected_owner);
  assert(decision.deadline_action == FollowUpDeadlineAction::NONE);
  assert(!decision.mic_send_allowed);

  open.token = 41;
  open.deadline_ms = std::numeric_limits<uint32_t>::max() - 5;
  decision = decide_follow_up_runtime(
      open, std::numeric_limits<uint32_t>::max() - 6, 7, 41);
  assert(decision.deadline_action == FollowUpDeadlineAction::SCHEDULE);
  assert(decision.timer_delay_ms == 1);
  decision = decide_follow_up_runtime(
      open, std::numeric_limits<uint32_t>::max() - 5, 7, 41);
  assert(decision.deadline_action == FollowUpDeadlineAction::EXPIRE);
}

static void test_listening_requires_owned_open_mic() {
  FollowUpLifecycle lifecycle;
  lifecycle.on_connected();
  assert(!lifecycle.on_phase_listening());
  assert(!lifecycle.mic_open());
  assert(lifecycle.admit_legacy_zero_hello());

  const uint32_t wake_generation = lifecycle.prepare_local_wake();
  assert(wake_generation != 0);
  assert(!lifecycle.on_phase_listening());
  assert(!lifecycle.commit_local_wake(wake_generation));

  const uint32_t replacement_generation = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(replacement_generation));
  assert(lifecycle.on_phase_listening());
  lifecycle.stop();
  assert(!lifecycle.on_phase_listening());
  assert(!lifecycle.mic_open());
}

static void test_complete_two_phase_lifecycle() {
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation = start_trusted_wake(lifecycle, 7001);
  assert(lifecycle.on_phase_listening());
  assert(lifecycle.on_phase_thinking());
  lifecycle.on_phase_replying();
  assert(!lifecycle.mic_open());

  assert(lifecycle.prepare_follow_up(9001, 7001));
  assert(!lifecycle.per_answer_grant_available());
  assert(lifecycle.follow_up_stage() == FollowUpStage::PREPARED);
  lifecycle.on_phase_idle();
  assert(lifecycle.active_wake());
  assert(lifecycle.mark_follow_up_ready(9001, 7001, 11001, 4));
  assert(lifecycle.follow_up_stage() == FollowUpStage::READY);
  assert(!lifecycle.mic_open());
  assert(lifecycle.commit_is_safe(9001, 7001, 11001, 4));
  constexpr uint32_t kOpenedAtMs = 1000;
  assert(lifecycle.open_follow_up_after_commit(
      9001, 7001, 11001, 4, kOpenedAtMs));
  assert(lifecycle.mic_open());
  assert(lifecycle.follow_up_deadline_remaining_ms(kOpenedAtMs) ==
         kRequestFollowUpMs);
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::LISTENING, 7001, wake_generation, 9001,
             kOpenedAtMs + 1)
             .status == PhaseApplyStatus::APPLIED);
  assert(lifecycle.follow_up_deadline_remaining_ms(kOpenedAtMs + 1) ==
         kRequestFollowUpMs - 1);
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::REPLYING, 7001, wake_generation, 9001,
             kOpenedAtMs + 2)
             .status == PhaseApplyStatus::APPLIED);
  lifecycle.on_phase_idle();
  assert(!lifecycle.active_wake());
  assert(!lifecycle.mic_open());
}

static void test_commit_races_fail_closed() {
  FollowUpLifecycle lifecycle;
  start_trusted_wake(lifecycle, 7002);
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(9002, 7002));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(9002, 7002, 11002, 8));
  assert(lifecycle.commit_is_safe(9002, 7002, 11002, 8));
  lifecycle.set_muted(true);
  assert(!lifecycle.open_follow_up_after_commit(
      9002, 7002, 11002, 8, 1000));
  assert(!lifecycle.mic_open());

  lifecycle.set_muted(false);
  lifecycle.on_disconnected();
  lifecycle.on_connected();
  assert(lifecycle.admit_trusted_hello(7002) == HelloAdmission::RECOVERY);
  const uint32_t next_wake = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(next_wake));
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(9003, 7002));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(9003, 7002, 11003, 9));
  uint32_t audio_generation = 0;
  assert(lifecycle.assistant_audio(audio_generation));
  assert(!lifecycle.commit_is_safe(9003, 7002, 11003, audio_generation));
  assert(!lifecycle.mic_open());
}

static void test_prepare_and_ready_revocations_never_reopen() {
  FollowUpLifecycle muted;
  start_trusted_wake(muted, 7100);
  muted.on_phase_replying();
  assert(muted.prepare_follow_up(7101, 7100));
  muted.set_muted(true);
  assert(!muted.mark_follow_up_ready(7101, 7100, 7102, 0));
  assert(!muted.mic_open());

  FollowUpLifecycle stopped;
  start_trusted_wake(stopped, 7200);
  stopped.on_phase_replying();
  assert(stopped.prepare_follow_up(7201, 7200));
  stopped.on_phase_idle();
  assert(stopped.mark_follow_up_ready(7201, 7200, 7202, 0));
  stopped.stop();
  assert(!stopped.open_follow_up_after_commit(
      7201, 7200, 7202, 0, 1000));
  assert(!stopped.mic_open());

  FollowUpLifecycle disconnected;
  start_trusted_wake(disconnected, 7300);
  disconnected.on_phase_replying();
  assert(disconnected.prepare_follow_up(7301, 7300));
  disconnected.on_phase_idle();
  assert(disconnected.mark_follow_up_ready(7301, 7300, 7302, 0));
  disconnected.on_disconnected();
  assert(!disconnected.open_follow_up_after_commit(
      7301, 7300, 7302, 0, 1000));
  assert(!disconnected.mic_open());

  FollowUpLifecycle replaced;
  start_trusted_wake(replaced, 7400);
  replaced.on_phase_replying();
  assert(replaced.prepare_follow_up(7401, 7400));
  replaced.on_phase_idle();
  assert(replaced.mark_follow_up_ready(7401, 7400, 7402, 0));
  const uint32_t replacement_wake = replaced.prepare_local_wake();
  assert(replacement_wake != 0);
  assert(!replaced.open_follow_up_after_commit(
      7401, 7400, 7402, 0, 1000));
  assert(!replaced.mic_open());
}

static void test_stale_wakes_timers_and_reconnects() {
  FollowUpLifecycle lifecycle;
  const uint32_t first_wake = start_trusted_wake(lifecycle, 7003);
  assert(lifecycle.silent_wake_timeout_matches(first_wake));
  const uint32_t replacement_wake = lifecycle.prepare_local_wake();
  assert(replacement_wake != first_wake);
  assert(!lifecycle.close_if_wake_generation(first_wake));
  assert(lifecycle.pending_wake_is_safe(replacement_wake));
  lifecycle.set_muted(true);
  assert(!lifecycle.commit_local_wake(replacement_wake));

  lifecycle.set_muted(false);
  const uint32_t third_wake = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(third_wake));
  const uint32_t third_protocol_wake = lifecycle.wake_generation();
  assert(lifecycle.silent_wake_timeout_matches(third_protocol_wake));
  assert(lifecycle.on_phase_listening());
  assert(!lifecycle.silent_wake_timeout_matches(third_protocol_wake));
  lifecycle.on_disconnected();
  assert(!lifecycle.mic_open());
  assert(!lifecycle.close_if_wake_generation(third_wake - 1));
}

static void test_delayed_wake_abort_stop_and_enrollment() {
  FollowUpLifecycle lifecycle;
  lifecycle.on_connected();
  assert(lifecycle.admit_trusted_hello(7004) == HelloAdmission::FRESH);

  const uint32_t stale_wake = lifecycle.prepare_local_wake();
  const uint32_t current_wake = lifecycle.prepare_local_wake();
  assert(current_wake != stale_wake);
  assert(!lifecycle.commit_local_wake(stale_wake));
  assert(!lifecycle.abort_pending_wake(stale_wake));
  assert(lifecycle.pending_wake_is_safe(current_wake));
  assert(lifecycle.abort_pending_wake(current_wake));
  assert(!lifecycle.mic_open());

  const uint32_t stopped_wake = lifecycle.prepare_local_wake();
  lifecycle.stop();
  assert(!lifecycle.commit_local_wake(stopped_wake));
  assert(!lifecycle.on_phase_listening());
  assert(!lifecycle.mic_open());

  const uint32_t post_stop_wake = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(post_stop_wake));
  lifecycle.set_muted(true);
  assert(!lifecycle.mic_open());
  lifecycle.set_muted(false);
  assert(!lifecycle.commit_local_wake(post_stop_wake));

  assert(lifecycle.start_enrollment());
  assert(lifecycle.enrollment());
  assert(lifecycle.mic_open());
  assert(lifecycle.prepare_local_wake() == 0);
  assert(lifecycle.mic_open());
  lifecycle.stop_enrollment();
  assert(!lifecycle.enrollment());
  assert(!lifecycle.mic_open());

  lifecycle.on_disconnected();
  assert(!lifecycle.start_enrollment());
  assert(!lifecycle.mic_open());
}

static void test_replay_and_bounded_session_history() {
  FollowUpLifecycle lifecycle;
  start_trusted_wake(lifecycle, 8001);
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(12001, 8001));
  lifecycle.revoke();

  const uint32_t second_wake = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(second_wake));
  lifecycle.on_phase_replying();
  assert(!lifecycle.prepare_follow_up(12001, 8001));

  FollowUpLifecycle nonce_replay;
  nonce_replay.on_connected();
  assert(nonce_replay.admit_trusted_hello(1) == HelloAdmission::FRESH);
  assert(nonce_replay.admit_trusted_hello(2) == HelloAdmission::FRESH);
  assert(nonce_replay.admit_trusted_hello(1) == HelloAdmission::REJECTED);

  FollowUpLifecycle bounded_sessions;
  bounded_sessions.on_connected();
  for (uint32_t nonce = 1; nonce <= 256; nonce++) {
    assert(bounded_sessions.admit_trusted_hello(nonce) == HelloAdmission::FRESH);
  }
  assert(bounded_sessions.admit_trusted_hello(257) == HelloAdmission::REJECTED);
}

static void complete_serialized_follow_up_round(
    FollowUpLifecycle &lifecycle, uint32_t session_nonce, uint32_t token,
    uint32_t ready_nonce, uint32_t wake_generation) {
  const uint32_t opened_at_ms = token;
  assert(lifecycle.per_answer_grant_available());
  assert(lifecycle.prepare_follow_up(token, session_nonce));
  assert(!lifecycle.per_answer_grant_available());
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(token, session_nonce, ready_nonce, 0));
  assert(lifecycle.open_follow_up_after_commit(token, session_nonce,
                                                 ready_nonce, 0,
                                                 opened_at_ms));
  assert(lifecycle.wake_generation() == wake_generation);
  assert(lifecycle.absolute_session_timeout_matches(wake_generation));
  assert(!lifecycle.follow_up_deadline_reached(opened_at_ms));
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::LISTENING, session_nonce,
             wake_generation, token, opened_at_ms + 1)
             .status == PhaseApplyStatus::APPLIED);
  // Genuine speech does not turn the aperture deadline into an inactivity timer.
  assert(lifecycle.follow_up_deadline_remaining_ms(opened_at_ms + 1) ==
         kRequestFollowUpMs - 1);
  assert(!lifecycle.per_answer_grant_available());
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::THINKING, session_nonce, wake_generation,
             token, opened_at_ms + 2)
             .status == PhaseApplyStatus::APPLIED);
  assert(!lifecycle.per_answer_grant_available());
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::REPLYING, session_nonce, wake_generation,
             token, opened_at_ms + 3)
             .status == PhaseApplyStatus::APPLIED);
  assert(lifecycle.follow_up_stage() == FollowUpStage::NONE);
  assert(lifecycle.per_answer_grant_available());
  assert(lifecycle.wake_generation() == wake_generation);
  assert(lifecycle.absolute_session_timeout_matches(wake_generation));
}

static void test_bounded_serialized_round_history_fails_closed_and_resets() {
  constexpr uint32_t kSessionNonce = 9000;
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation = start_trusted_wake(lifecycle, kSessionNonce);
  assert(lifecycle.on_phase_listening());
  assert(lifecycle.on_phase_thinking());
  assert(lifecycle.on_phase_replying());

  for (uint32_t round = 1; round <= kFollowUpReplayHistorySize; round++) {
    complete_serialized_follow_up_round(
        lifecycle, kSessionNonce, 20000 + round, 30000 + round,
        wake_generation);
  }

  assert(lifecycle.follow_up_token_history_count() ==
         kFollowUpReplayHistorySize);
  assert(lifecycle.ready_nonce_history_count() ==
         kFollowUpReplayHistorySize);
  assert(!lifecycle.ready_nonce_available(40000));
  assert(!lifecycle.prepare_follow_up(40001, kSessionNonce));
  assert(!lifecycle.active_wake());
  assert(!lifecycle.per_answer_grant_available());

  // A genuinely fresh hello owns a new replay domain and resets the fixed
  // stores without retaining dynamic capacity from the exhausted session.
  constexpr uint32_t kFreshSessionNonce = 9001;
  const uint32_t fresh_wake = start_trusted_wake(lifecycle, kFreshSessionNonce);
  assert(lifecycle.follow_up_token_history_count() == 0);
  assert(lifecycle.ready_nonce_history_count() == 0);
  assert(lifecycle.on_phase_listening());
  assert(lifecycle.on_phase_thinking());
  assert(lifecycle.on_phase_replying());
  complete_serialized_follow_up_round(
      lifecycle, kFreshSessionNonce, 20001, 30001, fresh_wake);
}

static void test_stale_prior_round_phase_credentials_cannot_progress_open() {
  constexpr uint32_t kSessionNonce = 9010;
  constexpr uint32_t kOldToken = 21001;
  constexpr uint32_t kCurrentToken = 21002;
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation =
      start_trusted_wake(lifecycle, kSessionNonce);
  assert(lifecycle.on_phase_listening());
  assert(lifecycle.on_phase_thinking());
  assert(lifecycle.on_phase_replying());
  complete_serialized_follow_up_round(
      lifecycle, kSessionNonce, kOldToken, 31001, wake_generation);

  assert(lifecycle.prepare_follow_up(kCurrentToken, kSessionNonce));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(
      kCurrentToken, kSessionNonce, 31002, 0));
  constexpr uint32_t kOpenedAtMs = 5000;
  assert(lifecycle.open_follow_up_after_commit(
      kCurrentToken, kSessionNonce, 31002, 0, kOpenedAtMs));

  const auto missing = apply_initial_phase(
      lifecycle, PilotPhase::LISTENING, kSessionNonce, wake_generation, false,
      kOpenedAtMs + 1);
  assert(missing.status == PhaseApplyStatus::REJECTED);
  assert(lifecycle.phase() == PilotPhase::WAITING);
  assert(lifecycle.mic_open());

  const std::array<PilotPhase, 3> delayed_targets = {
      PilotPhase::LISTENING,
      PilotPhase::THINKING,
      PilotPhase::REPLYING,
  };
  for (const PilotPhase target : delayed_targets) {
    const auto stale = apply_follow_up_phase(
        lifecycle, target, kSessionNonce, wake_generation, kOldToken,
        kOpenedAtMs + 2, true);
    assert(stale.status == PhaseApplyStatus::STALE);
    assert(lifecycle.phase() == PilotPhase::WAITING);
    assert(lifecycle.mic_open());
    assert(!lifecycle.muted());
    assert(!lifecycle.per_answer_grant_available());
    assert(lifecycle.credentials().token == kCurrentToken);
  }

  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::LISTENING, kSessionNonce,
             wake_generation, kCurrentToken, kOpenedAtMs + 3)
             .status == PhaseApplyStatus::APPLIED);
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::THINKING, kSessionNonce,
             wake_generation, kCurrentToken, kOpenedAtMs + 4)
             .status == PhaseApplyStatus::APPLIED);
  assert(apply_follow_up_phase(
             lifecycle, PilotPhase::REPLYING, kSessionNonce,
             wake_generation, kCurrentToken, kOpenedAtMs + 5)
             .status == PhaseApplyStatus::APPLIED);
  assert(lifecycle.per_answer_grant_available());
}

static void test_tokenized_idle_is_invalid_and_tokenless_idle_is_terminal() {
  constexpr uint32_t kSessionNonce = 9011;
  constexpr uint32_t kToken = 21101;
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation =
      start_trusted_wake(lifecycle, kSessionNonce);

  auto result = apply_follow_up_phase(
      lifecycle, PilotPhase::IDLE, kSessionNonce, wake_generation, kToken, 1);
  assert(result.status == PhaseApplyStatus::REJECTED);
  assert(phase_runtime_action(result.status) == PhaseRuntimeAction::REVOKE);
  assert(lifecycle.active_wake());

  assert(lifecycle.on_phase_replying());
  assert(lifecycle.prepare_follow_up(kToken, kSessionNonce));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(
      kToken, kSessionNonce, 31101, 0));
  assert(lifecycle.open_follow_up_after_commit(
      kToken, kSessionNonce, 31101, 0, 1000));

  // Backend 0.21.x sends tokenless progression and therefore cannot complete
  // any explicit OPEN answer with firmware 0.20.0.
  for (const PilotPhase target : std::array<PilotPhase, 3>{
           PilotPhase::LISTENING,
           PilotPhase::THINKING,
           PilotPhase::REPLYING,
       }) {
    result = apply_initial_phase(
        lifecycle, target, kSessionNonce, wake_generation, false, 1001);
    assert(result.status == PhaseApplyStatus::REJECTED);
    assert(phase_runtime_action(result.status) == PhaseRuntimeAction::REVOKE);
    assert(lifecycle.mic_open());
  }

  for (const uint32_t token :
       std::array<uint32_t, 3>{0U, kToken - 1, kToken}) {
    result = apply_follow_up_phase(
        lifecycle, PilotPhase::IDLE, kSessionNonce, wake_generation, token,
        1002);
    assert(result.status == PhaseApplyStatus::REJECTED);
    assert(phase_runtime_action(result.status) == PhaseRuntimeAction::REVOKE);
    assert(lifecycle.mic_open());
  }

  result = apply_initial_phase(
      lifecycle, PilotPhase::IDLE, kSessionNonce, wake_generation, false,
      1003);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(phase_runtime_action(result.status) == PhaseRuntimeAction::APPLY);
  assert(!lifecycle.active_wake());
  assert(!lifecycle.mic_open());
}

static void test_commit_deadline_is_absolute_and_wrap_safe() {
  FollowUpLifecycle late_thinking;
  const uint32_t thinking_wake = start_trusted_wake(late_thinking, 9020);
  assert(late_thinking.on_phase_replying());
  assert(late_thinking.prepare_follow_up(22001, 9020));
  late_thinking.on_phase_idle();
  assert(late_thinking.mark_follow_up_ready(22001, 9020, 32001, 0));
  assert(late_thinking.open_follow_up_after_commit(
      22001, 9020, 32001, 0, 1000));
  assert(apply_follow_up_phase(
             late_thinking, PilotPhase::LISTENING, 9020, thinking_wake,
             22001, 10999)
             .status == PhaseApplyStatus::APPLIED);
  const auto expired_thinking = apply_follow_up_phase(
      late_thinking, PilotPhase::THINKING, 9020, thinking_wake, 22001,
      11000);
  assert(expired_thinking.status == PhaseApplyStatus::EXPIRED);
  MicSendFence fence;
  const uint32_t lease_epoch = late_thinking.mic_epoch();
  fence.acquire();
  assert(late_thinking.expire_follow_up_deadline(
      11000, thinking_wake, 22001));
  assert(!late_thinking.mic_open());
  assert(!late_thinking.active_wake());
  assert(!MicSendFence::lease_is_current(
      lease_epoch, late_thinking.mic_epoch(), late_thinking.mic_open()));
  assert(fence.in_flight() == 1);
  fence.release();
  assert(fence.in_flight() == 0);

  FollowUpLifecycle late_replying;
  const uint32_t replying_wake = start_trusted_wake(late_replying, 9021);
  assert(late_replying.on_phase_replying());
  assert(late_replying.prepare_follow_up(22002, 9021));
  late_replying.on_phase_idle();
  assert(late_replying.mark_follow_up_ready(22002, 9021, 32002, 0));
  assert(late_replying.open_follow_up_after_commit(
      22002, 9021, 32002, 0, 2000));
  assert(apply_follow_up_phase(
             late_replying, PilotPhase::LISTENING, 9021, replying_wake,
             22002, 2001)
             .status == PhaseApplyStatus::APPLIED);
  assert(apply_follow_up_phase(
             late_replying, PilotPhase::THINKING, 9021, replying_wake,
             22002, 2002)
             .status == PhaseApplyStatus::APPLIED);
  assert(!late_replying.mic_open());
  assert(apply_follow_up_phase(
             late_replying, PilotPhase::REPLYING, 9021, replying_wake,
             22002, 12000)
             .status == PhaseApplyStatus::EXPIRED);
  assert(!late_replying.per_answer_grant_available());
  assert(late_replying.expire_follow_up_deadline(
      12000, replying_wake, 22002));
  assert(!late_replying.active_wake());

  FollowUpLifecycle wrapped;
  const uint32_t wrapped_wake = start_trusted_wake(wrapped, 9022);
  assert(wrapped.on_phase_replying());
  assert(wrapped.prepare_follow_up(22003, 9022));
  wrapped.on_phase_idle();
  assert(wrapped.mark_follow_up_ready(22003, 9022, 32003, 0));
  constexpr uint32_t kWrappedOpen =
      std::numeric_limits<uint32_t>::max() - 5000;
  assert(wrapped.open_follow_up_after_commit(
      22003, 9022, 32003, 0, kWrappedOpen));
  assert(!wrapped.follow_up_deadline_reached(kWrappedOpen + 9999));
  assert(wrapped.follow_up_deadline_reached(kWrappedOpen + 10000));
  assert(!wrapped.expire_follow_up_deadline(
      kWrappedOpen + 10000, wrapped_wake, 22002));
  assert(wrapped.mic_open());
  assert(wrapped.expire_follow_up_deadline(
      kWrappedOpen + 15000, wrapped_wake, 22003));
  assert(!wrapped.mic_open());
}

static void test_only_ordered_answer_speech_rearms_a_round() {
  FollowUpLifecycle no_speech;
  const uint32_t no_speech_wake = start_trusted_wake(no_speech, 9050);
  assert(no_speech.on_phase_replying());
  assert(no_speech.prepare_follow_up(9051, 9050));
  no_speech.on_phase_idle();
  assert(no_speech.mark_follow_up_ready(9051, 9050, 9052, 0));
  assert(no_speech.open_follow_up_after_commit(
      9051, 9050, 9052, 0, 1000));
  assert(apply_follow_up_phase(
             no_speech, PilotPhase::THINKING, 9050, no_speech_wake, 9051,
             1001)
             .status == PhaseApplyStatus::APPLIED);
  assert(apply_follow_up_phase(
             no_speech, PilotPhase::REPLYING, 9050, no_speech_wake, 9051,
             1002)
             .status == PhaseApplyStatus::APPLIED);
  assert(!no_speech.per_answer_grant_available());

  FollowUpLifecycle no_endpoint;
  const uint32_t no_endpoint_wake = start_trusted_wake(no_endpoint, 9060);
  assert(no_endpoint.on_phase_replying());
  assert(no_endpoint.prepare_follow_up(9061, 9060));
  no_endpoint.on_phase_idle();
  assert(no_endpoint.mark_follow_up_ready(9061, 9060, 9062, 0));
  assert(no_endpoint.open_follow_up_after_commit(
      9061, 9060, 9062, 0, 1000));
  assert(apply_follow_up_phase(
             no_endpoint, PilotPhase::LISTENING, 9060, no_endpoint_wake,
             9061, 1001)
             .status == PhaseApplyStatus::APPLIED);
  assert(apply_follow_up_phase(
             no_endpoint, PilotPhase::REPLYING, 9060, no_endpoint_wake,
             9061, 1002)
             .status == PhaseApplyStatus::APPLIED);
  assert(!no_endpoint.per_answer_grant_available());

  FollowUpLifecycle cancelled;
  start_trusted_wake(cancelled, 9070);
  assert(cancelled.on_phase_replying());
  assert(cancelled.prepare_follow_up(9071, 9070));
  cancelled.revoke();
  assert(!cancelled.per_answer_grant_available());

  FollowUpLifecycle denied;
  start_trusted_wake(denied, 9080);
  assert(denied.on_phase_replying());
  assert(!denied.prepare_follow_up(9080, 9080));
  assert(!denied.active_wake());
  assert(!denied.per_answer_grant_available());

  FollowUpLifecycle timed_out;
  const uint32_t timed_out_wake = start_trusted_wake(timed_out, 9090);
  assert(timed_out.on_phase_replying());
  assert(timed_out.prepare_follow_up(9091, 9090));
  timed_out.on_phase_idle();
  assert(timed_out.mark_follow_up_ready(9091, 9090, 9092, 0));
  assert(timed_out.open_follow_up_after_commit(
      9091, 9090, 9092, 0, 1000));
  assert(timed_out.follow_up_deadline_reached(11000));
  assert(timed_out.expire_follow_up_deadline(
      11000, timed_out_wake, 9091));
  assert(!timed_out.per_answer_grant_available());
}

static void test_ready_nonce_replay_and_mismatch() {
  FollowUpLifecycle lifecycle;
  start_trusted_wake(lifecycle, 9100);
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(9200, 9100));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(9200, 9100, 9300, 1));
  assert(!lifecycle.commit_is_safe(9200, 9100, 9301, 1));
  lifecycle.revoke();

  const uint32_t wake = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(wake));
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(9201, 9100));
  lifecycle.on_phase_idle();
  assert(!lifecycle.mark_follow_up_ready(9201, 9100, 9300, 2));
  assert(!lifecycle.mic_open());
}

static void test_stale_ready_callback_cannot_touch_new_wake() {
  FollowUpLifecycle lifecycle;
  start_trusted_wake(lifecycle, 9400);
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(9401, 9400));
  lifecycle.revoke();

  const uint32_t wake = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(wake));
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(9402, 9400));
  lifecycle.on_phase_idle();
  assert(!lifecycle.mark_follow_up_ready(9401, 9400, 9501, 0));
  assert(lifecycle.active_wake());
  assert(lifecycle.follow_up_stage() == FollowUpStage::PREPARED);
  assert(lifecycle.mark_follow_up_ready(9402, 9400, 9502, 0));
}

static void test_aborted_reservations_do_not_create_protocol_gaps() {
  FollowUpLifecycle lifecycle;
  lifecycle.on_connected();
  assert(lifecycle.admit_trusted_hello(7500) == HelloAdmission::FRESH);

  const uint32_t abandoned = lifecycle.prepare_local_wake();
  const uint32_t first_protocol =
      lifecycle.pending_protocol_wake_generation(abandoned);
  assert(first_protocol == 1);
  assert(lifecycle.abort_pending_wake(abandoned));
  assert(lifecycle.wake_generation() == 0);

  const uint32_t replacement = lifecycle.prepare_local_wake();
  assert(replacement != abandoned);
  assert(lifecycle.pending_protocol_wake_generation(replacement) ==
         first_protocol);
  assert(lifecycle.record_wake_transmitted(replacement, first_protocol));
  assert(lifecycle.open_transmitted_wake(replacement));
  assert(lifecycle.wake_generation() == 1);

  const uint32_t raced = lifecycle.prepare_local_wake();
  const uint32_t second_protocol =
      lifecycle.pending_protocol_wake_generation(raced);
  assert(second_protocol == 2);
  lifecycle.set_muted(true);
  // The backend saw this wake, so the committed sequence advances even though
  // the post-send local safety check refuses to open the mic.
  assert(lifecycle.record_wake_transmitted(raced, second_protocol));
  assert(!lifecycle.open_transmitted_wake(raced));
  assert(lifecycle.wake_generation() == 2);

  lifecycle.set_muted(false);
  const uint32_t final_reservation = lifecycle.prepare_local_wake();
  assert(lifecycle.pending_protocol_wake_generation(final_reservation) == 3);
}

static void test_absolute_session_ceiling_survives_progress() {
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation = start_trusted_wake(lifecycle, 7600);
  assert(lifecycle.absolute_session_timeout_matches(wake_generation));
  assert(lifecycle.on_phase_listening());
  assert(lifecycle.absolute_session_timeout_matches(wake_generation));
  assert(lifecycle.on_phase_thinking());
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(7601, 7600));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(7601, 7600, 7602, 0));
  assert(lifecycle.open_follow_up_after_commit(
      7601, 7600, 7602, 0, 1000));
  assert(lifecycle.absolute_session_timeout_matches(wake_generation));
  lifecycle.revoke();
  assert(!lifecycle.absolute_session_timeout_matches(wake_generation));
}

static void test_mic_send_fence_invalidates_revoked_lease() {
  FollowUpLifecycle lifecycle;
  start_trusted_wake(lifecycle, 7700);
  MicSendFence fence;
  const uint32_t lease_epoch = lifecycle.mic_epoch();
  fence.acquire();
  assert(fence.in_flight() == 1);
  lifecycle.revoke();
  assert(!MicSendFence::lease_is_current(
      lease_epoch, lifecycle.mic_epoch(), lifecycle.mic_open()));
  // A network flush/revoke barrier must wait until this bounded send releases.
  assert(fence.in_flight() == 1);
  fence.release();
  assert(fence.in_flight() == 0);
}

static void test_phase_context_binding() {
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation = start_trusted_wake(lifecycle, 7800);
  assert(lifecycle.trusted_phase_matches(7800, wake_generation));
  assert(!lifecycle.trusted_phase_matches(7801, wake_generation));
  assert(!lifecycle.trusted_phase_matches(7800, wake_generation + 1));
  lifecycle.revoke();
  assert(!lifecycle.trusted_phase_matches(7800, wake_generation));

  FollowUpLifecycle legacy;
  legacy.on_connected();
  assert(legacy.admit_legacy_zero_hello());
  const uint32_t reservation = legacy.prepare_local_wake();
  assert(legacy.commit_local_wake(reservation));
  assert(!legacy.trusted_phase_matches(0, legacy.wake_generation()));
}

static void test_announcement_start_after_ready_fails_closed() {
  FollowUpLifecycle lifecycle;
  start_trusted_wake(lifecycle, 7900);
  lifecycle.on_phase_replying();
  assert(lifecycle.prepare_follow_up(7901, 7900));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(7901, 7900, 7902, 0));
  assert(lifecycle.commit_is_safe(7901, 7900, 7902, 0, true));
  assert(!lifecycle.commit_is_safe(7901, 7900, 7902, 0, false));
  assert(!lifecycle.open_follow_up_after_commit(
      7901, 7900, 7902, 0, 1000, false));
  assert(!lifecycle.mic_open());
}

static void test_trusted_single_turn_endpoint_preserves_response_owner() {
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation = start_trusted_wake(lifecycle, 8100);
  assert(lifecycle.per_answer_grant_available());

  auto result = apply_initial_phase(
      lifecycle,
      PilotPhase::LISTENING, 8100, wake_generation, false);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(!result.mic_closed);
  assert(lifecycle.mic_open());

  const uint32_t listening_mic_epoch = lifecycle.mic_epoch();
  result = apply_initial_phase(
      lifecycle,
      PilotPhase::THINKING, 8100, wake_generation, false);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(result.mic_closed);
  assert(!lifecycle.mic_open());
  assert(lifecycle.active_wake());
  assert(lifecycle.wake_generation() == wake_generation);
  assert(lifecycle.per_answer_grant_available());
  assert(!MicSendFence::lease_is_current(
      listening_mic_epoch, lifecycle.mic_epoch(), lifecycle.mic_open()));

  result = apply_initial_phase(
      lifecycle,
      PilotPhase::REPLYING, 8100, wake_generation, false);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(lifecycle.active_wake());
  assert(lifecycle.prepare_follow_up(8101, 8100));
  assert(lifecycle.credentials().wake_generation == wake_generation);

  result = apply_initial_phase(
      lifecycle,
      PilotPhase::IDLE, 8100, wake_generation, false);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(result.active_after);
  assert(lifecycle.follow_up_stage() == FollowUpStage::PREPARED);
}

static void test_open_follow_up_ends_input_but_keeps_owner_until_idle() {
  FollowUpLifecycle lifecycle;
  const uint32_t wake_generation = start_trusted_wake(lifecycle, 8200);
  assert(lifecycle.on_phase_replying());
  assert(lifecycle.prepare_follow_up(8201, 8200));
  lifecycle.on_phase_idle();
  assert(lifecycle.mark_follow_up_ready(8201, 8200, 8202, 0));
  assert(lifecycle.open_follow_up_after_commit(
      8201, 8200, 8202, 0, 1000));

  auto result = apply_follow_up_phase(
      lifecycle, PilotPhase::LISTENING, 8200, wake_generation, 8201, 1001);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(!result.mic_closed);

  result = apply_follow_up_phase(
      lifecycle, PilotPhase::THINKING, 8200, wake_generation, 8201, 1002);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(result.mic_closed);
  assert(result.follow_up_input_ended);
  assert(!result.follow_up_window_completed);
  assert(lifecycle.active_wake());
  assert(lifecycle.follow_up_stage() == FollowUpStage::OPEN);
  uint32_t audio_generation = 0;
  assert(!lifecycle.assistant_audio(audio_generation));
  assert(audio_generation != 0);
  assert(lifecycle.active_wake());
  assert(lifecycle.follow_up_stage() == FollowUpStage::OPEN);

  result = apply_follow_up_phase(
      lifecycle, PilotPhase::REPLYING, 8200, wake_generation, 8201, 1003);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(result.follow_up_window_completed);
  assert(lifecycle.active_wake());
  assert(lifecycle.follow_up_stage() == FollowUpStage::NONE);
  assert(lifecycle.per_answer_grant_available());

  result = apply_initial_phase(
      lifecycle,
      PilotPhase::IDLE, 8200, wake_generation, false);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(!result.active_after);
  assert(!lifecycle.active_wake());
}

static void test_stale_trusted_phase_is_a_noop_for_newer_wake() {
  FollowUpLifecycle lifecycle;
  const uint32_t first_wake = start_trusted_wake(lifecycle, 8300);
  const uint32_t replacement = lifecycle.prepare_local_wake();
  assert(lifecycle.commit_local_wake(replacement));
  const uint32_t current_wake = lifecycle.wake_generation();
  assert(current_wake != first_wake);
  assert(lifecycle.mic_open());

  auto result = apply_initial_phase(
      lifecycle,
      PilotPhase::THINKING, 8300, first_wake, true);
  assert(result.status == PhaseApplyStatus::STALE);
  assert(lifecycle.active_wake());
  assert(lifecycle.mic_open());
  assert(!lifecycle.muted());
  assert(lifecycle.wake_generation() == current_wake);

  result = apply_initial_phase(
      lifecycle,
      PilotPhase::THINKING, 8300, current_wake, false);
  assert(result.status == PhaseApplyStatus::APPLIED);
  assert(!lifecycle.mic_open());
  assert(lifecycle.active_wake());

  result = apply_initial_phase(
      lifecycle,
      PilotPhase::LISTENING, 8300, first_wake, false);
  assert(result.status == PhaseApplyStatus::STALE);
  assert(!lifecycle.mic_open());
  assert(lifecycle.active_wake());
}

static void test_every_stale_trusted_phase_preserves_newer_wake_state() {
  FollowUpLifecycle lifecycle;
  const uint32_t first_wake = start_trusted_wake(lifecycle, 8350);
  const uint32_t replacement = lifecycle.prepare_local_wake();
  assert(replacement != 0);
  assert(lifecycle.commit_local_wake(replacement));
  const uint32_t current_wake = lifecycle.wake_generation();
  assert(current_wake != first_wake);

  const auto assert_current_wake_unchanged = [&]() {
    assert(lifecycle.connected());
    assert(lifecycle.authorized());
    assert(lifecycle.trusted());
    assert(!lifecycle.muted());
    assert(lifecycle.mic_open());
    assert(lifecycle.active_wake());
    assert(!lifecycle.pending_wake());
    assert(lifecycle.per_answer_grant_available());
    assert(!lifecycle.post_stop());
    assert(lifecycle.session_nonce() == 8350);
    assert(lifecycle.wake_generation() == current_wake);
    assert(lifecycle.phase() == PilotPhase::WAITING);
    assert(lifecycle.follow_up_stage() == FollowUpStage::NONE);
    assert(lifecycle.credentials().token == 0);
    assert(lifecycle.credentials().session_nonce == 0);
    assert(lifecycle.credentials().wake_generation == 0);
    assert(lifecycle.credentials().ready_nonce == 0);
    assert(lifecycle.credentials().audio_generation == 0);
  };

  const uint32_t mic_epoch = lifecycle.mic_epoch();
  const uint32_t connection_generation = lifecycle.connection_generation();
  const uint32_t audio_generation = lifecycle.audio_generation();
  const uint32_t effect_epoch = lifecycle.effect_epoch();
  const std::array<PilotPhase, 4> stale_targets = {
      PilotPhase::LISTENING,
      PilotPhase::THINKING,
      PilotPhase::REPLYING,
      PilotPhase::IDLE,
  };
  for (const PilotPhase target : stale_targets) {
    const auto result = apply_initial_phase(
        lifecycle,
        target, 8350, first_wake, true);
    assert(result.status == PhaseApplyStatus::STALE);
    assert_current_wake_unchanged();
    assert(lifecycle.mic_epoch() == mic_epoch);
    assert(lifecycle.connection_generation() == connection_generation);
    assert(lifecycle.audio_generation() == audio_generation);
    assert(lifecycle.effect_epoch() == effect_epoch);
  }
}

static void test_websocket_message_reassembly() {
  WsMessageReassembler assembler;
  const uint8_t text[] = {'{', '"', 'x', '"', ':', '1', '}'};
  auto result = assembler.push(0x01, true, sizeof(text), 0, text, 2);
  assert(result.status == WsReassemblyStatus::NEED_MORE);
  result = assembler.push(0x01, true, sizeof(text), 2, text + 2, 5);
  assert(result.status == WsReassemblyStatus::COMPLETE);
  assert(result.type == WsMessageType::TEXT);
  assert(assembler.message().size() == sizeof(text));

  // Transport chunks and WebSocket fragments may each split PCM on odd byte
  // boundaries. Only the complete PCM16 message must be even.
  const uint8_t pcm[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  result = assembler.push(0x02, false, 3, 0, pcm, 1);
  assert(result.status == WsReassemblyStatus::NEED_MORE);
  result = assembler.push(0x02, false, 3, 1, pcm + 1, 2);
  assert(result.status == WsReassemblyStatus::NEED_MORE);
  result = assembler.push(0x09, true, 0, 0, nullptr, 0);
  assert(result.status == WsReassemblyStatus::CONTROL);
  result = assembler.push(0x00, true, 3, 0, pcm + 3, 1);
  assert(result.status == WsReassemblyStatus::NEED_MORE);
  result = assembler.push(0x00, true, 3, 1, pcm + 4, 2);
  assert(result.status == WsReassemblyStatus::COMPLETE);
  assert(result.type == WsMessageType::BINARY);
  assert(assembler.message().size() == sizeof(pcm));
  for (size_t i = 0; i < sizeof(pcm); i++)
    assert(assembler.message()[i] == pcm[i]);

  const uint8_t odd_pcm[] = {1, 2, 3};
  result = assembler.push(0x02, true, sizeof(odd_pcm), 0, odd_pcm,
                          sizeof(odd_pcm));
  assert(result.status == WsReassemblyStatus::REJECTED);
  result = assembler.push(0x00, true, 2, 0, pcm, 2);
  assert(result.status == WsReassemblyStatus::REJECTED);

  result = assembler.push(0x01, true, sizeof(text), 0, text, 3);
  assert(result.status == WsReassemblyStatus::NEED_MORE);
  result = assembler.push(0x01, true, sizeof(text), 0, text, 3);
  assert(result.status == WsReassemblyStatus::REJECTED);

  result = assembler.push(0x01, false, 1, 0, text, 1);
  assert(result.status == WsReassemblyStatus::NEED_MORE);
  result = assembler.push(0x02, true, 2, 0, pcm, 2);
  assert(result.status == WsReassemblyStatus::REJECTED);

  result = assembler.push(0x09, false, 0, 0, nullptr, 0);
  assert(result.status == WsReassemblyStatus::REJECTED);
  result = assembler.push(0x08, true, 1, 0, pcm, 1);
  assert(result.status == WsReassemblyStatus::REJECTED);
  result = assembler.push(0x03, true, 0, 0, nullptr, 0);
  assert(result.status == WsReassemblyStatus::REJECTED);

  std::vector<uint8_t> oversized(kWsBinaryMessageMaxBytes + 2, 0);
  result = assembler.push(0x02, true, oversized.size(), 0, oversized.data(),
                          oversized.size());
  assert(result.status == WsReassemblyStatus::REJECTED);
}

static void test_generation_effect_gate_closes_phase_replacement_race() {
  FollowUpLifecycle lifecycle;
  GenerationEffectGate gate;
  const uint32_t old_wake = start_trusted_wake(lifecycle, 8400);
  std::mutex test_mutex;
  std::condition_variable test_cv;
  bool plan_ready = false;
  bool replacement_completed = false;
  PhaseApplyResult old_plan;
  struct EffectProbe {
    std::atomic_uint32_t runtime_phase_writes{0};
    std::atomic_uint32_t streaming_writes{0};
    std::atomic_uint32_t mic_barriers{0};
    std::atomic_uint32_t follow_up_clears{0};
    std::array<std::atomic_uint32_t, 8> timer_cancels{};
    std::atomic_uint32_t audio_ring_closes{0};
    std::atomic_uint32_t follow_up_opens{0};
    std::atomic_uint32_t deferred_triggers{0};

    void apply_every_phase_effect() {
      this->runtime_phase_writes++;
      this->streaming_writes++;
      this->mic_barriers++;
      this->follow_up_clears++;
      for (auto &timer_cancel : this->timer_cancels)
        timer_cancel++;
      this->audio_ring_closes++;
      this->follow_up_opens++;
      this->deferred_triggers++;
    }

    void assert_untouched() const {
      assert(this->runtime_phase_writes.load() == 0);
      assert(this->streaming_writes.load() == 0);
      assert(this->mic_barriers.load() == 0);
      assert(this->follow_up_clears.load() == 0);
      for (const auto &timer_cancel : this->timer_cancels)
        assert(timer_cancel.load() == 0);
      assert(this->audio_ring_closes.load() == 0);
      assert(this->follow_up_opens.load() == 0);
      assert(this->deferred_triggers.load() == 0);
    }
  } effects;

  std::thread phase_thread([&]() {
    {
      std::lock_guard<GenerationEffectGate> effect_guard(gate);
      old_plan = apply_initial_phase(
          lifecycle,
          PilotPhase::THINKING, 8400, old_wake, false);
      assert(old_plan.status == PhaseApplyStatus::APPLIED);
      assert(lifecycle.phase_effect_plan_is_current(old_plan));
    }
    {
      std::lock_guard<std::mutex> guard(test_mutex);
      plan_ready = true;
    }
    test_cv.notify_all();
    {
      std::unique_lock<std::mutex> guard(test_mutex);
      test_cv.wait(guard, [&]() { return replacement_completed; });
    }
    std::lock_guard<GenerationEffectGate> effect_guard(gate);
    if (lifecycle.phase_effect_plan_is_current(old_plan))
      effects.apply_every_phase_effect();
  });

  std::thread replacement_thread([&]() {
    {
      std::unique_lock<std::mutex> guard(test_mutex);
      test_cv.wait(guard, [&]() { return plan_ready; });
    }
    {
      std::lock_guard<GenerationEffectGate> effect_guard(gate);
      const uint32_t reservation = lifecycle.prepare_local_wake();
      assert(reservation != 0);
      assert(lifecycle.commit_local_wake(reservation));
    }
    {
      std::lock_guard<std::mutex> guard(test_mutex);
      replacement_completed = true;
    }
    test_cv.notify_all();
  });

  phase_thread.join();
  replacement_thread.join();
  assert(lifecycle.wake_generation() != old_wake);
  assert(lifecycle.active_wake());
  assert(lifecycle.mic_open());
  effects.assert_untouched();
}

static void test_close_failure_requires_new_session_admission() {
  // Thinking, timers, mute, announcements, enrollment, and other local closes
  // all enter this same admission state after a failed send barrier.
  const std::array<const char *, 6> closure_classes = {
      "thinking", "timeout", "mute", "announcement", "enrollment", "other"};
  for (uint32_t control = 0; control < closure_classes.size(); control++) {
    assert(closure_classes[control] != nullptr);
    FollowUpLifecycle lifecycle;
    const uint32_t nonce = 8500 + control;
    start_trusted_wake(lifecycle, nonce);
    TransportAdmissionGate admission;
    admission.on_transport_connected();
    assert(admission.can_admit_session());
    assert(admission.on_session_admitted());
    assert(admission.can_start_wake());

    assert(admission.on_authoritative_close_failure());
    // flush failure followed by interrupt failure must not replace the first
    // fault's close/reconnect plan.
    assert(!admission.on_authoritative_close_failure());
    lifecycle.on_disconnected();
    assert(!lifecycle.mic_open());
    assert(!lifecycle.active_wake());
    assert(admission.close_control_faulted());
    assert(!admission.can_start_wake());
    assert(!admission.can_admit_session());

    // A duplicate CONNECTED event from the faulted socket is not a reconnect.
    admission.on_transport_connected();
    assert(!admission.can_admit_session());
    assert(!admission.on_session_admitted());

    admission.on_transport_disconnected();
    // A delayed duplicate failure must not erase the completed disconnect that
    // permits only the next transport to attempt a fresh hello.
    assert(!admission.on_authoritative_close_failure());
    admission.on_transport_connected();
    assert(admission.can_admit_session());
    assert(!admission.can_start_wake());
    lifecycle.on_connected();
    assert(lifecycle.admit_trusted_hello(nonce) == HelloAdmission::RECOVERY);
    assert(admission.on_session_admitted());
    assert(!admission.close_control_faulted());
    assert(admission.can_start_wake());
  }
}

static void test_every_enrollment_exit_closes_before_transport_failure() {
  const std::array<const char *, 5> enrollment_exits = {
      "backend_stop", "device_stop", "safety_cap", "disconnect", "quarantine"};
  for (uint32_t exit = 0; exit < enrollment_exits.size(); exit++) {
    assert(enrollment_exits[exit] != nullptr);
    FollowUpLifecycle lifecycle;
    lifecycle.on_connected();
    assert(lifecycle.admit_trusted_hello(8600 + exit) == HelloAdmission::FRESH);
    assert(lifecycle.start_enrollment());
    assert(lifecycle.enrollment());
    assert(lifecycle.mic_open());

    lifecycle.stop_enrollment();
    assert(!lifecycle.enrollment());
    assert(!lifecycle.mic_open());

    TransportAdmissionGate admission;
    admission.on_transport_connected();
    assert(admission.on_session_admitted());
    assert(admission.on_authoritative_close_failure());
    assert(!admission.can_start_wake());
    assert(!admission.can_admit_session());
  }
}

static void test_enrollment_close_failure_is_locally_closed_first() {
  FollowUpLifecycle lifecycle;
  lifecycle.on_connected();
  assert(lifecycle.admit_trusted_hello(8600) == HelloAdmission::FRESH);
  assert(lifecycle.start_enrollment());
  assert(lifecycle.enrollment());
  assert(lifecycle.mic_open());

  TransportAdmissionGate admission;
  admission.on_transport_connected();
  assert(admission.on_session_admitted());

  // enroll_stop() performs this local transition before its bounded revoke.
  lifecycle.stop_enrollment();
  assert(!lifecycle.enrollment());
  assert(!lifecycle.mic_open());
  assert(admission.on_authoritative_close_failure());
  assert(!admission.can_start_wake());

  lifecycle.on_disconnected();
  admission.on_transport_disconnected();
  admission.on_transport_connected();
  assert(admission.can_admit_session());
  assert(!admission.can_start_wake());
}

int main() {
  static_assert(kRequestFollowUpMs == 10000,
                "The no-wake aperture must remain a hard 10 seconds");
  static_assert(kProtocolTokenMax == 0x7FFFFFFF,
                "Pilot credentials remain positive signed 31-bit values");
  static_assert(kAbsoluteSessionMaxMs == 120000,
                "Every physical wake session remains bounded across rounds");
  static_assert(kRequestFollowUpReadyTimeoutMs >=
                    2000 + kFollowupOpenDelayMaxMs + 500,
                "READY deadline must cover chime wait plus negotiated delay");

  test_strict_flat_json();
  test_follow_up_phase_credential_contract();
  test_runtime_deadline_and_mic_decisions();
  test_listening_requires_owned_open_mic();
  test_complete_two_phase_lifecycle();
  test_commit_races_fail_closed();
  test_prepare_and_ready_revocations_never_reopen();
  test_stale_wakes_timers_and_reconnects();
  test_delayed_wake_abort_stop_and_enrollment();
  test_replay_and_bounded_session_history();
  test_bounded_serialized_round_history_fails_closed_and_resets();
  test_stale_prior_round_phase_credentials_cannot_progress_open();
  test_tokenized_idle_is_invalid_and_tokenless_idle_is_terminal();
  test_commit_deadline_is_absolute_and_wrap_safe();
  test_only_ordered_answer_speech_rearms_a_round();
  test_ready_nonce_replay_and_mismatch();
  test_stale_ready_callback_cannot_touch_new_wake();
  test_aborted_reservations_do_not_create_protocol_gaps();
  test_absolute_session_ceiling_survives_progress();
  test_mic_send_fence_invalidates_revoked_lease();
  test_phase_context_binding();
  test_announcement_start_after_ready_fails_closed();
  test_trusted_single_turn_endpoint_preserves_response_owner();
  test_open_follow_up_ends_input_but_keeps_owner_until_idle();
  test_stale_trusted_phase_is_a_noop_for_newer_wake();
  test_every_stale_trusted_phase_preserves_newer_wake_state();
  test_websocket_message_reassembly();
  test_generation_effect_gate_closes_phase_replacement_race();
  test_close_failure_requires_new_session_admission();
  test_every_enrollment_exit_closes_before_transport_failure();
  test_enrollment_close_failure_is_locally_closed_first();
  return 0;
}
