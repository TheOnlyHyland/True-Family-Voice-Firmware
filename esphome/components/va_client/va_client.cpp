#include "va_client.h"
#include "automation.h"

#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

#include <algorithm>
#include <cstring>

#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

namespace esphome {
namespace va_client {

static const char *const TAG = "va_client";

bool VaClient::send_text_bounded_(const std::string &message, const char *label) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr || message.empty()) {
    ESP_LOGW(TAG, "%s not sent: websocket unavailable", label);
    return false;
  }
  const uint32_t connection_generation = this->ws_connection_generation_.load();
  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  const int sent = esp_websocket_client_send_text(
      handle, message.data(), static_cast<int>(message.size()),
      pdMS_TO_TICKS(kControlSendTimeoutMs));
  const bool exact = sent == static_cast<int>(message.size()) &&
                     this->ws_connected_.load() &&
                     this->ws_connection_generation_.load() == connection_generation;
  if (!exact) {
    ESP_LOGW(TAG, "%s send failed or partial (%d/%u bytes)", label, sent,
             (unsigned) message.size());
  }
  return exact;
}

bool VaClient::send_binary_bounded_(const uint8_t *data, size_t len) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr || data == nullptr || len == 0)
    return false;
  const uint32_t connection_generation = this->ws_connection_generation_.load();
  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  const int sent = esp_websocket_client_send_bin(
      handle, reinterpret_cast<const char *>(data), static_cast<int>(len),
      pdMS_TO_TICKS(kAudioSendTimeoutMs));
  return sent == static_cast<int>(len) && this->ws_connected_.load() &&
         this->ws_connection_generation_.load() == connection_generation;
}

// Free-function trampoline. esp-idf event registration takes a C function
// pointer; we recover the VaClient* from the user_data slot.
static void va_ws_event_handler(void *handler_args, esp_event_base_t /*base*/, int32_t event_id, void *event_data) {
  auto *self = static_cast<VaClient *>(handler_args);
  if (self == nullptr)
    return;
  self->on_ws_event(event_id, event_data);
}

void VaClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up VA Client...");

  if (this->mic_ != nullptr) {
    this->mic_->add_data_callback(
        [this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  } else {
    ESP_LOGE(TAG, "Microphone not configured");
  }

  // Allocate the audio ring buffer in PSRAM (8 MB available, internal RAM
  // is only 320 KB and we don't want to starve wifi/mww).
  this->audio_buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(kAudioBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->audio_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte audio buffer in PSRAM", (unsigned) kAudioBufBytes);
  } else {
    ESP_LOGCONFIG(TAG, "Allocated %u-byte audio ring buffer in PSRAM", (unsigned) kAudioBufBytes);
  }
  this->audio_drain_buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(kAudioDrainChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->audio_drain_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte audio drain buffer in PSRAM",
             (unsigned) kAudioDrainChunkBytes);
  }

  // Allocate the mic pre-roll ring in PSRAM (kPreRollMs of 16 kHz int16 mono).
  this->preroll_capacity_samples_ = (size_t) kPreRollMs * (kMicSampleRate / 1000);
  this->preroll_buf_ = static_cast<int16_t *>(
      heap_caps_malloc(this->preroll_capacity_samples_ * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->preroll_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-sample mic pre-roll buffer in PSRAM",
             (unsigned) this->preroll_capacity_samples_);
    this->preroll_capacity_samples_ = 0;
  } else {
    ESP_LOGCONFIG(TAG, "Allocated %u ms mic pre-roll buffer in PSRAM (%u samples)",
                  (unsigned) kPreRollMs, (unsigned) this->preroll_capacity_samples_);
  }

  // Tell the resampler what format we'll feed it. The resampler converts to
  // its yaml-configured output format (48k 16-bit) before passing to the
  // mixer → i2s leaf. Start the speaker task once so play() calls just push
  // into its ring buffer instead of racing to re-create the i2s channel.
  if (this->speaker_ != nullptr) {
    audio::AudioStreamInfo info(/*bits_per_sample=*/16, /*channels=*/1, /*sample_rate=*/24000);
    this->speaker_->set_audio_stream_info(info);
    this->speaker_->start();
  }

  this->connect_();
}

void VaClient::loop() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  // Drain the audio ring buffer into the speaker. speaker.play() accepts
  // only what fits in its own ring (returns the count actually queued).
  if (this->speaker_ != nullptr && this->audio_buf_ != nullptr &&
      this->audio_drain_buf_ != nullptr) {
    // Snapshot ring state under the lock — head/tail/fill are all
    // mutated from the WS task on the other core.
    portENTER_CRITICAL(&this->ring_mux_);
    size_t head = this->audio_head_;
    size_t tail = this->audio_tail_;
    size_t fill = this->audio_fill_;
    const bool playback_prebuffer_pending = this->playback_prebuffer_pending_;
    const bool playback_priming = this->playback_priming_;
    const uint32_t prime_started_ms = this->prime_started_ms_;
    const uint32_t playback_prebuffer_ms = this->playback_prebuffer_ms_;
    const uint32_t prebuffer_generation = this->prebuffer_generation_;
    portEXIT_CRITICAL(&this->ring_mux_);
    if (fill > 0) {
      // Decide whether this empty→nonempty transition needs a jitter cushion
      // from the main task. Resampler state is not safe to inspect from the WS
      // task, so that task publishes only this pending decision with the ring.
      if (playback_prebuffer_pending) {
        const bool should_prime = !this->speaker_->has_buffered_data();
        portENTER_CRITICAL(&this->ring_mux_);
        if (this->playback_prebuffer_pending_ &&
            this->prebuffer_generation_ == prebuffer_generation) {
          this->playback_prebuffer_pending_ = false;
          this->playback_priming_ = should_prime;
          if (!should_prime)
            this->prime_started_ms_ = 0;
        }
        portEXIT_CRITICAL(&this->ring_mux_);
        return;  // Re-snapshot the resolved state before touching the ring.
      }

      // Accumulate the jitter cushion before warming the downstream chain. If
      // we prime first, a large prebuffer can outlast kChainColdMs and let the
      // resampler stop again before the first real sample reaches it.
      if (playback_priming) {
        bool release_succeeded = false;
        const size_t target =
            (size_t) playback_prebuffer_ms * (kPlaybackSampleRate / 1000) * 2;
        if (fill >= target ||
            (millis() - prime_started_ms) >= playback_prebuffer_ms) {
          portENTER_CRITICAL(&this->ring_mux_);
          if (this->playback_priming_ &&
              this->prebuffer_generation_ == prebuffer_generation) {
            this->playback_priming_ = false;
            release_succeeded = true;
          }
          portEXIT_CRITICAL(&this->ring_mux_);
          if (!release_succeeded)
            return;  // Interrupted or re-armed while this snapshot was in flight.
          ESP_LOGD(TAG, "prebuffer ready (%u bytes) — checking chain before playback",
                   (unsigned) fill);
        } else {
          return;  // Keep accumulating; do not warm the chain prematurely.
        }
      }

      // Resampler cold-start SILENCE-PRIME (crackle fix). The resampler does NOT
      // idle-timeout (verified vs ESPHome source): resample(stop_gracefully=false)
      // never returns FINISHED, and its output mixer-source is timeout:never, so the
      // chain stays WARM between normal replies. It goes COLD only after an explicit
      // `speaker.stop: media_resampling_speaker` (yaml interrupt / "stop" word / wake /
      // follow-up), which tears the task down to STATE_STOPPED. The next reply then
      // cold-starts a fresh AudioResampler whose windowed-sinc FIR begins from a zero
      // state → a startup-transient click. A PSRAM prebuffer can't fix it (the transient
      // is downstream of the ring). Fix: when cold, feed kChainPrimeMs of silence BEFORE
      // the first real sample so the FIR settles to a clean zero output first. We detect
      // "cold" two ways: the resampler actually reporting is_stopped() (true exactly
      // post-speaker.stop — the precise signal) OR, as a backup, nothing fed for
      // > kChainColdMs. is_stopped() closes the window the timer alone misses: a reply
      // whose audio lands < kChainColdMs after a speaker.stop (the residual click). A
      // needless prime on an already-warm chain is harmless (60ms of silence); both
      // signals are only ever true at a real cold reply-start, never mid-speech. The
      // real audio waits safely in PSRAM (and builds a small cushion) until priming done.
      {
        const uint32_t now_ms = millis();
        const bool resampler_cold = this->speaker_->is_stopped() ||
                                    this->last_fed_ms_ == 0 ||
                                    (now_ms - this->last_fed_ms_) > kChainColdMs;
        if (this->chain_prime_remaining_ == 0 && resampler_cold) {
          this->chain_prime_remaining_ =
              (size_t) kChainPrimeMs * (kPlaybackSampleRate / 1000) * 2;  // ms→bytes (mono 16-bit)
          ESP_LOGD(TAG, "resampler cold — priming %u bytes of silence before reply",
                   (unsigned) this->chain_prime_remaining_);
        }
        if (this->chain_prime_remaining_ > 0) {
          static const uint8_t kSilence[480] = {0};  // 10ms @24k mono16; fed in chunks
          size_t want = std::min(this->chain_prime_remaining_, sizeof(kSilence));
          size_t fed = this->speaker_->play(kSilence, want);
          if (fed > 0) {
            this->chain_prime_remaining_ -= fed;
            this->last_fed_ms_ = now_ms;  // count silence as "fed" so cold-check clears
          }
          // Hold real-audio drain until the chain is warmed. Real audio stays in
          // PSRAM. Re-enter loop() next tick to continue/finish priming.
          return;
        }
      }
      // Detector 3: downstream underrun. If the resampler/mixer/i2s chain
      // ran out of bytes to play while we *still* have PSRAM queued,
      // something hiccupped downstream — the user hears silence or a
      // brief stuck-sample glitch. Log the first occurrence per reply so
      // we know whether bad audio in a turn correlates with this.
      if (!this->underrun_logged_this_turn_ && !this->speaker_->has_buffered_data()) {
        ESP_LOGW(TAG, "downstream underrun: %u bytes queued in PSRAM but speaker chain is dry",
                 (unsigned) fill);
        this->underrun_logged_this_turn_ = true;
      }
      // Copy a bounded, generation-checked slice while holding the ring lock.
      // play() can block, so it consumes the private scratch copy outside the
      // lock while the websocket producer remains free to append or reset.
      size_t contiguous = 0;
      portENTER_CRITICAL(&this->ring_mux_);
      if (this->prebuffer_generation_ == prebuffer_generation &&
          this->audio_head_ == head && this->audio_fill_ > 0) {
        tail = this->audio_tail_;
        fill = this->audio_fill_;
        contiguous = (head < tail) ? (tail - head) : (kAudioBufBytes - head);
        contiguous = std::min(contiguous, fill);
        contiguous = std::min(contiguous, kAudioDrainChunkBytes);
        std::memcpy(this->audio_drain_buf_, this->audio_buf_ + head, contiguous);
      }
      portEXIT_CRITICAL(&this->ring_mux_);
      if (contiguous == 0)
        return;
      size_t accepted = this->speaker_->play(this->audio_drain_buf_, contiguous);
      if (accepted > 0) {
        this->last_fed_ms_ = millis();  // keep the chain "warm" for cold-detection
        bool committed = false;
        size_t remaining = 0;
        portENTER_CRITICAL(&this->ring_mux_);
        // An interrupt can reset the ring while play() runs outside the lock.
        // Commit this read only if it still belongs to the exact snapshot;
        // otherwise subtracting from the reset fill would underflow size_t.
        if (this->prebuffer_generation_ == prebuffer_generation &&
            this->audio_head_ == head && this->audio_fill_ >= accepted) {
          this->audio_head_ = (this->audio_head_ + accepted) % kAudioBufBytes;
          this->audio_fill_ -= accepted;
          remaining = this->audio_fill_;
          committed = true;
        }
        portEXIT_CRITICAL(&this->ring_mux_);
        if (!committed)
          return;
        static uint32_t dbg_last = 0;
        uint32_t now = millis();
        if (now - dbg_last >= 500) {
          ESP_LOGD(TAG, "drained %u bytes (%u still queued)", (unsigned) accepted,
                    (unsigned) remaining);
          dbg_last = now;
        }
      }
    }
  }
  // If a follow-up window was deferred while audio was draining, wait for
  // the downstream chain (resampler + mixer + i2s + DAC tail) to actually
  // finish playing before firing the deferred LED-idle / chime trigger.
  // Just because our PSRAM queue is empty doesn't mean the user has heard
  // the audio yet — and an "сейчас посмотрю" preamble before a tool call
  // would drain the ring mid-turn, so we can't act on audio_fill_==0
  // alone.
  //
  // Primary signal: speaker_->is_stopped(). The resampling speaker
  // transitions to STOPPED only after every byte we wrote has actually
  // gone out through the i2s pipeline.
  //
  // Fallback: kSpeakerStopTimeoutMs (3 s). If something wedges and the
  // speaker never reports STOPPED, we still progress so the LED doesn't
  // lock in `replying`.
  if (this->followup_pending_ && this->audio_fill_snapshot_() == 0 &&
      !this->waiting_for_speaker_stop_) {
    this->waiting_for_speaker_stop_ = true;
    this->speaker_stop_wait_started_ms_ = millis();
  }
  if (this->waiting_for_speaker_stop_) {
    // Use has_buffered_data() instead of is_stopped(): the resampler only
    // transitions to STATE_STOPPED once its downstream (mixer source)
    // reports stopped, but our mixer sources are configured `timeout:
    // never` and stay RUNNING forever, so is_stopped() would never fire
    // and we'd always hit the fallback. has_buffered_data() walks the
    // chain (resampler ring + mixer source ring) and returns false as
    // soon as both have drained — exactly what we want.
    //
    // Note: this does *not* cover the i2s 500ms ring + ~100ms DAC tail
    // downstream of the mixer. We fire ~500ms before true silence. For
    // the LED that's imperceptible; for the request_follow_up chime,
    // yaml's wait_until !is_announcing + i2s tail delay already absorbs
    // any small overlap with the fading TTS tail.
    const bool speaker_drained =
        (this->speaker_ != nullptr) && !this->speaker_->has_buffered_data();
    const bool timed_out =
        (millis() - this->speaker_stop_wait_started_ms_) >= kSpeakerStopTimeoutMs;
    if (speaker_drained || timed_out) {
      const uint32_t graceful_close_token = this->graceful_close_token_.load();
      const uint32_t request_follow_up_token = this->request_follow_up_token_.load();
      const bool was_request =
          this->request_follow_up_pending_ && request_follow_up_token != 0 &&
          graceful_close_token == 0;
      if (timed_out && !speaker_drained && was_request) {
        ESP_LOGW(TAG,
                 "speaker still had buffered data after %u ms — explicit "
                 "follow-up revoked (fail closed)",
                 (unsigned) kSpeakerStopTimeoutMs);
        this->waiting_for_speaker_stop_ = false;
        this->revoke_followup_("speaker_drain_timeout", true);
        return;
      }
      if (timed_out && !speaker_drained) {
        ESP_LOGW(TAG,
                 "speaker still had buffered data after %u ms — "
                 "proceeding anyway (fallback)",
                 (unsigned) kSpeakerStopTimeoutMs);
      }
      this->waiting_for_speaker_stop_ = false;
      this->followup_pending_ = false;
      this->request_follow_up_pending_ = false;
      if (graceful_close_token != 0) {
        // has_buffered_data() excludes the final i2s/DAC tail. Keep the device
        // in replying through the same tail delay used before a normal follow-up.
        this->followup_armed_ = false;
        const uint32_t tail_delay = this->followup_open_delay_ms_;
        const ControlContext graceful_context = this->control_context_();
        ESP_LOGI(TAG, "graceful close — final buffers drained; idle in %u ms",
                 (unsigned) tail_delay);
        this->set_timeout("va_graceful_close", tail_delay,
                          [this, graceful_close_token, graceful_context]() {
          std::lock_guard<GenerationEffectGate> effect_guard(
              this->generation_effect_gate_);
          const ControlContext current_context = this->control_context_();
          if (current_context.session_nonce != graceful_context.session_nonce ||
              current_context.wake_generation != graceful_context.wake_generation ||
              current_context.effect_epoch != graceful_context.effect_epoch)
            return;
          uint32_t expected = graceful_close_token;
          if (!this->graceful_close_token_.compare_exchange_strong(expected, 0))
            return;
          ESP_LOGI(TAG, "graceful close complete — follow-up suppressed");
          this->open_followup_window_(0);
        });
      } else if (was_request) {
        // Request-driven path: speaker has drained, now hand off to yaml
        // for the chime → wait_until !is_announcing → READY sequence
        // (announcement lane is separate from the TTS lane we
        // just waited on, so the chime won't collide with our tail).
        this->open_followup_window_(0);  // emit deferred LED idle + latency log; no mic
        bool fire_callback = false;
        bool callback_conflict = false;
        uint32_t request_session_nonce = 0;
        portENTER_CRITICAL(&this->followup_mux_);
        const FollowUpCredentials credentials = this->lifecycle_.credentials();
        request_session_nonce = credentials.session_nonce;
        if (this->lifecycle_.follow_up_stage() == FollowUpStage::PREPARED &&
            credentials.token == request_follow_up_token &&
            request_session_nonce != 0 && this->lifecycle_.active_wake()) {
          if (this->request_follow_up_callback_in_flight_.load()) {
            callback_conflict = true;
          } else {
            this->request_follow_up_callback_in_flight_ = true;
            this->request_follow_up_callback_token_ = request_follow_up_token;
            this->request_follow_up_callback_session_nonce_ = request_session_nonce;
            this->followup_armed_ = true;
            fire_callback = true;
          }
        }
        portEXIT_CRITICAL(&this->followup_mux_);
        if (!fire_callback && !callback_conflict) {
          ESP_LOGW(TAG, "follow-up credentials changed before callback — revoked");
          this->revoke_followup_("callback_credentials_changed", true);
          return;
        }
        if (callback_conflict) {
          ESP_LOGW(TAG, "follow-up YAML callback already in flight; rejecting request");
          this->revoke_followup_("callback_conflict", true);
          return;
        }
        this->set_timeout(
            "va_followup_commit", kRequestFollowUpReadyTimeoutMs,
            [this, request_follow_up_token, request_session_nonce]() {
              std::lock_guard<GenerationEffectGate> effect_guard(
                  this->generation_effect_gate_);
              ESP_LOGW(TAG, "follow-up chime callback timed out — request revoked");
              this->abort_followup_mic(request_follow_up_token, request_session_nonce);
            });
        for (auto *t : this->followup_opened_triggers_) {
          t->trigger(request_follow_up_token, request_session_nonce);
        }
      } else {
        // Natural-idle path: emit the deferred LED idle, and — if the backend
        // configured a follow-up window (followup_ms_ > 0) — open the mic for
        // that long so the user can answer back without a wake word.
        this->open_followup_window_(this->followup_ms_);
      }
    }
  }
}

void VaClient::connect_() {
  if (this->ws_handle_ != nullptr) {
    // Already initialised; just (re)start. A synchronous start failure must
    // reschedule — otherwise the reconnect chain stalls silently and the
    // device stays offline until a reboot.
    esp_err_t err = esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(this->ws_handle_));
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_websocket_client_start (restart) failed: %d — rescheduling", (int) err);
      this->schedule_reconnect_();
    }
    return;
  }

  esp_websocket_client_config_t cfg = {};
  cfg.uri = this->url_.c_str();
  cfg.disable_auto_reconnect = true;  // we drive reconnects ourselves with exponential backoff
  cfg.reconnect_timeout_ms = 5000;    // ignored because disable_auto_reconnect=true

  esp_websocket_client_handle_t handle = esp_websocket_client_init(&cfg);
  if (handle == nullptr) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed");
    this->schedule_reconnect_();
    return;
  }
  this->ws_handle_ = handle;

  esp_err_t err = esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY, va_ws_event_handler, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_register_events failed: %d", (int) err);
  }

  ESP_LOGI(TAG, "Connecting to %s", this->url_.c_str());
  err = esp_websocket_client_start(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_client_start failed: %d", (int) err);
    this->schedule_reconnect_();
  }
}

void VaClient::schedule_reconnect_() {
  // esp_websocket_client emits multiple events per failure (DISCONNECTED,
  // CLOSED, sometimes ERROR). Coalesce them into a single reconnect.
  if (this->reconnect_pending_) {
    return;
  }
  this->reconnect_pending_ = true;

  // One per *failure* (coalesced), not per individual WS event. Once we
  // cross the threshold fire the audible-error trigger exactly once until
  // a successful connect resets the counter.
  this->consecutive_failures_++;
  if (this->consecutive_failures_ >= kRepeatedFailureThreshold &&
      !this->repeated_failure_fired_) {
    this->repeated_failure_fired_ = true;
    ESP_LOGW(TAG, "%u consecutive reconnect failures — firing on_repeated_failure",
             (unsigned) this->consecutive_failures_);
    this->defer([this]() {
      for (auto *t : this->repeated_failure_triggers_) {
        t->trigger();
      }
    });
  }

  uint32_t delay = this->reconnect_delay_ms_;
  ESP_LOGI(TAG, "Scheduling reconnect in %u ms", (unsigned) delay);
  // Backoff schedule: 1s -> 2s -> 5s -> 10s (capped).
  if (this->reconnect_delay_ms_ < 2000) {
    this->reconnect_delay_ms_ = 2000;
  } else if (this->reconnect_delay_ms_ < 5000) {
    this->reconnect_delay_ms_ = 5000;
  } else {
    this->reconnect_delay_ms_ = 10000;
  }
  this->set_timeout("va_reconnect", delay, [this]() {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    this->reconnect_pending_ = false;
    this->connect_();
  });
}

void VaClient::on_ws_event(int32_t event_id, void *event_data) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
      ESP_LOGI(TAG, "WS connected");
      uint32_t connection_generation = this->ws_connection_generation_.load() + 1;
      if (connection_generation == 0)
        connection_generation = 1;
      this->ws_connection_generation_ = connection_generation;
      this->ws_connected_ = true;
      this->transport_admission_.on_transport_connected();
      this->ws_reassembler_.reset();
      this->session_nonce_ = 0;
      portENTER_CRITICAL(&this->followup_mux_);
      this->lifecycle_.on_connected();
      portEXIT_CRITICAL(&this->followup_mux_);
      this->revoke_followup_("connect", false);
      this->reconnect_delay_ms_ = 1000;  // reset backoff on a clean open
      // Don't reset the failure counter / fired flag yet — a flap-and-die
      // link would spam chimes. Only re-arm after the connection has held
      // for kStableConnectionMs without a disconnect.
      this->set_timeout("va_stable_connection", kStableConnectionMs, [this]() {
        std::lock_guard<GenerationEffectGate> effect_guard(
            this->generation_effect_gate_);
        if (this->ws_connected_) {
          this->consecutive_failures_ = 0;
          this->repeated_failure_fired_ = false;
          ESP_LOGD(TAG, "WS stable for %u ms — error chime re-armed",
                   (unsigned) kStableConnectionMs);
        }
      });

      if (!this->send_text_bounded_("{\"type\":\"start\"}", "start")) {
        this->revoke_followup_("start_send_failed", false);
      }
      this->set_phase_("idle");
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      if (data == nullptr || data->data_len < 0 || data->payload_len < 0 ||
          data->payload_offset < 0 ||
          (data->data_ptr == nullptr && data->data_len != 0)) {
        ESP_LOGW(TAG, "invalid WebSocket event metadata rejected");
        this->ws_reassembler_.reset();
        this->revoke_followup_("malformed_ws_event", true);
        break;
      }
      const auto result = this->ws_reassembler_.push(
          data->op_code, data->fin, static_cast<size_t>(data->payload_len),
          static_cast<size_t>(data->payload_offset),
          reinterpret_cast<const uint8_t *>(data->data_ptr),
          static_cast<size_t>(data->data_len));
      if (result.status == WsReassemblyStatus::REJECTED) {
        ESP_LOGW(TAG, "malformed or oversized WebSocket message rejected");
        this->revoke_followup_("malformed_ws_message", true);
      } else if (result.status == WsReassemblyStatus::COMPLETE) {
        const auto &message = this->ws_reassembler_.message();
        if (result.type == WsMessageType::TEXT) {
          this->handle_text_(reinterpret_cast<const char *>(message.data()),
                             message.size());
        } else if (result.type == WsMessageType::BINARY) {
          this->handle_binary_(message.data(), message.size());
        }
      }
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR: {
      if (this->ws_connected_) {
        ESP_LOGW(TAG, "WS disconnected (event %d)", (int) event_id);
      }
      this->ws_connected_ = false;
      this->transport_admission_.on_transport_disconnected();
      this->ws_reassembler_.reset();
      this->session_nonce_ = 0;
      portENTER_CRITICAL(&this->followup_mux_);
      this->lifecycle_.on_disconnected();
      portEXIT_CRITICAL(&this->followup_mux_);
      this->revoke_followup_("disconnect", false);
      // A dropped WS mid-enrollment must not leave the mic pinned open with
      // nobody recording — exit enrollment locally (no notify; link is gone).
      if (this->enroll_mode_)
        this->defer([this]() { this->enroll_stop(false); });
      // Connection broke before the stability window elapsed — keep the
      // failure counter and the fired flag. A flapping link won't earn
      // a fresh chime.
      this->cancel_timeout("va_stable_connection");
      this->set_phase_("idle");
      this->schedule_reconnect_();
      break;
    }
    default:
      break;
  }
}

void VaClient::handle_text_(const char *data, size_t len) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  std::string msg(data, len);
  FlatJsonObject message;
  std::string type;
  if (!message.parse(msg) || !message.get_string("type", type)) {
    ESP_LOGW(TAG, "malformed control message — follow-up privilege revoked");
    this->revoke_followup_("malformed_control", true);
    return;
  }
  // Tokens and nonces are bearer credentials during a wake. Never emit the raw
  // control frame into logs where another local reader could replay them.
  ESP_LOGD(TAG, "WS text type: %s", type.c_str());

  if (type == "hello" && !this->transport_admission_.can_admit_session()) {
    ESP_LOGW(TAG, "hello rejected until the faulted transport reconnects");
    return;
  }

  if (type != "hello") {
    bool admitted = false;
    portENTER_CRITICAL(&this->followup_mux_);
    admitted = this->lifecycle_.connected() && this->lifecycle_.authorized();
    portEXIT_CRITICAL(&this->followup_mux_);
    if (!admitted) {
      ESP_LOGW(TAG, "control before admitted hello rejected");
      this->revoke_followup_("control_before_hello", false);
      return;
    }
  }

  if (type == "error") {
    ESP_LOGW(TAG, "server reported an error");
    // Without an audible cue the user just sees the LED go idle and
    // assumes the assistant ignored them. Reuse the on_repeated_failure
    // trigger — it already plays error_cloud_expired and the failure
    // mode is identical from the user's perspective ("something went
    // wrong, try again"). We deliberately don't bump consecutive_failures_
    // here; that counter is for WS reachability, not server-side errors.
    // Marshalled via defer(): handle_text_ runs on the WS task and ESPHome
    // triggers (→ the yaml play_sound script) are not thread-safe.
    this->defer([this]() {
      for (auto *t : this->repeated_failure_triggers_) {
        t->trigger();
      }
    });
    this->revoke_followup_("backend_error", false);
    this->set_phase_("idle");
    return;
  }

  if (type == "hello") {
    const bool nonce_field_present = message.has("nonce");
    std::string audio_out;
    uint32_t nonce = 0;
    uint32_t follow_up_ms = 0;
    uint32_t follow_up_open_delay_ms = 0;
    uint32_t wake_open_delay_ms = 0;
    uint32_t playback_prebuffer_ms = 0;
    const bool exact_shape = nonce_field_present
                                 ? message.has_exact(
                                       {"type", "nonce", "audio_out", "follow_up_ms",
                                        "follow_up_open_delay_ms", "wake_open_delay_ms",
                                        "playback_prebuffer_ms"})
                                 : message.has_exact(
                                       {"type", "audio_out", "follow_up_ms",
                                        "follow_up_open_delay_ms", "wake_open_delay_ms",
                                        "playback_prebuffer_ms"});
    const bool common_fields_valid =
        exact_shape && message.get_string("audio_out", audio_out) && audio_out == "pcm" &&
        message.get_uint("follow_up_ms", follow_up_ms) &&
        follow_up_ms == 0 &&
        message.get_uint("follow_up_open_delay_ms", follow_up_open_delay_ms) &&
        follow_up_open_delay_ms <= kFollowupOpenDelayMaxMs &&
        message.get_uint("wake_open_delay_ms", wake_open_delay_ms) &&
        wake_open_delay_ms <= kFollowupOpenDelayMaxMs &&
        message.get_uint("playback_prebuffer_ms", playback_prebuffer_ms) &&
        playback_prebuffer_ms <= kPlaybackPrebufferMaxMs;

    if (!nonce_field_present) {
      bool accepted = false;
      portENTER_CRITICAL(&this->followup_mux_);
      if (common_fields_valid)
        accepted = this->lifecycle_.admit_legacy_zero_hello();
      else
        this->lifecycle_.reject_hello();
      portEXIT_CRITICAL(&this->followup_mux_);
      this->session_nonce_ = 0;
      this->revoke_followup_(accepted ? "legacy_hello" : "legacy_hello_rejected", false);
      if (!accepted) {
        ESP_LOGW(TAG, "legacy hello rejected: exact backend 0.20.6 zero-mode shape required");
        return;
      }
      this->followup_ms_ = 0;
      this->followup_open_delay_ms_ = follow_up_open_delay_ms;
      this->wake_open_delay_ms_ = wake_open_delay_ms;
      portENTER_CRITICAL(&this->ring_mux_);
      this->playback_prebuffer_ms_ = playback_prebuffer_ms;
      this->playback_prebuffer_pending_ = false;
      this->playback_priming_ = false;
      this->prime_started_ms_ = 0;
      portEXIT_CRITICAL(&this->ring_mux_);
      if (!this->transport_admission_.on_session_admitted()) {
        portENTER_CRITICAL(&this->followup_mux_);
        this->lifecycle_.reject_hello();
        portEXIT_CRITICAL(&this->followup_mux_);
        ESP_LOGW(TAG, "legacy hello rejected on a quarantined transport");
        return;
      }
      ESP_LOGI(TAG, "exact legacy zero-mode hello applied; explicit follow-up unavailable");
      return;
    }

    const bool trusted_fields_valid =
        common_fields_valid && message.get_uint("nonce", nonce) && nonce != 0 &&
        nonce <= kProtocolTokenMax;
    portENTER_CRITICAL(&this->ring_mux_);
    const uint32_t current_playback_prebuffer_ms = this->playback_prebuffer_ms_;
    portEXIT_CRITICAL(&this->ring_mux_);
    HelloAdmission admission = HelloAdmission::REJECTED;
    portENTER_CRITICAL(&this->followup_mux_);
    if (trusted_fields_valid)
      admission = this->lifecycle_.admit_trusted_hello(nonce);
    else
      this->lifecycle_.reject_hello();
    portEXIT_CRITICAL(&this->followup_mux_);
    const bool recovery_values_match =
        admission != HelloAdmission::RECOVERY ||
        (this->followup_ms_.load() == 0 &&
         this->followup_open_delay_ms_.load() == follow_up_open_delay_ms &&
         this->wake_open_delay_ms_.load() == wake_open_delay_ms &&
         current_playback_prebuffer_ms == playback_prebuffer_ms);
    const bool accepted = admission != HelloAdmission::REJECTED && recovery_values_match;
    if (accepted) {
      this->revoke_followup_(admission == HelloAdmission::RECOVERY ? "hello_recovery" : "hello", false);
      this->followup_ms_ = follow_up_ms;
      this->followup_open_delay_ms_ = follow_up_open_delay_ms;
      this->wake_open_delay_ms_ = wake_open_delay_ms;
      portENTER_CRITICAL(&this->ring_mux_);
      this->playback_prebuffer_ms_ = playback_prebuffer_ms;
      if (playback_prebuffer_ms == 0) {
        this->playback_prebuffer_pending_ = false;
        this->playback_priming_ = false;
        this->prime_started_ms_ = 0;
      }
      portEXIT_CRITICAL(&this->ring_mux_);
      this->session_nonce_ = nonce;
      ESP_LOGI(TAG,
                "strict hello applied: follow-up=%u ms open-delay=%u ms wake-delay=%u ms "
                "prebuffer=%u ms",
               (unsigned) this->followup_ms_,
               (unsigned) this->followup_open_delay_ms_,
               (unsigned) this->wake_open_delay_ms_,
               (unsigned) this->playback_prebuffer_ms_);
    } else {
      this->session_nonce_ = 0;
      portENTER_CRITICAL(&this->followup_mux_);
      this->lifecycle_.reject_hello();
      portEXIT_CRITICAL(&this->followup_mux_);
      this->revoke_followup_("hello_rejected", false);
      ESP_LOGW(TAG, "hello rejected: exact fields, pcm audio, fresh nonce, and zero mode required");
    }
    const bool ack_sent = this->send_hello_ack_(nonce, accepted);
    if (accepted && ack_sent && this->session_nonce_.load() == nonce) {
      if (!this->transport_admission_.on_session_admitted()) {
        this->session_nonce_ = 0;
        portENTER_CRITICAL(&this->followup_mux_);
        this->lifecycle_.reject_hello();
        portEXIT_CRITICAL(&this->followup_mux_);
        ESP_LOGW(TAG, "trusted hello rejected on a quarantined transport");
      }
    }
    if (accepted && this->session_nonce_.load() != nonce) {
      ESP_LOGW(TAG, "hello ACK failed; trusted session revoked");
    }
    return;
  }

  if (type == "request_follow_up") {
    uint32_t token = 0;
    uint32_t session_nonce = 0;
    const bool message_shape_valid =
        message.has_exact({"type", "token", "session_nonce"}) &&
        message.get_uint("token", token) &&
        message.get_uint("session_nonce", session_nonce);
    const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
    const bool microphone_ready = !this->microphone_is_muted_();
    bool accepted = false;
    portENTER_CRITICAL(&this->followup_mux_);
    accepted = message_shape_valid && phase_now == Phase::REPLYING &&
               this->followup_ms_.load() == 0 && !this->barge_in_ &&
               microphone_ready &&
               this->graceful_close_prepared_token_.load() == 0 &&
               this->graceful_close_token_.load() == 0 &&
               this->lifecycle_.prepare_follow_up(token, session_nonce);
    if (accepted) {
      this->request_follow_up_token_ = token;
      this->followup_pending_ = false;
      this->waiting_for_speaker_stop_ = false;
      this->request_follow_up_pending_ = true;
      this->followup_armed_ = false;
      this->streaming_ = false;
    }
    portEXIT_CRITICAL(&this->followup_mux_);
    if (accepted) {
      ESP_LOGI(TAG, "request_follow_up accepted; waiting for idle + drain");
    } else {
      ESP_LOGW(TAG, "request_follow_up rejected (phase=%s mode=%u)",
               phase_name_(phase_now), (unsigned) this->followup_ms_);
      // A malformed, stale, replayed, or competing request makes ownership
      // ambiguous. Burn nothing new, but revoke this wake until a local wake.
      this->revoke_followup_("prepare_rejected", false);
    }
    const bool ack_sent = this->send_request_follow_up_ack_(token, session_nonce, accepted);
    if (accepted && !ack_sent)
      this->revoke_followup_("prepare_ack_failed", false);
    return;
  }

  if (type == "cancel_request_follow_up") {
    uint32_t token = 0;
    uint32_t session_nonce = 0;
    const bool shape_valid =
        message.has_exact({"type", "token", "session_nonce"}) &&
        message.get_uint("token", token) && token != 0 &&
        token <= kProtocolTokenMax &&
        message.get_uint("session_nonce", session_nonce) &&
        session_nonce != 0 && session_nonce <= kProtocolTokenMax;
    bool accepted = false;
    portENTER_CRITICAL(&this->followup_mux_);
    const FollowUpCredentials credentials = this->lifecycle_.credentials();
    accepted = shape_valid && this->lifecycle_.follow_up_stage() != FollowUpStage::NONE &&
               credentials.token == token && credentials.session_nonce == session_nonce;
    this->lifecycle_.revoke();
    portEXIT_CRITICAL(&this->followup_mux_);
    if (accepted) {
      ESP_LOGI(TAG, "request_follow_up cancelled");
    } else {
      ESP_LOGW(TAG, "stale or malformed request_follow_up cancellation rejected");
    }
    this->revoke_followup_(accepted ? "cancelled" : "cancel_rejected", false);
    this->send_cancel_request_follow_up_ack_(
        token, session_nonce, accepted, accepted);
    return;
  }

  if (type == "commit_follow_up") {
    uint32_t token = 0;
    uint32_t session_nonce = 0;
    uint32_t ready_nonce = 0;
    const bool shape_valid =
        message.has_exact({"type", "token", "session_nonce", "ready_nonce"}) &&
        message.get_uint("token", token) && message.get_uint("session_nonce", session_nonce) &&
        message.get_uint("ready_nonce", ready_nonce);
    uint32_t audio_generation = 0;
    uint32_t wake_generation = 0;
    bool commit_safe = false;
    const bool microphone_muted = this->microphone_is_muted_();
    const bool initial_announcement_clear = this->announcement_path_clear_();
    portENTER_CRITICAL(&this->followup_mux_);
    this->lifecycle_.set_muted(microphone_muted);
    audio_generation = this->lifecycle_.audio_generation();
    wake_generation = this->lifecycle_.credentials().wake_generation;
    commit_safe = shape_valid && initial_announcement_clear &&
                  this->mic_send_fence_.in_flight() == 0 &&
                  this->lifecycle_.commit_is_safe(
                      token, session_nonce, ready_nonce, audio_generation,
                      initial_announcement_clear);
    if (!commit_safe)
      this->lifecycle_.revoke();
    portEXIT_CRITICAL(&this->followup_mux_);

    if (!commit_safe) {
      this->revoke_followup_("commit_rejected", false);
      this->send_follow_up_commit_ack_(token, session_nonce, ready_nonce, false);
      return;
    }

    // The accepted ACK is sent while the mic is still closed. Only an exact,
    // bounded send followed by a second local safety check may open it.
    if (!this->send_follow_up_commit_ack_(token, session_nonce, ready_nonce, true)) {
      this->revoke_followup_("commit_ack_failed", false);
      return;
    }

    bool ring_empty = false;
    portENTER_CRITICAL(&this->ring_mux_);
    ring_empty = this->audio_fill_ == 0;
    portEXIT_CRITICAL(&this->ring_mux_);
    const bool speaker_drained =
        this->speaker_ != nullptr && !this->speaker_->has_buffered_data();
    const bool announcement_clear = this->announcement_path_clear_();
    const bool microphone_ready = !this->microphone_is_muted_();
    bool opened = false;
    portENTER_CRITICAL(&this->followup_mux_);
    const uint32_t final_audio_generation = this->lifecycle_.audio_generation();
    if (ring_empty && speaker_drained && announcement_clear && microphone_ready &&
        this->mic_send_fence_.in_flight() == 0 &&
        final_audio_generation == audio_generation) {
      opened = this->lifecycle_.open_follow_up_after_commit(
          token, session_nonce, ready_nonce, final_audio_generation,
          announcement_clear);
      if (opened) {
        this->streaming_ = true;
      }
    } else {
      this->lifecycle_.revoke();
    }
    portEXIT_CRITICAL(&this->followup_mux_);
    if (!opened) {
      this->revoke_followup_("commit_race", true);
      return;
    }

    this->preroll_discard_pending_ = true;
    this->cancel_timeout("va_followup_commit");
    ESP_LOGI(TAG, "two-phase follow-up mic open; hard timeout=%u ms",
             (unsigned) kRequestFollowUpMs);
    this->set_timeout(
        "va_request_follow_up_hard", kRequestFollowUpMs,
        [this, wake_generation, ready_nonce]() {
          std::lock_guard<GenerationEffectGate> effect_guard(
              this->generation_effect_gate_);
          bool matches = false;
          uint32_t session_nonce = 0;
          portENTER_CRITICAL(&this->followup_mux_);
          matches = this->lifecycle_.hard_timeout_matches(wake_generation, ready_nonce);
          if (matches) {
            session_nonce = this->lifecycle_.session_nonce();
            this->lifecycle_.revoke();
            this->streaming_ = false;
          }
          portEXIT_CRITICAL(&this->followup_mux_);
          if (!matches)
            return;
          if (!this->wait_for_mic_send_barrier_(
                  "follow_up_timeout_mic_send_barrier_failed"))
            return;
          this->send_mic_flush_(session_nonce, wake_generation);
          this->send_interrupt_control_("follow_up_timeout", session_nonce,
                                        wake_generation);
          this->fire_phase_led_("idle");
        });
    this->fire_phase_led_("listening");
    return;
  }

  if (type == "prepare_suppress_followup") {
    uint32_t token = 0;
    const bool has_token = message.has_exact({"type", "token"}) &&
                           message.get_uint("token", token) && token != 0;
    const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
    const bool accepted = has_token &&
                           this->request_follow_up_token_.load() == 0 &&
                           (phase_now == Phase::THINKING || phase_now == Phase::REPLYING);
    if (accepted) {
      this->revoke_followup_("graceful_prepare", false);
      this->graceful_close_prepared_token_ = token;
      ESP_LOGI(TAG, "graceful close prepared");
    } else {
      ESP_LOGW(TAG, "graceful close prepare rejected in phase=%s", phase_name_(phase_now));
      this->revoke_followup_("graceful_prepare_rejected", true);
    }
    this->send_graceful_close_ack_("prepared", token, accepted);
    return;
  }

  if (type == "commit_suppress_followup") {
    uint32_t token = 0;
    const bool has_token = message.has_exact({"type", "token"}) &&
                           message.get_uint("token", token) && token != 0;
    const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
    const bool accepted = has_token &&
                          this->graceful_close_prepared_token_.load() == token &&
                          (phase_now == Phase::THINKING || phase_now == Phase::REPLYING);
    if (accepted) {
      this->graceful_close_prepared_token_ = 0;
      this->graceful_close_token_ = token;
      ESP_LOGI(TAG, "graceful close committed");
    } else {
      ESP_LOGW(TAG, "graceful close commit rejected in phase=%s", phase_name_(phase_now));
      this->revoke_followup_("graceful_commit_rejected", true);
    }
    this->send_graceful_close_ack_("committed", token, accepted);
    return;
  }

  if (type == "cancel_suppress_followup") {
    uint32_t token = 0;
    this->revoke_followup_("graceful_cancel", false);
    if (message.has_exact({"type", "token"}) &&
        message.get_uint("token", token) && token != 0) {
      if (this->graceful_close_prepared_token_.load() == token)
        this->graceful_close_prepared_token_ = 0;
      if (this->graceful_close_token_.load() == token) {
        this->graceful_close_token_ = 0;
        this->cancel_timeout("va_graceful_close");
      }
      ESP_LOGI(TAG, "graceful close cancelled");
    }
    return;
  }

  if (type == "ack") {
    // Backend confirms mic audio is flowing for this turn. Cancel the
    // no-speech abort: with semantic VAD the "speech detected" phase can
    // arrive only at utterance COMMIT, which for longer commands lands past
    // any reasonable fixed timeout (community report: aborts mid-sentence).
    // Silent misfires still close via the follow-up window flush timers.
    uint32_t session_nonce = 0;
    uint32_t wake_generation = 0;
    bool valid = false;
    portENTER_CRITICAL(&this->followup_mux_);
    const PilotAuthMode auth_mode = this->lifecycle_.auth_mode();
    if (auth_mode == PilotAuthMode::LEGACY_ZERO) {
      valid = message.has_exact({"type"}) && this->lifecycle_.active_wake();
    } else if (auth_mode == PilotAuthMode::NONCE) {
      valid = message.has_exact({"type", "session_nonce", "wake_generation"}) &&
              message.get_uint("session_nonce", session_nonce) &&
              message.get_uint("wake_generation", wake_generation) &&
              session_nonce == this->lifecycle_.session_nonce() &&
              wake_generation == this->lifecycle_.wake_generation() &&
              this->lifecycle_.active_wake();
    }
    portEXIT_CRITICAL(&this->followup_mux_);
    if (valid) {
      this->cancel_timeout("va_no_speech");
      ESP_LOGD(TAG, "current backend audio ACK cancelled silent-wake timeout");
    } else {
      ESP_LOGW(TAG, "stale or malformed backend audio ACK rejected");
      this->revoke_followup_("stale_audio_ack", true);
    }
    return;
  }

  if (type == "enroll") {
    std::string mode;
    if (!message.has_exact({"type", "mode"}) ||
        !message.get_string("mode", mode) ||
        (mode != "start" && mode != "stop")) {
      ESP_LOGW(TAG, "malformed enrollment control rejected");
      this->revoke_followup_("malformed_enroll", true);
      return;
    }
    const bool start = mode == "start";
    this->revoke_followup_("enroll_control", false);
    ESP_LOGI(TAG, "enroll control from backend: %s", start ? "start" : "stop");
    this->defer([this, start]() {
      if (start)
        this->enroll_start();
      else
        this->enroll_stop(false);
    });
    return;
  }

  if (type == "phase") {
    if (this->enroll_mode_) {
      ESP_LOGD(TAG, "ignoring backend phase during enrollment");
      return;
    }
    std::string value;
    uint32_t session_nonce = 0;
    uint32_t wake_generation = 0;
    const bool value_valid = message.get_string("value", value);
    const bool legacy_shape =
        value_valid && message.has_exact({"type", "value"});
    const bool trusted_shape =
        value_valid &&
        message.has_exact(
            {"type", "value", "session_nonce", "wake_generation"}) &&
        message.get_uint("session_nonce", session_nonce) &&
        message.get_uint("wake_generation", wake_generation);

    PilotPhase lifecycle_phase = PilotPhase::IDLE;
    Phase runtime_phase = Phase::IDLE;
    bool known_phase = true;
    if (value == "listening") {
      lifecycle_phase = PilotPhase::LISTENING;
      runtime_phase = Phase::LISTENING;
    } else if (value == "thinking") {
      lifecycle_phase = PilotPhase::THINKING;
      runtime_phase = Phase::THINKING;
    } else if (value == "replying") {
      lifecycle_phase = PilotPhase::REPLYING;
      runtime_phase = Phase::REPLYING;
    } else if (value != "idle") {
      known_phase = false;
    }
    if (!known_phase) {
      this->revoke_followup_("unknown_phase", true);
      return;
    }

    const bool microphone_muted = this->microphone_is_muted_();
    const Phase runtime_previous =
        static_cast<Phase>(this->current_phase_.load());
    PhaseApplyResult transition;
    portENTER_CRITICAL(&this->followup_mux_);
    if (this->lifecycle_.legacy_zero()) {
      if (legacy_shape) {
        transition = this->lifecycle_.apply_legacy_phase(
            lifecycle_phase, microphone_muted, this->barge_in_);
      }
    } else if (this->lifecycle_.trusted()) {
      if (trusted_shape) {
        transition = this->lifecycle_.apply_trusted_phase(
            lifecycle_phase, session_nonce, wake_generation,
            microphone_muted);
      }
    }
    portEXIT_CRITICAL(&this->followup_mux_);

    if (transition.status == PhaseApplyStatus::STALE) {
      ESP_LOGD(TAG, "stale phase ignored without touching current wake");
      return;
    }
    if (transition.status != PhaseApplyStatus::APPLIED) {
      this->revoke_followup_("malformed_phase", true);
      return;
    }
    this->apply_phase_side_effects_(value, runtime_phase, runtime_previous,
                                    transition);
    return;
  }

  ESP_LOGW(TAG, "unknown control type rejected");
  this->revoke_followup_("unknown_control", true);
}

void VaClient::handle_binary_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || len < 2 || this->audio_buf_ == nullptr)
    return;
  // After a "stop"/barge-in (send_interrupt) the backend is still streaming the
  // rest of the already-generated reply. Drop it so the cancelled reply goes
  // silent instead of refilling the PSRAM ring we just flushed. Cleared on the
  // next "idle"/"listening" phase (see set_phase_).
  if (this->suppress_incoming_audio_)
    return;
  uint32_t audio_generation = 0;
  bool admitted = false;
  bool invalidated_follow_up = false;
  portENTER_CRITICAL(&this->followup_mux_);
  admitted = this->lifecycle_.connected() && this->lifecycle_.authorized();
  if (admitted)
    invalidated_follow_up = this->lifecycle_.assistant_audio(audio_generation);
  if (invalidated_follow_up) {
    this->streaming_ = false;
  }
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!admitted) {
    ESP_LOGW(TAG, "audio before admitted hello rejected");
    return;
  }
  if (invalidated_follow_up) {
    ESP_LOGW(TAG, "assistant audio arrived outside the bound reply drain — follow-up revoked");
    this->revoke_followup_("late_assistant_audio", true);
  }
  const uint32_t now_ms = millis();
  if (this->turn_t_first_audio_out_ == 0 && this->turn_t_wake_ != 0) {
    this->turn_t_first_audio_out_ = now_ms;
  }
  // Detector 1: WS frame inter-arrival jitter. Normal cadence is ~20 ms
  // per frame (OpenAI streams realtime). A gap > kWsGapWarnMs means the
  // bridge stalled, network blip, or OpenAI burst late — anything that
  // could starve the downstream chain. Log immediately so the gap is
  // adjacent to whatever the user reports hearing.
  if (this->last_binary_ms_ != 0) {
    const uint32_t gap = now_ms - this->last_binary_ms_;
    if (gap > kWsGapWarnMs) {
      this->ws_gap_count_++;
      if (gap > this->ws_gap_max_ms_) this->ws_gap_max_ms_ = gap;
      ESP_LOGW(TAG, "ws audio gap: %u ms (ring fill %u bytes)",
               (unsigned) gap, (unsigned) this->audio_fill_snapshot_());
    }
  }
  this->last_binary_ms_ = now_ms;
  // PCM16 mono @ 24 kHz, append to ring buffer. loop() drains.
  // Snapshot audio_fill_ under the lock — it's modified by loop() on the
  // other core and we can't trust a torn read.
  size_t free_space;
  uint32_t write_generation;
  portENTER_CRITICAL(&this->ring_mux_);
  free_space = kAudioBufBytes - this->audio_fill_;
  write_generation = this->prebuffer_generation_;
  portEXIT_CRITICAL(&this->ring_mux_);
  if (this->suppress_incoming_audio_)
    return;
  if (len > free_space) {
    ESP_LOGW(TAG, "audio buffer overflow: dropping %u bytes (have %u free of %u total)",
             (unsigned) (len - free_space), (unsigned) free_space, (unsigned) kAudioBufBytes);
    len = free_space;
    if (len == 0)
      return;
  }
  // Apply user-controlled volume from external_media_player before writing to
  // the ring. volume_ is set from yaml on every media_player volume / mute
  // change. With vol ≤ 1 there is no mathematical way to overflow int16, but
  // we keep a defensive saturation + clipped_samples_ counter so any future
  // gain re-introduction shows up in the per-turn summary instead of silently
  // distorting.
  size_t pairs = len / 2;
  if (pairs > 0) {
    auto *in = reinterpret_cast<const int16_t *>(data);
    // Scale into tts_buf_ — NOT mono_buf_: that vector is owned by the mic
    // task (on_mic_data_ refills it on every mic frame, and the mic never
    // stops while mWW runs), whereas we're on the WS task. Sharing one vector
    // raced the two tasks (concurrent resize/realloc + interleaved writes),
    // putting mic samples / freed memory into the TTS ring — audible as hiss.
    this->tts_buf_.resize(pairs);
    float vol = this->volume_;
    if (vol < 0.0f) vol = 0.0f;
    else if (vol > 1.0f) vol = 1.0f;
    // The Voice PE's acoustic echo cancellation is imperfect at high speaker
    // volume. Keep enough headroom for reliable handsfree barge-in and avoid
    // feeding the assistant's own reply back into server VAD.
    if (this->barge_in_ && vol > 0.70f) vol = 0.70f;
    // Q15 fixed point so the inner loop stays integer-only.
    int32_t scale = static_cast<int32_t>(vol * 32768.0f);
    uint32_t clipped = 0;
    for (size_t i = 0; i < pairs; i++) {
      int32_t v = (static_cast<int32_t>(in[i]) * scale) >> 15;
      if (v > 32767) { v = 32767; clipped++; }
      else if (v < -32768) { v = -32768; clipped++; }
      this->tts_buf_[i] = static_cast<int16_t>(v);
    }
    this->clipped_samples_ += clipped;
    data = reinterpret_cast<const uint8_t *>(this->tts_buf_.data());
    // len is unchanged (pairs * 2 == len rounded down; trailing odd byte ignored).
    len = pairs * 2;
  }
  // Two-part write: from tail to end, then wrap to start.
  // We need a stable snapshot of audio_tail_ for the memcpy destination,
  // then commit tail + fill atomically with the writes so the reader on
  // the other core never sees a new tail before the memcpy completed.
  // Doing the memcpy *inside* the critical section is the simplest way
  // to guarantee that ordering — len is at most a few KB per WS frame
  // and PSRAM memcpy is ~10–20 µs, well under any audio deadline.
  portENTER_CRITICAL(&this->ring_mux_);
  if (this->prebuffer_generation_ != write_generation ||
      this->suppress_incoming_audio_) {
    portEXIT_CRITICAL(&this->ring_mux_);
    return;
  }
  const bool was_empty = (this->audio_fill_ == 0);
  size_t tail = this->audio_tail_;
  size_t first = std::min(len, kAudioBufBytes - tail);
  std::memcpy(this->audio_buf_ + tail, data, first);
  if (first < len) {
    std::memcpy(this->audio_buf_, data + first, len - first);
  }
  this->audio_tail_ = (tail + len) % kAudioBufBytes;
  this->audio_fill_ += len;
  // Publish the empty→nonempty transition and its pending jitter decision
  // atomically. The main loop resolves downstream state from its own task.
  if (was_empty && this->playback_prebuffer_ms_ > 0 &&
      !this->playback_prebuffer_pending_ && !this->playback_priming_) {
    this->prime_started_ms_ = now_ms;
    this->prebuffer_generation_++;
    this->playback_prebuffer_pending_ = true;
  }
  portEXIT_CRITICAL(&this->ring_mux_);
  // The main loop arms priming only if the downstream chain is also dry. This
  // avoids holding routine mid-reply clumps while keeping speaker access on its
  // owning task.
  // No per-chunk log — fires 50+ times per reply at DEBUG and drowns the
  // log. The throttled drain log in loop() gives enough visibility into
  // queue depth.
}

void VaClient::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr)
    return;
  // i2s_mics yields interleaved stereo int32 frames: [L0_low,L0_high, R0_low,R0_high, L1..].
  // Each frame = 8 bytes (2ch × 4 bytes). We want one channel converted to
  // int16 mono. Real audio sits in the high 16 bits (ADC pads up to int32).
  if (samples.size() < 8)
    return;

  const auto *in32 = reinterpret_cast<const int32_t *>(samples.data());
  size_t total_int32 = samples.size() / 4;
  size_t mono_samples = total_int32 / 2;  // half belong to this channel
  size_t offset = this->mic_channel_ & 0x1;

  this->mono_buf_.resize(mono_samples);
  for (size_t i = 0; i < mono_samples; i++) {
    int32_t s = in32[i * 2 + offset];
    this->mono_buf_[i] = static_cast<int16_t>(s >> 16);
  }

  // Streaming gate. When no session is active we don't forward frames to the
  // server (otherwise OpenAI's VAD would respond to any room speech — the wake
  // word would be decoration). We keep a rolling pre-roll ring while closed,
  // but it is DISCARDED on session open (see below), so this is just a cheap
  // rolling buffer kept around for a possible future capture-gating approach.
  // The session opens via start_session() (wake handler) and closes on
  // "phase":"idle" from the server (response.done).
  bool owned_open_mic = false;
  bool send_lease_acquired = false;
  uint32_t send_lease_epoch = 0;
  const bool microphone_muted = this->microphone_is_muted_();
  {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    portENTER_CRITICAL(&this->followup_mux_);
    this->lifecycle_.set_muted(microphone_muted);
    owned_open_mic = this->lifecycle_.mic_open();
    if (!owned_open_mic)
      this->streaming_ = false;
    if (owned_open_mic && this->streaming_) {
      send_lease_epoch = this->lifecycle_.mic_epoch();
      this->mic_send_fence_.acquire();
      send_lease_acquired = true;
    }
    portEXIT_CRITICAL(&this->followup_mux_);
  }
  if (!send_lease_acquired) {
    this->preroll_push_(this->mono_buf_.data(), this->mono_buf_.size());
    return;
  }

  bool send_lease_current = false;
  portENTER_CRITICAL(&this->followup_mux_);
  send_lease_current = this->streaming_ && MicSendFence::lease_is_current(
                                                    send_lease_epoch,
                                                    this->lifecycle_.mic_epoch(),
                                                    this->lifecycle_.mic_open());
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!send_lease_current) {
    this->mic_send_fence_.release();
    this->preroll_push_(this->mono_buf_.data(), this->mono_buf_.size());
    return;
  }

  // First frame of a fresh session: DISCARD the pre-roll instead of replaying
  // it. The ring caught the wake chime leaking through the mic (XMOS AEC leaves
  // ~10x) during the chime + tail-delay window; replaying it fed the chime back
  // to OpenAI as a phantom utterance ("Au!"). Resetting here (the ring's only
  // owner is this mic task) avoids any cross-task race — the main loop just sets
  // the flag. Matches marcinnowak79 gemini_proxy's ring_buffer_->reset() on
  // start. Trade-off: a word spoken *during* the chime is lost (the user speaks
  // after the listening ring lights up).
  if (this->preroll_discard_pending_) {
    this->preroll_discard_pending_ = false;
    this->preroll_count_ = 0;
    this->preroll_head_ = 0;
  }

  const size_t payload_size = this->mono_buf_.size() * sizeof(int16_t);
  const bool sent = this->send_binary_bounded_(
      reinterpret_cast<const uint8_t *>(this->mono_buf_.data()), payload_size);
  this->mic_send_fence_.release();
  if (!sent) {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    ControlContext context;
    portENTER_CRITICAL(&this->followup_mux_);
    context.session_nonce = this->lifecycle_.session_nonce();
    context.wake_generation = this->lifecycle_.wake_generation();
    this->lifecycle_.revoke();
    this->streaming_ = false;
    portEXIT_CRITICAL(&this->followup_mux_);
    ESP_LOGW(TAG, "mic audio send failed or partial; local mic gate closed");
    this->send_client_revoke_("audio_send_failed", context.session_nonce,
                              context.wake_generation);
  }
}

void VaClient::preroll_push_(const int16_t *data, size_t n) {
  if (this->preroll_buf_ == nullptr || this->preroll_capacity_samples_ == 0)
    return;
  const size_t cap = this->preroll_capacity_samples_;
  for (size_t i = 0; i < n; i++) {
    this->preroll_buf_[this->preroll_head_] = data[i];
    if (++this->preroll_head_ >= cap)
      this->preroll_head_ = 0;
    if (this->preroll_count_ < cap)
      this->preroll_count_++;
  }
}

VaClient::Phase VaClient::phase_from_string_(const std::string &phase) {
  if (phase == "enrolling")
    return Phase::ENROLLING;
  if (phase == "listening")
    return Phase::LISTENING;
  if (phase == "thinking")
    return Phase::THINKING;
  if (phase == "replying")
    return Phase::REPLYING;
  return Phase::IDLE;
}

PilotPhase VaClient::pilot_phase_from_runtime_(Phase phase) {
  switch (phase) {
    case Phase::LISTENING:
      return PilotPhase::LISTENING;
    case Phase::THINKING:
      return PilotPhase::THINKING;
    case Phase::REPLYING:
      return PilotPhase::REPLYING;
    case Phase::ENROLLING:
      return PilotPhase::ENROLLING;
    default:
      return PilotPhase::IDLE;
  }
}

const char *VaClient::phase_name_(Phase p) {
  switch (p) {
    case Phase::LISTENING:
      return "listening";
    case Phase::THINKING:
      return "thinking";
    case Phase::REPLYING:
      return "replying";
    default:
      return "idle";
  }
}

void VaClient::set_phase_(const std::string &phase) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (this->enroll_mode_ && phase != "enrolling") {
    ESP_LOGD(TAG, "ignoring phase '%s' during enrollment", phase.c_str());
    return;
  }
  const Phase prev = static_cast<Phase>(this->current_phase_.load());
  const Phase runtime_phase = phase_from_string_(phase);
  PhaseApplyResult transition;
  transition.status = PhaseApplyStatus::APPLIED;
  transition.previous = pilot_phase_from_runtime_(prev);
  transition.target = pilot_phase_from_runtime_(runtime_phase);
  portENTER_CRITICAL(&this->followup_mux_);
  transition.active_after = this->lifecycle_.active_wake();
  transition.connection_generation = this->lifecycle_.connection_generation();
  transition.session_nonce = this->lifecycle_.session_nonce();
  transition.wake_generation = this->lifecycle_.wake_generation();
  transition.effect_epoch = this->lifecycle_.effect_epoch();
  portEXIT_CRITICAL(&this->followup_mux_);
  this->apply_phase_side_effects_(phase, runtime_phase, prev, transition);
}

bool VaClient::phase_effect_plan_current_(
    const PhaseApplyResult &transition) {
  bool current = false;
  portENTER_CRITICAL(&this->followup_mux_);
  current = this->lifecycle_.phase_effect_plan_is_current(transition);
  portEXIT_CRITICAL(&this->followup_mux_);
  return current;
}

void VaClient::apply_phase_side_effects_(
    const std::string &phase, Phase runtime_phase, Phase runtime_previous,
    const PhaseApplyResult &transition) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (transition.target != pilot_phase_from_runtime_(runtime_phase) ||
      static_cast<Phase>(this->current_phase_.load()) != runtime_previous ||
      !this->phase_effect_plan_current_(transition)) {
    ESP_LOGD(TAG, "superseded phase effect plan ignored");
    return;
  }
  const Phase prev = runtime_previous;
  this->current_phase_.store(static_cast<uint8_t>(runtime_phase));
  portENTER_CRITICAL(&this->followup_mux_);
  this->streaming_ = this->lifecycle_.mic_open();
  portEXIT_CRITICAL(&this->followup_mux_);
  ESP_LOGD(TAG, "Phase -> %s", phase.c_str());
  if (transition.mic_closed &&
      !this->wait_for_mic_send_barrier_(
          "phase_close_mic_send_barrier_failed"))
    return;
  if (transition.follow_up_input_ended ||
      transition.follow_up_window_completed) {
    this->clear_request_follow_up_(false);
    ESP_LOGI(TAG, "explicit follow-up input ended; response owner retained");
  }
  if (phase == "idle" && !transition.active_after)
    this->cancel_timeout("va_session_ceiling");

  // Lift the post-"stop" incoming-audio suppression ONLY on "listening" — a
  // genuine fresh user turn whose reply is legitimate. We deliberately do NOT
  // lift on "idle": the backend keeps streaming the cancelled reply's audio in
  // real-time (OpenAI generated it faster than playback; a `response.cancel`
  // after the response already finished is a no-op), so it can drain for many
  // seconds AFTER the stop. An "idle" can arrive mid-drain — notably the
  // backend's thinking-watchdog force-idle — and lifting suppression there
  // un-mutes the still-arriving tail, which then plays as an "answer out of
  // nowhere" (observed live 2026-06-13 10:11). A real next reply is always
  // preceded by "listening" (the user speaks), so that stays the only safe
  // gate; until then the tail keeps being dropped. We also do NOT clear on
  // "thinking"/"replying" — those can still belong to the reply we cancelled.
  if (this->suppress_incoming_audio_ && phase == "listening") {
    this->suppress_incoming_audio_ = false;
    ESP_LOGI(TAG, "incoming-audio suppression lifted on phase=listening");
  }

  // Streaming gate state machine:
  //   listening  → mic on (user is being heard)
  //   thinking   → trusted input closes immediately; response ownership and
  //                the physical wake generation remain active
  //   replying   → trusted input remains closed; a model-selected follow-up
  //                may consume the still-active wake's one-shot budget
  //   idle       → terminal unless a PREPARED/READY explicit follow-up owns it
  // Legacy zero mode retains its pre-0.19 barge-in behavior.
  if (phase == "listening") {
    // A genuine new user turn wins over any stale or late model close request.
    this->graceful_close_prepared_token_ = 0;
    this->graceful_close_token_ = 0;
    this->cancel_timeout("va_graceful_close");
    if (this->request_follow_up_token_.load() != 0)
      this->clear_request_follow_up_(false);
    ESP_LOGI(TAG, "phase=listening confirmed for current physical owner");
    // Handsfree barge-in cut-over: a `listening` arriving while we still have
    // TTS queued means the backend's server VAD heard the user talk over the
    // reply and already cancelled the OpenAI response. Drop the audio still in
    // our PSRAM ring so playback stops immediately instead of finishing the
    // now-cancelled sentence. We do NOT send a WS interrupt here — the backend
    // initiated this — we just stop local playback.
    if (this->barge_in_ && this->audio_fill_snapshot_() > 0) {
      this->close_audio_ring_();
      this->idle_emit_pending_ = false;
      ESP_LOGI(TAG, "phase=listening during reply — barge-in, flushed TTS queue");
    }
    if (this->turn_t_listening_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_listening_ = millis();
    }
    // Server heard us — watchdog no longer needed.
    this->cancel_timeout("va_no_speech");
    this->cancel_timeout("va_followup");
  } else if (phase == "thinking" || phase == "replying") {
    if (this->request_follow_up_token_.load() != 0) {
      ESP_LOGW(TAG, "competing phase=%s revoked pending follow-up", phase.c_str());
      this->revoke_followup_("competing_phase", true);
    }
    if (!this->streaming_)
      ESP_LOGI(TAG, "phase=%s — mic streaming off", phase.c_str());
    if (phase == "thinking" && this->turn_t_thinking_ == 0 && this->turn_t_wake_ != 0) {
      this->turn_t_thinking_ = millis();
    }
    this->cancel_timeout("va_followup");
    this->cancel_timeout("va_followup_open");
    this->cancel_timeout("va_tts_tail");
    this->cancel_timeout("va_graceful_close");
    this->cancel_timeout("va_no_speech");
    this->followup_pending_ = false;
    this->waiting_for_speaker_stop_ = false;
    this->request_follow_up_pending_ = false;
    this->followup_armed_ = false;
    this->idle_emit_pending_ = false;  // new turn began, drop any held idle
  } else if (phase == "idle") {
    // Turn boundary: reset the WS-gap reference so the silence between THIS
    // reply and the NEXT turn's reply (~7 s across a follow-up exchange, where
    // start_session() — the other reset point — is never called) isn't logged
    // as a bogus "ws audio gap". Only intra-reply gaps are real signal.
    this->last_binary_ms_ = 0;
    // Only open a follow-up window if we just finished a real turn —
    // i.e. the previous phase was thinking or replying. Otherwise we'd
    // open the window for every spurious idle (initial WS hello, post-
    // disconnect idle, etc), spamming "follow-up window open" logs and
    // opening the mic for 5s every time the device just reconnects.
    const bool turn_just_ended = prev == Phase::THINKING || prev == Phase::REPLYING;
    if (!turn_just_ended) {
      // Plain idle (boot, reconnect, etc) — no follow-up. Fall through to
      // the regular trigger fire so the LED updates.
      //
      // An idle that arrives while the mic gate is still OPEN means an orphaned
      // stream that nothing else will close — shut it. Two real ways to reach
      // here with streaming_ still true:
      //   • idle straight from `listening`: the turn died without a reply (a
      //     backend force-idle on rate-limit / thinking-watchdog — the backend
      //     suppresses `thinking` after declaring a turn dead, so `listening`
      //     is exactly where the device sits — or a WS drop mid-listening).
      //   • idle from `idle` (prev==IDLE) with the mic open: the brief post-wake
      //     "waiting for the first phase" window, or an open follow-up window,
      //     when the WS drops (on_ws_event fires set_phase_("idle") on
      //     disconnect). Without this the gate stays open and the mic resumes
      //     streaming the room the instant we reconnect — and the backend's
      //     mic-resume buffer clear can't help because the stream never paused.
      // Closing here can't cut a LIVE follow-up window short: the only idle that
      // reaches an open window is exactly such a disconnect — the backend sends
      // no idle while it's waiting for the user to answer.
      if (this->streaming_) {
        ESP_LOGI(TAG, "idle while mic open (prev=%s) — closing orphaned mic gate",
                 phase_name_(prev));
        this->streaming_ = false;
        this->cancel_timeout("va_no_speech");
      }
      if (this->request_follow_up_token_.load() != 0)
        this->revoke_followup_("plain_idle", true);
    } else if (this->suppress_followup_) {
      // send_interrupt() set this — user explicitly asked us to stop.
      // Close the session cleanly: streaming off, no follow-up, fall through
      // to the regular trigger fire so the LED goes idle.
      this->suppress_followup_ = false;
      this->streaming_ = false;
      this->followup_pending_ = false;
      this->waiting_for_speaker_stop_ = false;
      this->request_follow_up_pending_ = false;
      this->followup_armed_ = false;
      this->cancel_timeout("va_tts_tail");
      this->idle_emit_pending_ = false;
      this->revoke_followup_("stop_idle", false);
    } else if (this->graceful_close_token_.load() != 0) {
      // The model judged this exchange complete. Keep the mic closed and defer
      // idle until the same speaker-drain path used by ordinary follow-ups.
      this->streaming_ = false;
      this->followup_pending_ = true;
      this->waiting_for_speaker_stop_ = false;
      this->request_follow_up_pending_ = false;
      this->followup_armed_ = false;
      this->idle_emit_pending_ = true;
      return;
    } else if (this->request_follow_up_token_.load() != 0 &&
               this->request_follow_up_pending_) {
      // A tokenized explicit request always enters the drain path, even when
      // the PSRAM ring is already empty by the time backend idle arrives.
      this->streaming_ = false;
      this->followup_pending_ = true;
      this->waiting_for_speaker_stop_ = false;
      this->idle_emit_pending_ = true;
      return;
    } else if (this->audio_fill_snapshot_() == 0) {
      // Stale-`idle` guard. prev==REPLYING with NO audio played since the last
      // wake (turn_t_first_audio_out_==0) means this `idle` belongs to a reply
      // that was stopped and then superseded by a new wake while it was still
      // draining: start_session() reset turn_t_first_audio_out_ and the old
      // tail is suppressed, so nothing actually played in THIS session. Opening
      // a follow-up here lights a spurious `listening` window over the fresh
      // wake session (observed live 2026-06-14: web-search → "stop" → bare wake
      // → ~4 s `listening` flicker; device log "Phase -> idle (was replying)").
      // Ignore it: the wake's no-speech watchdog is still armed (we never
      // reached `listening`, so it was never cancelled) and owns the session —
      // it idles the LED and closes the mic. current_phase_ is already IDLE
      // (set at the top of set_phase_), so the next message's `prev` is fine.
      // Tight by construction: a reply that really played sets
      // turn_t_first_audio_out_ (suppression lifts on `listening`), and a
      // follow-up chain keeps it set (only start_session resets it), so no
      // legitimate follow-up is skipped; the dead-turn path (prev==THINKING,
      // watchdog already cancelled) is deliberately left untouched.
      if (prev == Phase::REPLYING && this->turn_t_first_audio_out_ == 0) {
        ESP_LOGI(TAG, "stale replying-idle after wake (no audio this session) "
                      "— ignoring, no follow-up");
        // Distinguished by the mic gate: streaming_==true is a BARE WAKE (mic
        // still open, never reached `replying` this session) → the no-speech
        // watchdog is armed and owns the idle + mic close, so leave the LED on
        // the wake state. streaming_==false means the turn DID reach `replying`
        // (mic gated) but the audio never played — e.g. suppress_incoming_audio_
        // was still set from an earlier stop, so turn_t_first_audio_out_ stayed
        // 0. The watchdog was cancelled when the turn reached `listening`, so
        // nothing else fires idle → fall through to the LED trigger below, else
        // the ring strands on `replying` (observed live 2026-06-14: rapid stops
        // left suppression on, the search reply was dropped, the LED hung).
        if (this->streaming_) {
          return;  // bare wake: va_no_speech owns the idle + mic
        }
        // else: fall through to fire the idle LED (still no follow-up).
      } else {
        // Server says response.done and the device has actually played out.
        // Open the follow-up window (mic on so user can answer a question).
        this->open_followup_window_(this->followup_ms_);
      }
      // fall through to fire the trigger normally below (LED -> idle); the
      // follow-up window, if any, fires its own `listening` LED after its delay.
    } else {
      // Server says response.done, but we still have seconds of TTS queued
      // in PSRAM + downstream rings. Two things wait on the queue:
      //   1) the LED transition to idle (otherwise it goes off while the
      //      device is still speaking)
      //   2) opening the follow-up mic window (echo + false VAD trigger)
      // Mark both pending; the drain handler in loop() releases them
      // together after the speaker actually finishes.
      ESP_LOGI(TAG, "phase=idle but %u bytes still queued; LED + follow-up deferred",
               (unsigned) this->audio_fill_snapshot_());
      this->followup_pending_ = true;
      this->idle_emit_pending_ = true;
      return;  // suppress immediate trigger fire — open_followup_window_ will fire it later
    }
    // A completed response never grants another request. This only revokes the
    // current wake; it does not replenish the already-spent one-shot budget.
  }

  // set_phase_ may be called from the websocket task; ESPHome triggers and
  // most component APIs are not thread-safe. Marshal the side effects onto
  // the main loop via defer().
  PhaseApplyResult effect_plan = transition;
  portENTER_CRITICAL(&this->followup_mux_);
  effect_plan.active_after = this->lifecycle_.active_wake();
  effect_plan.connection_generation = this->lifecycle_.connection_generation();
  effect_plan.session_nonce = this->lifecycle_.session_nonce();
  effect_plan.wake_generation = this->lifecycle_.wake_generation();
  effect_plan.effect_epoch = this->lifecycle_.effect_epoch();
  portEXIT_CRITICAL(&this->followup_mux_);
  std::string phase_copy = phase;
  this->defer([this, phase_copy, runtime_phase, effect_plan]() {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    if (static_cast<Phase>(this->current_phase_.load()) != runtime_phase ||
        !this->phase_effect_plan_current_(effect_plan))
      return;
    // We deliberately do NOT call speaker->stop() on "listening" anymore:
    // the speaker task runs continuously after setup() and play() just
    // appends to its ring buffer. Stop/start churn was creating multiple
    // speaker_task instances racing for the i2s channel ("Parent bus is
    // busy"). For barge-in/interrupt we'll add a buffer-flush API in M3.
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

uint32_t VaClient::prepare_local_wake() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (!this->transport_admission_.can_start_wake()) {
    ESP_LOGW(TAG, "local wake rejected: a newly admitted backend session is required");
    return 0;
  }
  if (this->enroll_mode_ || this->microphone_is_muted_()) {
    ESP_LOGW(TAG, "local wake rejected: enrollment or mute active");
    return 0;
  }
  bool admitted = false;
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.set_muted(false);
  admitted = this->lifecycle_.connected() && this->lifecycle_.authorized() &&
             !this->lifecycle_.enrollment();
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!admitted) {
    ESP_LOGW(TAG, "local wake rejected: backend hello not admitted");
    return 0;
  }

  const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
  const size_t audio_fill = this->audio_fill_snapshot_();
  const bool residual_reply =
      !this->post_stop_guard_ &&
      (audio_fill > 0 || this->idle_emit_pending_ || phase_now == Phase::REPLYING ||
       phase_now == Phase::THINKING);
  ControlContext old_context;

  // Revoke every old grant and cancel every delayed opener before any network
  // control is attempted. The replacement grant is created only afterwards.
  if (!this->revoke_followup_("new_wake", false, false, &old_context)) {
    ESP_LOGW(TAG, "local wake rejected: prior mic send barrier did not drain");
    return 0;
  }
  bool close_sent = false;
  if (residual_reply) {
    this->close_audio_ring_();
    this->suppress_incoming_audio_ = true;
    close_sent = this->send_interrupt_control_(
        "new_wake", old_context.session_nonce, old_context.wake_generation);
  } else {
    close_sent = this->send_client_revoke_(
        "new_wake", old_context.session_nonce, old_context.wake_generation);
  }
  if (!close_sent) {
    ESP_LOGW(TAG, "local wake rejected: prior generation close was not delivered");
    return 0;
  }

  uint32_t wake_reservation = 0;
  const bool microphone_muted = this->microphone_is_muted_();
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.set_muted(microphone_muted);
  wake_reservation = this->lifecycle_.prepare_local_wake();
  portEXIT_CRITICAL(&this->followup_mux_);
  if (wake_reservation == 0) {
    ESP_LOGW(TAG, "local wake rejected: no admitted backend session");
    return 0;
  }

  this->post_stop_guard_ = false;
  this->suppress_followup_ = false;
  this->graceful_close_prepared_token_ = 0;
  this->graceful_close_token_ = 0;
  ESP_LOGI(TAG, "local wake pending with mic closed");
  return wake_reservation;
}

bool VaClient::pending_wake_is_safe(uint32_t wake_reservation) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (!this->transport_admission_.can_start_wake())
    return false;
  if (this->mic_ == nullptr)
    return false;
  bool safe = false;
  const bool microphone_muted = this->microphone_is_muted_();
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.set_muted(microphone_muted);
  safe = this->lifecycle_.pending_wake_is_safe(wake_reservation);
  portEXIT_CRITICAL(&this->followup_mux_);
  return safe;
}

bool VaClient::explicit_followup_active() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  bool active = false;
  portENTER_CRITICAL(&this->followup_mux_);
  active = this->lifecycle_.follow_up_stage() == FollowUpStage::OPEN &&
           this->lifecycle_.mic_open();
  portEXIT_CRITICAL(&this->followup_mux_);
  return active;
}

void VaClient::abort_local_wake(uint32_t wake_reservation) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  bool aborted = false;
  portENTER_CRITICAL(&this->followup_mux_);
  aborted = this->lifecycle_.abort_pending_wake(wake_reservation);
  if (aborted)
    this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!aborted)
    return;
  this->cancel_session_timers_();
}

bool VaClient::start_session(uint32_t wake_reservation) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (!this->transport_admission_.can_start_wake())
    return false;
  if (!this->pending_wake_is_safe(wake_reservation)) {
    return false;
  }

  uint32_t session_nonce = 0;
  uint32_t protocol_wake_generation = 0;
  portENTER_CRITICAL(&this->followup_mux_);
  session_nonce = this->lifecycle_.session_nonce();
  protocol_wake_generation =
      this->lifecycle_.pending_protocol_wake_generation(wake_reservation);
  portEXIT_CRITICAL(&this->followup_mux_);
  if (protocol_wake_generation == 0)
    return false;

  // Reservation ids are local-only. The protocol generation advances only
  // after this exact bounded wake send succeeds.
  if (!this->send_wake_(session_nonce, protocol_wake_generation)) {
    this->revoke_followup_("wake_send_failed", false);
    return false;
  }

  bool opened = false;
  bool transmitted = false;
  const bool microphone_muted = this->microphone_is_muted_();
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.set_muted(microphone_muted);
  transmitted = this->lifecycle_.record_wake_transmitted(
      wake_reservation, protocol_wake_generation);
  opened = transmitted &&
           this->lifecycle_.open_transmitted_wake(wake_reservation);
  this->streaming_ = opened;
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!opened) {
    this->revoke_followup_("wake_commit_race", false);
    if (transmitted)
      this->send_client_revoke_("wake_commit_race", session_nonce,
                                protocol_wake_generation);
    return false;
  }

  this->preroll_discard_pending_ = true;
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->idle_emit_pending_ = false;
  this->cancel_session_timers_();
  this->turn_t_wake_ = millis();
  this->turn_t_listening_ = 0;
  this->turn_t_thinking_ = 0;
  this->turn_t_first_audio_out_ = 0;
  this->last_binary_ms_ = 0;
  this->ws_gap_count_ = 0;
  this->ws_gap_max_ms_ = 0;
  this->clipped_samples_ = 0;
  this->underrun_logged_this_turn_ = false;
  ESP_LOGI(TAG, "local wake committed with mic open");

  this->set_timeout("va_no_speech", kNoSpeechTimeoutMs,
                    [this, protocol_wake_generation]() {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    bool close = false;
    bool mic_was_open = false;
    uint32_t session_nonce = 0;
    portENTER_CRITICAL(&this->followup_mux_);
    close = this->lifecycle_.silent_wake_timeout_matches(
        protocol_wake_generation);
    if (close) {
      session_nonce = this->lifecycle_.session_nonce();
      mic_was_open = this->lifecycle_.mic_open();
      this->lifecycle_.revoke();
      this->streaming_ = false;
    }
    portEXIT_CRITICAL(&this->followup_mux_);
    if (!close)
      return;

    if (!this->wait_for_mic_send_barrier_(
            "silent_wake_mic_send_barrier_failed"))
      return;
    ESP_LOGI(TAG, "silent wake timed out");
    if (mic_was_open)
      this->send_mic_flush_(session_nonce, protocol_wake_generation);
    this->send_interrupt_control_("silent_wake", session_nonce,
                                  protocol_wake_generation);
    this->turn_t_wake_ = 0;
    this->fire_phase_led_("idle");
  });

  this->set_timeout(
      "va_session_ceiling", kAbsoluteSessionMaxMs,
      [this, protocol_wake_generation]() {
        std::lock_guard<GenerationEffectGate> effect_guard(
            this->generation_effect_gate_);
        bool close = false;
        bool mic_was_open = false;
        uint32_t session_nonce = 0;
        portENTER_CRITICAL(&this->followup_mux_);
        close = this->lifecycle_.absolute_session_timeout_matches(
            protocol_wake_generation);
        if (close) {
          session_nonce = this->lifecycle_.session_nonce();
          mic_was_open = this->lifecycle_.mic_open();
          this->lifecycle_.revoke();
          this->streaming_ = false;
        }
        portEXIT_CRITICAL(&this->followup_mux_);
        if (!close || !this->wait_for_mic_send_barrier_(
                          "session_ceiling_mic_send_barrier_failed"))
          return;
        this->clear_request_follow_up_(false);
        if (mic_was_open)
          this->send_mic_flush_(session_nonce, protocol_wake_generation);
        this->send_interrupt_control_("session_ceiling", session_nonce,
                                      protocol_wake_generation);
        ESP_LOGW(TAG, "absolute voice-session safety ceiling reached");
        this->fire_phase_led_("idle");
      });
  return true;
}

void VaClient::open_followup_window_(uint32_t duration_ms) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  // If a phase=idle LED transition was held back while audio drained, fire
  // it now so the LED goes to idle in sync with the speaker actually going
  // quiet (instead of as soon as the server emitted response.done).
  if (this->idle_emit_pending_) {
    this->idle_emit_pending_ = false;
    this->fire_phase_led_("idle");
    // Per-turn latency summary. Anchors are zero if we skipped a milestone
    // (e.g. interrupt mid-reply); show "?" so the line stays readable.
    if (this->turn_t_wake_ != 0) {
      uint32_t now = millis();
      auto fmt = [](uint32_t from, uint32_t to) -> std::string {
        if (from == 0 || to == 0 || to < from)
          return "?";
        return std::to_string(to - from) + "ms";
      };
      ESP_LOGI(TAG,
               "turn latency: wake→listening=%s listening→thinking=%s "
               "thinking→first_audio=%s first_audio→played_out=%s "
               "total=%s",
               fmt(this->turn_t_wake_, this->turn_t_listening_).c_str(),
               fmt(this->turn_t_listening_, this->turn_t_thinking_).c_str(),
               fmt(this->turn_t_thinking_, this->turn_t_first_audio_out_).c_str(),
               fmt(this->turn_t_first_audio_out_, now).c_str(),
               fmt(this->turn_t_wake_, now).c_str());
      // Audio-quality summary: only logged if anything anomalous fired.
      // A clean turn produces no line — keeps the noise floor low.
      if (this->ws_gap_count_ > 0 || this->clipped_samples_ > 0 ||
          this->underrun_logged_this_turn_) {
        ESP_LOGW(TAG,
                 "turn audio: ws_gaps=%u (max=%ums) clipped_samples=%u underrun=%s",
                 (unsigned) this->ws_gap_count_,
                 (unsigned) this->ws_gap_max_ms_,
                 (unsigned) this->clipped_samples_,
                 this->underrun_logged_this_turn_ ? "yes" : "no");
      }
      this->turn_t_wake_ = 0;  // mark turn as logged
    }
  }
  // RAPID-PILOT is deliberately staged in zero mode. Legacy automatic
  // post-reply mic timers are not authorized to open the mic at any duration;
  // only the nonce-bound two-phase explicit path can do so.
  this->streaming_ = false;
  if (duration_ms != 0) {
    ESP_LOGE(TAG, "non-zero legacy follow-up rejected in RAPID-PILOT mode");
    this->revoke_followup_("legacy_follow_up_nonzero", true);
  }
}

void VaClient::enroll_start() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (this->enroll_mode_)
    return;
  if (this->microphone_is_muted_()) {
    ESP_LOGW(TAG, "enrollment rejected while microphone is muted");
    return;
  }
  ESP_LOGI(TAG, "enrollment START — mic pinned open, session timers suspended");
  if (!this->revoke_followup_("enrollment", true))
    return;
  this->cancel_session_timers_();
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->idle_emit_pending_ = false;
  this->suppress_followup_ = false;
  this->graceful_close_prepared_token_ = 0;
  this->graceful_close_token_ = 0;
  this->post_stop_guard_ = false;
  this->suppress_incoming_audio_ = false;
  this->preroll_discard_pending_ = true;
  this->enroll_mode_ = true;
  this->enroll_started_ms_ = millis();
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.set_muted(false);
  this->streaming_ = this->lifecycle_.start_enrollment();
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!this->streaming_) {
    this->enroll_mode_ = false;
    portENTER_CRITICAL(&this->followup_mux_);
    this->lifecycle_.stop_enrollment();
    portEXIT_CRITICAL(&this->followup_mux_);
    ESP_LOGW(TAG, "enrollment rejected: backend session is not admitted");
    return;
  }
  this->set_phase_("enrolling");
  // Hard cap: a forgotten/hung session must not stream the household forever.
  this->set_timeout("va_enroll_cap", kEnrollMaxMs, [this]() {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    ESP_LOGW(TAG, "enrollment hit the safety cap — stopping");
    this->enroll_stop(true);
  });
}

void VaClient::enroll_stop(bool notify_backend) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (!this->enroll_mode_)
    return;
  ESP_LOGI(TAG, "enrollment STOP (%s) after %u s",
           notify_backend ? "device-initiated" : "backend-initiated",
           (unsigned) ((millis() - this->enroll_started_ms_) / 1000));
  ControlContext context;
  this->cancel_timeout("va_enroll_cap");
  this->enroll_mode_ = false;
  portENTER_CRITICAL(&this->followup_mux_);
  context.session_nonce = this->lifecycle_.session_nonce();
  context.wake_generation = this->lifecycle_.wake_generation();
  this->lifecycle_.stop_enrollment();
  this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  const bool barrier_clear = this->wait_for_mic_send_barrier_(
      "enroll_mic_send_barrier_failed");
  // Don't let the routine idle transition open a follow-up mic window — the
  // session is over, the device should go fully to rest.
  this->suppress_followup_ = true;
  if (barrier_clear && notify_backend && this->legacy_zero_mode_()) {
    if (!this->send_text_bounded_("{\"type\":\"enroll_stopped\"}",
                                  "enroll_stopped")) {
      this->quarantine_transport_("enroll_stopped_send_failed");
    }
  } else if (barrier_clear && notify_backend) {
    // Trusted enrollment uses the same authoritative close control as every
    // other mic owner. The backend stops enrollment on client_revoke; if the
    // bounded send fails, quarantine closes the socket and stops it there too.
    this->send_client_revoke_("enrollment_stopped", context.session_nonce,
                              context.wake_generation);
  }
  this->set_phase_("idle");
}

void VaClient::send_button_cancel() {
  ControlContext context;
  if (!this->revoke_followup_("button_cancel", false, true, &context))
    return;
  std::string message = this->legacy_zero_mode_()
                            ? "{\"type\":\"button_cancel\"}"
                            : "{\"type\":\"button_cancel\",\"session_nonce\":" +
                                  std::to_string(context.session_nonce) +
                                  ",\"wake_generation\":" +
                                  std::to_string(context.wake_generation) + "}";
  if (this->send_text_bounded_(message, "button_cancel"))
    ESP_LOGI(TAG, "button cancel sent");
}

void VaClient::send_false_flag() {
  ControlContext context;
  if (!this->revoke_followup_("false_flag", false, true, &context))
    return;
  std::string message = this->legacy_zero_mode_()
                            ? "{\"type\":\"false_flag\"}"
                            : "{\"type\":\"false_flag\",\"session_nonce\":" +
                                  std::to_string(context.session_nonce) +
                                  ",\"wake_generation\":" +
                                  std::to_string(context.wake_generation) + "}";
  if (this->send_text_bounded_(message, "false_flag"))
    ESP_LOGI(TAG, "explicit false-wake flag sent (double-press)");
}

bool VaClient::send_mic_flush_(uint32_t session_nonce,
                               uint32_t wake_generation) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  // The mic gate just closed mid-stream because a follow-up window timed out.
  // If the user had started (but not finished) speaking, that audio sits
  // UNCOMMITTED in OpenAI's input_audio_buffer; left there, a later wake's
  // audio "completes" it and the model answers a stale half-sentence. Drop it
  // NOW, at the cut-off, so no reactive clear-on-wake is needed (that disturbed
  // the server VAD and caused garbage commits). This timer only fires when the
  // user did NOT trigger speech — `listening` cancels va_followup — so it can
  // never drop a valid command. Cheap no-op when the buffer was empty.
  std::string message = this->legacy_zero_mode_()
                            ? "{\"type\":\"flush\"}"
                            : "{\"type\":\"flush\",\"session_nonce\":" +
                                  std::to_string(session_nonce) +
                                  ",\"wake_generation\":" +
                                  std::to_string(wake_generation) + "}";
  const bool sent = this->send_text_bounded_(message, "flush");
  if (sent) {
    ESP_LOGI(TAG, "follow-up window closed — sent flush (drop uncommitted mic audio)");
  } else {
    this->quarantine_transport_("flush_send_failed");
  }
  return sent;
}

bool VaClient::send_wake_(uint32_t session_nonce,
                          uint32_t wake_generation) {
  // Tell the backend a fresh wake started (dangling-VAD guard, A). Until the
  // user actually speaks this turn, OpenAI's server VAD can still fire an
  // end-of-turn for a PREVIOUS utterance that never closed (the reply gated the
  // mic mid-sentence) — committing it auto-creates a garbage answer to an empty
  // turn. The backend uses this signal to suppress that stale thinking + cancel
  // the racing response. Sent on every start_session(); old backends ignore it.
  std::string message = this->legacy_zero_mode_()
                             ? "{\"type\":\"wake\"}"
                             : "{\"type\":\"wake\",\"session_nonce\":" +
                                   std::to_string(session_nonce) +
                                   ",\"wake_generation\":" +
                                   std::to_string(wake_generation) + "}";
  const bool sent = this->send_text_bounded_(message, "wake");
  if (sent) {
    ESP_LOGI(TAG, "wake — sent {\"type\":\"wake\"} (dangling-VAD guard)");
  }
  return sent;
}

bool VaClient::send_hello_ack_(uint32_t nonce, bool accepted) {
  std::string ack = "{\"type\":\"hello_ack\",\"nonce\":" + std::to_string(nonce);
  ack += accepted ? ",\"accepted\":true" : ",\"accepted\":false";
  ack += ",\"audio_out\":\"pcm\",\"follow_up_ms\":" +
         std::to_string(this->followup_ms_);
  ack += ",\"follow_up_open_delay_ms\":" +
         std::to_string(this->followup_open_delay_ms_);
  ack += ",\"wake_open_delay_ms\":" + std::to_string(this->wake_open_delay_ms_);
  ack += ",\"playback_prebuffer_ms\":" + std::to_string(this->playback_prebuffer_ms_) + "}";
  const bool sent = this->send_text_bounded_(ack, "hello_ack");
  if (accepted && !sent) {
    this->session_nonce_ = 0;
    portENTER_CRITICAL(&this->followup_mux_);
    this->lifecycle_.reject_hello();
    portEXIT_CRITICAL(&this->followup_mux_);
  }
  return sent;
}

bool VaClient::send_request_follow_up_ack_(uint32_t token, uint32_t session_nonce,
                                           bool accepted) {
  std::string ack = "{\"type\":\"request_follow_up_ack\",\"token\":" +
                    std::to_string(token);
  ack += ",\"session_nonce\":" + std::to_string(session_nonce);
  ack += accepted ? ",\"accepted\":true}" : ",\"accepted\":false}";
  return this->send_text_bounded_(ack, "request_follow_up_ack");
}

bool VaClient::send_cancel_request_follow_up_ack_(uint32_t token,
                                                  uint32_t session_nonce,
                                                  bool accepted, bool cleared) {
  std::string ack = "{\"type\":\"cancel_request_follow_up_ack\",\"token\":" +
                    std::to_string(token);
  ack += ",\"session_nonce\":" + std::to_string(session_nonce);
  ack += accepted ? ",\"accepted\":true" : ",\"accepted\":false";
  ack += cleared ? ",\"cleared\":true}" : ",\"cleared\":false}";
  return this->send_text_bounded_(ack, "cancel_request_follow_up_ack");
}

bool VaClient::send_follow_up_ready_(const FollowUpCredentials &credentials) {
  std::string message = "{\"type\":\"follow_up_ready\",\"token\":" +
                        std::to_string(credentials.token) +
                        ",\"session_nonce\":" +
                        std::to_string(credentials.session_nonce) +
                        ",\"ready_nonce\":" +
                        std::to_string(credentials.ready_nonce) + "}";
  return this->send_text_bounded_(message, "follow_up_ready");
}

bool VaClient::send_follow_up_commit_ack_(uint32_t token, uint32_t session_nonce,
                                          uint32_t ready_nonce, bool accepted) {
  std::string message = "{\"type\":\"commit_follow_up_ack\",\"token\":" +
                        std::to_string(token) + ",\"session_nonce\":" +
                        std::to_string(session_nonce) + ",\"ready_nonce\":" +
                        std::to_string(ready_nonce);
  message += accepted ? ",\"accepted\":true}" : ",\"accepted\":false}";
  return this->send_text_bounded_(message, "commit_follow_up_ack");
}

bool VaClient::send_client_revoke_(const char *reason, uint32_t session_nonce,
                                   uint32_t wake_generation) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  if (this->legacy_zero_mode_())
    return true;
  std::string message = "{\"type\":\"client_revoke\",\"session_nonce\":" +
                        std::to_string(session_nonce) +
                        ",\"wake_generation\":" +
                        std::to_string(wake_generation) + ",\"reason\":\"" +
                        reason + "\"}";
  const bool sent = this->send_text_bounded_(message, "client_revoke");
  if (!sent)
    this->quarantine_transport_("client_revoke_send_failed");
  return sent;
}

bool VaClient::send_interrupt_control_(const char *reason, uint32_t session_nonce,
                                       uint32_t wake_generation) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  std::string message = this->legacy_zero_mode_()
                            ? "{\"type\":\"interrupt\"}"
                            : "{\"type\":\"interrupt\",\"session_nonce\":" +
                                  std::to_string(session_nonce) +
                                  ",\"wake_generation\":" +
                                  std::to_string(wake_generation) + ",\"reason\":\"" +
                                  reason + "\"}";
  const bool sent = this->send_text_bounded_(message, "interrupt");
  if (!sent)
    this->quarantine_transport_("interrupt_send_failed");
  return sent;
}

void VaClient::clear_request_follow_up_(bool close_window) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  ControlContext context;
  uint32_t token = 0;
  bool lifecycle_window_was_open = false;
  bool should_settle_idle = false;
  portENTER_CRITICAL(&this->followup_mux_);
  context.session_nonce = this->lifecycle_.session_nonce();
  context.wake_generation = this->lifecycle_.wake_generation();
  token = this->request_follow_up_token_.exchange(0);
  lifecycle_window_was_open =
      this->lifecycle_.follow_up_stage() == FollowUpStage::OPEN &&
      this->lifecycle_.mic_open();
  should_settle_idle =
      (token != 0 || lifecycle_window_was_open) &&
      static_cast<Phase>(this->current_phase_.load()) == Phase::IDLE;
  if (close_window) {
    this->lifecycle_.revoke();
  }
  this->request_follow_up_pending_ = false;
  this->followup_armed_ = false;
  this->request_follow_up_callback_in_flight_ = false;
  this->request_follow_up_callback_token_ = 0;
  this->request_follow_up_callback_session_nonce_ = 0;
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->idle_emit_pending_ = false;
  if (close_window)
    this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  this->cancel_timeout("va_followup_commit");
  this->cancel_timeout("va_request_follow_up_hard");
  const bool barrier_clear =
      !close_window ||
      this->wait_for_mic_send_barrier_(
          "follow_up_close_mic_send_barrier_failed");
  if (close_window && lifecycle_window_was_open) {
    if (barrier_clear)
      this->send_mic_flush_(context.session_nonce, context.wake_generation);
  }
  if (close_window && should_settle_idle)
    this->fire_phase_led_("idle");
}

void VaClient::cancel_session_timers_() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  this->cancel_timeout("va_no_speech");
  this->cancel_timeout("va_followup");
  this->cancel_timeout("va_followup_open");
  this->cancel_timeout("va_tts_tail");
  this->cancel_timeout("va_graceful_close");
  this->cancel_timeout("va_followup_commit");
  this->cancel_timeout("va_request_follow_up_hard");
  this->cancel_timeout("va_session_ceiling");
}

VaClient::ControlContext VaClient::control_context_() {
  ControlContext context;
  portENTER_CRITICAL(&this->followup_mux_);
  context.session_nonce = this->lifecycle_.session_nonce();
  context.wake_generation = this->lifecycle_.wake_generation();
  context.effect_epoch = this->lifecycle_.effect_epoch();
  portEXIT_CRITICAL(&this->followup_mux_);
  return context;
}

bool VaClient::legacy_zero_mode_() {
  bool legacy = false;
  portENTER_CRITICAL(&this->followup_mux_);
  legacy = this->lifecycle_.legacy_zero();
  portEXIT_CRITICAL(&this->followup_mux_);
  return legacy;
}

void VaClient::close_audio_ring_() {
  portENTER_CRITICAL(&this->ring_mux_);
  this->audio_head_ = 0;
  this->audio_tail_ = 0;
  this->audio_fill_ = 0;
  this->playback_prebuffer_pending_ = false;
  this->playback_priming_ = false;
  this->prime_started_ms_ = 0;
  this->prebuffer_generation_++;
  portEXIT_CRITICAL(&this->ring_mux_);
  this->chain_prime_remaining_ = 0;
}

size_t VaClient::audio_fill_snapshot_() {
  portENTER_CRITICAL(&this->ring_mux_);
  const size_t fill = this->audio_fill_;
  portEXIT_CRITICAL(&this->ring_mux_);
  return fill;
}

bool VaClient::microphone_is_muted_() {
  return this->external_mute_.load() || this->mic_ == nullptr ||
         this->mic_->get_mute_state();
}

bool VaClient::announcement_path_clear_() {
  return !this->announcement_active_.load() &&
         this->announcement_speaker_ != nullptr &&
         !this->announcement_speaker_->has_buffered_data();
}

bool VaClient::wait_for_mic_send_barrier_(const char *failure_reason) {
  const uint32_t started_ms = millis();
  while (this->mic_send_fence_.in_flight() != 0 &&
         millis() - started_ms < kMicSendBarrierTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  const bool clear = this->mic_send_fence_.in_flight() == 0;
  if (!clear) {
    ESP_LOGE(TAG,
             "mic send barrier did not drain; network close controls suppressed");
    this->quarantine_transport_(failure_reason);
  }
  return clear;
}

void VaClient::quarantine_transport_(const char *reason) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  const bool first_fault =
      this->transport_admission_.on_authoritative_close_failure();
  this->ws_connected_ = false;
  this->session_nonce_ = 0;
  uint32_t fault_generation = this->ws_connection_generation_.load();
  if (first_fault) {
    fault_generation++;
    if (fault_generation == 0)
      fault_generation = 1;
    this->ws_connection_generation_ = fault_generation;
  }
  this->cancel_timeout("va_enroll_cap");
  this->enroll_mode_ = false;
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.on_disconnected();
  this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  this->clear_request_follow_up_(false);
  this->cancel_session_timers_();
  if (!first_fault)
    return;

  ESP_LOGE(TAG, "authoritative close failed (%s); transport quarantined", reason);
  this->defer([this, fault_generation]() {
    bool should_stop = false;
    esp_websocket_client_handle_t handle = nullptr;
    {
      std::lock_guard<GenerationEffectGate> effect_guard(
          this->generation_effect_gate_);
      should_stop =
          this->ws_connection_generation_.load() == fault_generation;
      handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
    }
    if (!should_stop)
      return;
    const esp_err_t stop_result =
        handle == nullptr ? ESP_OK : esp_websocket_client_stop(handle);
    esp_err_t destroy_result = ESP_FAIL;
    if (handle != nullptr && stop_result != ESP_OK)
      destroy_result = esp_websocket_client_destroy(handle);
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    if (destroy_result == ESP_OK && this->ws_handle_ == handle)
      this->ws_handle_ = nullptr;
    if (this->ws_connection_generation_.load() != fault_generation)
      return;
    if (stop_result == ESP_OK || destroy_result == ESP_OK) {
      // A successful stop/destroy establishes the transport boundary. Mark it
      // here as well as in the event callback so a missing callback or an
      // already-absent handle cannot leave the next real connection unable to
      // admit its hello.
      this->transport_admission_.on_transport_disconnected();
    } else {
      ESP_LOGE(TAG, "quarantined websocket stop/destroy failed: %d/%d",
               (int) stop_result, (int) destroy_result);
    }
    this->schedule_reconnect_();
  });
}

bool VaClient::revoke_followup_(const char *reason, bool notify_backend,
                                bool post_stop,
                                ControlContext *closed_context) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  ControlContext context;
  bool mic_was_open = false;
  portENTER_CRITICAL(&this->followup_mux_);
  context.session_nonce = this->lifecycle_.session_nonce();
  context.wake_generation = this->lifecycle_.wake_generation();
  mic_was_open = this->streaming_.load() || this->lifecycle_.mic_open();
  if (post_stop)
    this->lifecycle_.stop();
  else
    this->lifecycle_.revoke();
  this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  if (closed_context != nullptr)
    *closed_context = context;
  const bool barrier_clear = this->wait_for_mic_send_barrier_(
      "revoke_mic_send_barrier_failed");
  this->clear_request_follow_up_(false);
  this->cancel_session_timers_();
  if (!barrier_clear) {
    return false;
  }
  bool controls_sent = true;
  if (mic_was_open)
    controls_sent = this->send_mic_flush_(context.session_nonce,
                                          context.wake_generation);
  if (controls_sent && notify_backend) {
    controls_sent = this->send_client_revoke_(
        reason, context.session_nonce, context.wake_generation);
  }
  return controls_sent;
}

void VaClient::revoke_followup() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  this->revoke_followup_("manual_revoke", true);
}

void VaClient::revoke_for_mute() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  this->external_mute_ = true;
  ControlContext context;
  bool mic_was_open = false;
  portENTER_CRITICAL(&this->followup_mux_);
  context.session_nonce = this->lifecycle_.session_nonce();
  context.wake_generation = this->lifecycle_.wake_generation();
  mic_was_open = this->streaming_.load() || this->lifecycle_.mic_open();
  this->lifecycle_.set_muted(true);
  this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  const bool barrier_clear = this->wait_for_mic_send_barrier_(
      "mute_mic_send_barrier_failed");
  this->clear_request_follow_up_(false);
  this->cancel_session_timers_();
  if (!barrier_clear)
    return;
  if (barrier_clear && mic_was_open)
    this->send_mic_flush_(context.session_nonce, context.wake_generation);
  if (barrier_clear)
    this->send_client_revoke_("mute", context.session_nonce,
                              context.wake_generation);
}

void VaClient::release_mute() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  this->external_mute_ = false;
  const bool microphone_muted = this->microphone_is_muted_();
  portENTER_CRITICAL(&this->followup_mux_);
  this->lifecycle_.set_muted(microphone_muted);
  portEXIT_CRITICAL(&this->followup_mux_);
}

void VaClient::set_announcement_active(bool active) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  this->announcement_active_ = active;
  if (!active)
    return;

  ControlContext context;
  bool revoke = false;
  bool mic_was_open = false;
  portENTER_CRITICAL(&this->followup_mux_);
  const FollowUpStage stage = this->lifecycle_.follow_up_stage();
  revoke = this->lifecycle_.mic_open() || stage == FollowUpStage::READY;
  if (revoke) {
    context.session_nonce = this->lifecycle_.session_nonce();
    context.wake_generation = this->lifecycle_.wake_generation();
    mic_was_open = this->lifecycle_.mic_open();
    this->lifecycle_.revoke();
    this->streaming_ = false;
  }
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!revoke)
    return;

  const bool barrier_clear = this->wait_for_mic_send_barrier_(
      "announcement_mic_send_barrier_failed");
  this->clear_request_follow_up_(false);
  this->cancel_session_timers_();
  if (!barrier_clear)
    return;
  if (barrier_clear && mic_was_open)
    this->send_mic_flush_(context.session_nonce, context.wake_generation);
  if (barrier_clear)
    this->send_client_revoke_("announcement_started", context.session_nonce,
                              context.wake_generation);
}

void VaClient::send_graceful_close_ack_(const char *stage, uint32_t token, bool accepted) {
  std::string ack = "{\"type\":\"suppress_followup_ack\",\"stage\":\"";
  ack += stage;
  ack += "\",\"token\":" + std::to_string(token);
  if (!this->legacy_zero_mode_()) {
    const ControlContext context = this->control_context_();
    ack += ",\"session_nonce\":" + std::to_string(context.session_nonce);
    ack += ",\"wake_generation\":" + std::to_string(context.wake_generation);
  }
  ack += accepted ? ",\"accepted\":true}" : ",\"accepted\":false}";
  this->send_text_bounded_(ack, "suppress_followup_ack");
}

void VaClient::fire_phase_led_(const std::string &phase) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  // Drive the yaml on_phase automation (LED ring + voice_assistant_phase global)
  // from a device-side timer, not a server message. Marshalled via defer() so it
  // runs on the main loop even if called from another task.
  uint32_t effect_epoch = 0;
  portENTER_CRITICAL(&this->followup_mux_);
  effect_epoch = this->lifecycle_.effect_epoch();
  portEXIT_CRITICAL(&this->followup_mux_);
  std::string phase_copy = phase;
  this->defer([this, phase_copy, effect_epoch]() {
    std::lock_guard<GenerationEffectGate> effect_guard(
        this->generation_effect_gate_);
    bool current = false;
    portENTER_CRITICAL(&this->followup_mux_);
    current = this->lifecycle_.effect_epoch_matches(effect_epoch);
    portEXIT_CRITICAL(&this->followup_mux_);
    if (!current)
      return;
    for (auto *t : this->phase_triggers_) {
      t->trigger(phase_copy);
    }
  });
}

uint32_t VaClient::generate_ready_nonce_() {
  for (size_t attempt = 0; attempt < 16; attempt++) {
    const uint32_t candidate = esp_random() & kProtocolTokenMax;
    bool available = false;
    portENTER_CRITICAL(&this->followup_mux_);
    available = this->lifecycle_.ready_nonce_available(candidate);
    portEXIT_CRITICAL(&this->followup_mux_);
    if (available)
      return candidate;
  }
  return 0;
}

bool VaClient::mark_followup_ready(uint32_t token, uint32_t session_nonce) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  bool ring_empty = false;
  portENTER_CRITICAL(&this->ring_mux_);
  ring_empty = this->audio_fill_ == 0;
  portEXIT_CRITICAL(&this->ring_mux_);

  const bool speaker_drained =
      this->speaker_ != nullptr && !this->speaker_->has_buffered_data();
  const bool announcement_clear = this->announcement_path_clear_();
  const bool microphone_ready = !this->microphone_is_muted_();
  const Phase phase_now = static_cast<Phase>(this->current_phase_.load());
  const uint32_t ready_nonce = this->generate_ready_nonce_();
  FollowUpCredentials credentials;
  bool ready = false;
  bool stale_callback = false;
  bool owned_failure = false;

  portENTER_CRITICAL(&this->followup_mux_);
  credentials = this->lifecycle_.credentials();
  const uint32_t audio_generation = this->lifecycle_.audio_generation();
  const bool callback_matches =
      this->request_follow_up_callback_in_flight_.load() &&
      this->request_follow_up_callback_token_.load() == token &&
      this->request_follow_up_callback_session_nonce_.load() == session_nonce;
  const bool request_matches =
      this->lifecycle_.follow_up_stage() != FollowUpStage::NONE &&
      credentials.token == token && credentials.session_nonce == session_nonce;
  stale_callback = !callback_matches || !request_matches;
  const bool safe_to_ready = !stale_callback && this->followup_armed_.load() &&
      ready_nonce != 0 &&
      this->graceful_close_prepared_token_.load() == 0 &&
      this->graceful_close_token_.load() == 0 &&
      this->followup_ms_.load() == 0 && !this->barge_in_ &&
      this->ws_connected_.load() && !this->enroll_mode_.load() &&
      !this->streaming_.load() && phase_now == Phase::IDLE && ring_empty &&
       speaker_drained && announcement_clear &&
       this->mic_send_fence_.in_flight() == 0 && microphone_ready;
  if (safe_to_ready && this->lifecycle_.mark_follow_up_ready(
                           token, session_nonce, ready_nonce, audio_generation)) {
    this->followup_armed_ = false;
    this->request_follow_up_callback_in_flight_ = false;
    this->request_follow_up_callback_token_ = 0;
    this->request_follow_up_callback_session_nonce_ = 0;
    credentials = this->lifecycle_.credentials();
    ready = true;
  } else if (!stale_callback) {
    this->lifecycle_.revoke();
    this->streaming_ = false;
    this->request_follow_up_token_ = 0;
    this->request_follow_up_pending_ = false;
    this->followup_armed_ = false;
    this->request_follow_up_callback_in_flight_ = false;
    this->request_follow_up_callback_token_ = 0;
    this->request_follow_up_callback_session_nonce_ = 0;
    this->followup_pending_ = false;
    this->waiting_for_speaker_stop_ = false;
    this->idle_emit_pending_ = false;
    owned_failure = true;
  }
  portEXIT_CRITICAL(&this->followup_mux_);

  if (stale_callback) {
    ESP_LOGW(TAG, "stale follow-up READY callback ignored");
    return false;
  }
  this->cancel_timeout("va_followup_commit");
  if (owned_failure || !ready) {
    this->cancel_timeout("va_followup");
    this->cancel_timeout("va_followup_open");
    ESP_LOGW(TAG,
             "follow-up READY rejected (phase=%s); "
             "mic remains closed",
             phase_name_(phase_now));
    this->send_client_revoke_("ready_rejected", credentials.session_nonce,
                              credentials.wake_generation);
    return false;
  }

  if (!this->send_follow_up_ready_(credentials)) {
    this->revoke_followup_("ready_send_failed", false);
    return false;
  }

  ESP_LOGI(TAG, "follow-up READY sent with mic closed");
  this->set_timeout(
      "va_followup_commit", kRequestFollowUpCommitTimeoutMs,
      [this, credentials]() {
        std::lock_guard<GenerationEffectGate> effect_guard(
            this->generation_effect_gate_);
        bool matches = false;
        portENTER_CRITICAL(&this->followup_mux_);
        const FollowUpCredentials current = this->lifecycle_.credentials();
        matches = this->lifecycle_.follow_up_stage() == FollowUpStage::READY &&
                  current.token == credentials.token &&
                  current.session_nonce == credentials.session_nonce &&
                  current.wake_generation == credentials.wake_generation &&
                  current.ready_nonce == credentials.ready_nonce;
        if (matches) {
          this->lifecycle_.revoke();
          this->streaming_ = false;
        }
        portEXIT_CRITICAL(&this->followup_mux_);
        if (!matches)
          return;
        ESP_LOGW(TAG, "follow-up COMMIT timed out; mic remained closed");
        this->revoke_followup_("commit_timeout", true);
      });
  return true;
}

void VaClient::abort_followup_mic(uint32_t token, uint32_t session_nonce) {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  bool owns_callback_or_request = false;
  portENTER_CRITICAL(&this->followup_mux_);
  const FollowUpCredentials credentials = this->lifecycle_.credentials();
  owns_callback_or_request = this->lifecycle_.follow_up_stage() != FollowUpStage::NONE &&
                             credentials.token == token &&
                             credentials.session_nonce == session_nonce;
  portEXIT_CRITICAL(&this->followup_mux_);
  if (!owns_callback_or_request)
    return;
  ESP_LOGI(TAG, "follow-up callback aborted");
  this->revoke_followup_("follow_up_abort", true);
}

void VaClient::send_interrupt() {
  std::lock_guard<GenerationEffectGate> effect_guard(this->generation_effect_gate_);
  ControlContext context;
  portENTER_CRITICAL(&this->followup_mux_);
  context.session_nonce = this->lifecycle_.session_nonce();
  context.wake_generation = this->lifecycle_.wake_generation();
  this->lifecycle_.stop();
  this->streaming_ = false;
  portEXIT_CRITICAL(&this->followup_mux_);
  const bool barrier_clear = this->wait_for_mic_send_barrier_(
      "stop_mic_send_barrier_failed");
  // Flush our PSRAM playback queue — what's already been pushed into the
  // resampler/mixer/leaf will still drain (~600 ms residual), but everything
  // we have yet to hand off is dropped. The yaml side stops the resampler
  // explicitly. Reset deferred state too so we don't accidentally hold an
  // "idle" emit waiting for the (now-empty) queue. The ring reset has to
  // happen under the mux: the WS task could be mid-write and seeing
  // head=tail=fill=0 partway through would let it write into a "freshly
  // empty" buffer the user just barge-cancelled.
  this->close_audio_ring_();
  // Drop further incoming TTS until the backend confirms the turn boundary —
  // it keeps streaming the rest of the (already-generated) reply otherwise.
  this->suppress_incoming_audio_ = true;
  // Close the mic gate. An interrupt during the OPEN follow-up window would
  // otherwise leave streaming_ true while the va_followup close-timer gets
  // cancelled just below — mic open + streaming to OpenAI indefinitely, so any
  // later room speech becomes an unprompted turn. Callers that start a fresh
  // turn (start_session) re-open it themselves right after.
  this->clear_request_follow_up_(false);
  this->followup_pending_ = false;
  this->waiting_for_speaker_stop_ = false;
  this->idle_emit_pending_ = false;
  this->cancel_session_timers_();
  // The phase=idle the server is about to send shouldn't open a follow-up
  // mic window — the user said "stop", not "wait for me to keep talking".
  this->suppress_followup_ = true;
  this->graceful_close_prepared_token_ = 0;
  this->graceful_close_token_ = 0;
  // Mic gate is now closed: no new turn can begin until a wake. Ignore any
  // `thinking` the backend emits in the meantime — it's the server VAD's
  // end-of-turn for the utterance we just cancelled, not a real new turn.
  // Cleared in start_session() (the next wake). See set_phase_.
  this->post_stop_guard_ = true;
  if (barrier_clear)
    this->send_interrupt_control_("stop", context.session_nonce,
                                  context.wake_generation);
  ESP_LOGI(TAG, "send_interrupt — local state closed before bounded control send");
}

}  // namespace va_client
}  // namespace esphome
