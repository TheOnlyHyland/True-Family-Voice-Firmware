import hashlib
import os
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BuildContractTest(unittest.TestCase):
    def read(self, relative_path: str) -> str:
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_generic_factory_uses_checked_out_realtime_component(self) -> None:
        factory_config = self.read("home-assistant-voice.realtime.factory.yaml")
        realtime = self.read("home-assistant-voice.realtime.yaml")

        self.assertIn("realtime: !include home-assistant-voice.realtime.yaml", factory_config)
        self.assertIn("va_client_source: esphome/components", factory_config)
        self.assertIn("name: true-family-voice", factory_config)
        self.assertIn('firmware_version: "0.20.1"', factory_config)
        self.assertIn('va_url: "ws://homeassistant.local:8080/"', factory_config)
        self.assertNotIn("ota_password", factory_config)
        self.assertNotIn("\napi:\n", factory_config)
        self.assertNotIn("\napi:\n", realtime)
        self.assertNotIn("platform: esphome", realtime)
        self.assertNotIn("dashboard_import:", factory_config)
        self.assertNotIn("compile-only", factory_config)
        self.assertIn(
            'va_client_source: "github://TheOnlyHyland/True-Family-Voice-Firmware@0.20.1"',
            realtime,
        )
        self.assertIn("- source: ${va_client_source}", realtime)
        self.assertIn('version: "0.20.1"', realtime)

        compatibility_factory = self.read("home-assistant-voice.factory.yaml")
        self.assertEqual(compatibility_factory, factory_config)
        self.assertFalse((ROOT / "home-assistant-voice.8mb.yaml").exists())
        self.assertFalse((ROOT / "home-assistant-voice.yaml").exists())

    def test_private_overlay_requires_encrypted_api_and_private_ota(self) -> None:
        adopted = self.read("home-assistant-voice.adopted.yaml")
        dhcp = self.read("esphome-builder.dhcp.yaml")
        static_ip = self.read("esphome-builder.static-ip.yaml")

        self.assertIn("api:\n  encryption:\n    key: ${api_encryption_key}", adopted)
        self.assertIn("password: ${ota_password}", adopted)
        version = self.read("VERSION").strip()
        for filename, stub in (
            ("esphome-builder.dhcp.yaml", dhcp),
            ("esphome-builder.static-ip.yaml", static_ip),
        ):
            normalized_stub = " ".join(stub.split())
            self.assertIn("api_encryption_key: !secret api_encryption_key", stub)
            self.assertIn("ota_password: !secret ota_password", stub)
            self.assertIn("home-assistant-voice.adopted.yaml", stub)
            self.assertIn(f'ref: "{version}"', stub)
            self.assertIn(f"{filename}@{version}", stub)
            self.assertNotIn("ref: main", stub)
            self.assertNotIn("@main", stub)
            self.assertIn("do not auto-discover later releases", normalized_stub)
            self.assertIn("deliberately re-adopt", stub)
        example = self.read("secrets.yaml.example")
        self.assertIn("api_encryption_key:", example)
        self.assertNotIn("\napi_key:", example)

    def test_verify_compares_every_local_component_header(self) -> None:
        verify = self.read("scripts/verify")
        realtime = self.read("home-assistant-voice.realtime.yaml")
        self.assertIn("scripts/verify-external-inputs", verify)
        self.assertIn("scripts/deterministic-build-env", verify)
        self.assertIn("scripts/apply-esphome-source-date-epoch-patch", verify)
        self.assertIn("scripts/verify-resolved-inputs.py", verify)
        self.assertIn("scripts/verify-idf-component-lock.py", verify)
        self.assertIn("scripts/vendor-installer", verify)
        self.assertIn("scripts/run-actionlint", verify)
        self.assertIn("home-assistant-voice.realtime.factory.yaml", verify)
        self.assertIn("home-assistant-voice.factory.yaml", verify)
        self.assertIn(
            "cmp home-assistant-voice.realtime.factory.yaml "
            "home-assistant-voice.factory.yaml",
            verify,
        )
        self.assertIn("scripts/compare-yaml.py", verify)
        self.assertNotIn("compile-test.yaml", verify)
        self.assertIn('grep -q "compile-only"', verify)
        self.assertIn("generic factory unexpectedly exposes ESPHome native API", verify)
        self.assertIn("generic factory unexpectedly exposes ESPHome native OTA", verify)
        self.assertIn("_esphomelib._tcp", verify)
        self.assertIn("compile_process_limit: 1", realtime)
        self.assertIn("flock is required for the ESPHome compile lock", verify)
        self.assertIn("true-family-voice-esphome-compile.lock", verify)
        self.assertIn("refusing ESPHome compile while Chromium workers are active", verify)
        self.assertIn("less than 4.5 GiB memory available", verify)
        self.assertLess(verify.index("flock 9"), verify.index("esphome compile"))
        self.assertLess(verify.index("flock 9"), verify.index("esphome clean"))
        self.assertIn("ESPHOME_BUILD_TIME = $SOURCE_DATE_EPOCH", verify)
        self.assertEqual(verify.count("esphome compile"), 1)
        for filename in (
            "automation.h",
            "follow_up_lifecycle.h",
            "follow_up_safety.h",
            "va_client.cpp",
            "va_client.h",
        ):
            self.assertIn(filename, verify)

    def test_transport_calls_are_centralized_and_bounded(self) -> None:
        source = self.read("esphome/components/va_client/va_client.cpp")
        header = self.read("esphome/components/va_client/va_client.h")

        self.assertNotIn("portMAX_DELAY", source)
        self.assertEqual(source.count("esp_websocket_client_send_text("), 1)
        self.assertEqual(source.count("esp_websocket_client_send_bin("), 1)
        self.assertIn("kControlSendTimeoutMs", source)
        self.assertIn("kAudioSendTimeoutMs", source)
        self.assertIn("mic_send_fence_.acquire()", source)
        self.assertIn("wait_for_mic_send_barrier_(", source)
        self.assertIn("ws_reassembler_.push(", source)
        self.assertIn("data->payload_offset", source)
        self.assertIn("quarantine_transport_", source)
        self.assertIn("transport_admission_.can_start_wake()", source)
        self.assertIn("client_revoke_send_failed", source)
        self.assertIn("enroll_stopped_send_failed", source)
        self.assertIn("enroll_mic_send_barrier_failed", source)
        self.assertIn("interrupt_send_failed", source)
        self.assertIn("flush_send_failed", source)
        self.assertIn("bool send_client_revoke_", header)
        self.assertIn("bool send_interrupt_control_", header)
        self.assertIn(
            "bool wait_for_mic_send_barrier_(const char *failure_reason)", header
        )
        self.assertIn("on_authoritative_close_failure", source)
        self.assertIn("esp_websocket_client_stop", source)
        self.assertGreaterEqual(source.count("on_session_admitted()"), 2)
        self.assertIn("on_transport_connected()", source)
        self.assertIn("on_transport_disconnected()", source)
        self.assertIn("can_admit_session()", source)
        quarantine = source[source.index("void VaClient::quarantine_transport_") :]
        quarantine = quarantine[: quarantine.index("bool VaClient::revoke_followup_")]
        fault = quarantine.index("on_authoritative_close_failure()")
        local_disconnect = quarantine.index("lifecycle_.on_disconnected()")
        stop = quarantine.index("esp_websocket_client_stop(handle)")
        destroy = quarantine.index("esp_websocket_client_destroy(handle)")
        reconnect = quarantine.index("schedule_reconnect_()")
        self.assertLess(fault, local_disconnect)
        self.assertLess(local_disconnect, stop)
        self.assertLess(stop, destroy)
        self.assertLess(destroy, reconnect)
        self.assertIn("const bool first_fault", quarantine)
        self.assertIn("if (first_fault)", quarantine)
        self.assertIn("if (!first_fault)", quarantine)
        self.assertIn("transport_admission_.on_transport_disconnected()", quarantine)
        self.assertLess(
            quarantine.index('cancel_timeout("va_enroll_cap")'),
            quarantine.index("lifecycle_.on_disconnected()"),
        )
        self.assertLess(
            quarantine.index("enroll_mode_ = false"),
            quarantine.index("lifecycle_.on_disconnected()"),
        )

        barrier_helper = source[
            source.index("bool VaClient::wait_for_mic_send_barrier_(") :
        ]
        barrier_helper = barrier_helper[
            : barrier_helper.index("void VaClient::quarantine_transport_")
        ]
        self.assertIn("quarantine_transport_(failure_reason)", barrier_helper)
        self.assertLess(
            barrier_helper.index("quarantine_transport_(failure_reason)"),
            barrier_helper.index("return clear"),
        )

        enroll_stop = source[source.index("void VaClient::enroll_stop") :]
        enroll_stop = enroll_stop[: enroll_stop.index("void VaClient::send_button_cancel")]
        local_close = enroll_stop.index("lifecycle_.stop_enrollment()")
        barrier = enroll_stop.index(
            'wait_for_mic_send_barrier_(\n      "enroll_mic_send_barrier_failed")'
        )
        trusted_revoke = enroll_stop.index(
            'send_client_revoke_("enrollment_stopped"'
        )
        self.assertLess(local_close, barrier)
        self.assertLess(barrier, trusted_revoke)
        self.assertIn('quarantine_transport_("enroll_stopped_send_failed")', enroll_stop)
        self.assertNotIn(
            'quarantine_transport_("enroll_mic_send_barrier_failed")', enroll_stop
        )

    def test_every_barrier_closure_class_uses_the_quarantining_helper(self) -> None:
        source = self.read("esphome/components/va_client/va_client.cpp")
        reasons = {
            "thinking": "phase_close_mic_send_barrier_failed",
            "follow_up_timeout": "follow_up_timeout_mic_send_barrier_failed",
            "silent_timeout": "silent_wake_mic_send_barrier_failed",
            "session_timeout": "session_ceiling_mic_send_barrier_failed",
            "mute": "mute_mic_send_barrier_failed",
            "announcement": "announcement_mic_send_barrier_failed",
            "enrollment": "enroll_mic_send_barrier_failed",
            "follow_up_close": "follow_up_close_mic_send_barrier_failed",
            "revoke": "revoke_mic_send_barrier_failed",
            "stop": "stop_mic_send_barrier_failed",
        }
        self.assertNotIn("wait_for_mic_send_barrier_()", source)
        for closure_class, reason in reasons.items():
            with self.subTest(closure_class=closure_class):
                self.assertEqual(source.count(f'"{reason}"'), 1)

        helper = source[source.index("bool VaClient::wait_for_mic_send_barrier_(") :]
        helper = helper[: helper.index("void VaClient::quarantine_transport_")]
        self.assertIn("if (!clear)", helper)
        self.assertIn("quarantine_transport_(failure_reason)", helper)

        phase = source[source.index("void VaClient::apply_phase_side_effects_") :]
        phase = phase[: phase.index("uint32_t VaClient::prepare_local_wake()")]
        self.assertLess(
            phase.index("this->streaming_ = this->lifecycle_.mic_open()"),
            phase.index('"phase_close_mic_send_barrier_failed"'),
        )

        enrollment_calls = (
            "this->enroll_stop(false);",
            "this->enroll_stop(true);",
        )
        for call in enrollment_calls:
            self.assertIn(call, source)
        enrollment = source[source.index("void VaClient::enroll_stop") :]
        enrollment = enrollment[: enrollment.index("void VaClient::send_button_cancel")]
        self.assertLess(
            enrollment.index("lifecycle_.stop_enrollment()"),
            enrollment.index('"enroll_mic_send_barrier_failed"'),
        )
        self.assertIn('send_client_revoke_("enrollment_stopped"', enrollment)

    def test_yaml_has_bound_delays_and_both_speaker_lanes(self) -> None:
        yaml = self.read("home-assistant-voice.realtime.yaml")

        self.assertIn("id: guarded_wake_start", yaml)
        self.assertIn("mode: restart", yaml)
        self.assertIn("wake_reservation: int", yaml)
        self.assertIn("pending_wake_is_safe(wake_reservation)", yaml)
        self.assertIn("start_session(wake_reservation)", yaml)
        self.assertNotIn("start_session()", yaml)
        self.assertNotIn("followup_window_watchdog", yaml)
        self.assertNotIn("commit_followup_mic", yaml)
        self.assertIn("revoke_for_mute()", yaml)
        self.assertIn("release_mute()", yaml)
        self.assertIn("get_followup_open_delay_ms()", yaml)
        self.assertIn("announcement_speaker: announcement_resampling_speaker", yaml)
        self.assertIn("set_announcement_active(true)", yaml)
        self.assertIn('version: "5.5.5"', yaml)
        self.assertNotIn("version: recommended", yaml)
        self.assertNotIn("ref: dev", yaml)
        self.assertNotIn("raw/dev/sounds", yaml)
        self.assertNotIn("@main", yaml)

        enrollment_button = yaml[yaml.index("# Enrollment (fork)") :]
        enrollment_button = enrollment_button[: enrollment_button.index("# Double Click")]
        stop = enrollment_button.index('lambda: "id(va)->enroll_stop(true);"')
        alternate = enrollment_button.index("else:", stop)
        ordinary_press = enrollment_button.index("switch.is_on: timer_ringing")
        self.assertLess(stop, alternate)
        self.assertLess(alternate, ordinary_press)

    def test_graceful_commit_locally_enters_existing_idle_drain(self) -> None:
        source = self.read("esphome/components/va_client/va_client.cpp")
        safety = self.read("esphome/components/va_client/follow_up_safety.h")

        control_context = source[
            source.index(
                "GracefulControlContext VaClient::graceful_control_context_"
            ) :
        ]
        control_context = control_context[
            : control_context.index("bool VaClient::clear_graceful_close_owner_")
        ]
        self.assertIn(
            '{"type", "token", "session_nonce", "wake_generation"}',
            control_context,
        )
        for field in (
            'message.get_uint("token", context.token)',
            'message.get_uint("session_nonce", context.session_nonce)',
            'message.get_uint("wake_generation", context.wake_generation)',
            "context.prepared_token =",
            "context.committed_token =",
            "context.owner_session_nonce =",
            "context.owner_wake_generation =",
            "context.active_session_nonce = this->lifecycle_.session_nonce()",
            "context.active_wake_generation = this->lifecycle_.wake_generation()",
            "context.active_wake = this->lifecycle_.active_wake()",
        ):
            self.assertIn(field, control_context)
        self.assertIn("matches_active_wake", safety)
        self.assertIn("context.session_nonce == context.owner_session_nonce", safety)
        self.assertIn(
            "context.wake_generation == context.owner_wake_generation", safety
        )

        prepare = source[source.index('if (type == "prepare_suppress_followup")') :]
        prepare = prepare[: prepare.index('if (type == "commit_suppress_followup")')]
        self.assertIn("GracefulControlStage::PREPARE", prepare)
        self.assertIn("graceful_control_context_", prepare)
        prepare_accept = prepare.index(
            "if (action == GracefulControlAction::ACCEPT)"
        )
        prepare_settle = prepare.index(
            "} else if (action == GracefulControlAction::SETTLE_CURRENT)",
            prepare_accept,
        )
        prepare_ignore = prepare.index("} else {", prepare_settle)
        accepted_prepare = prepare[prepare_accept:prepare_settle]
        settled_prepare = prepare[prepare_settle:prepare_ignore]
        ignored_prepare = prepare[prepare_ignore:]
        self.assertIn(
            'revoke_followup_("graceful_prepare", false)', accepted_prepare
        )
        revoke_success = accepted_prepare.index(
            'if (this->revoke_followup_("graceful_prepare", false))'
        )
        owner_session = accepted_prepare.index(
            "this->graceful_close_owner_session_nonce_ = control.session_nonce"
        )
        owner_wake = accepted_prepare.index(
            "this->graceful_close_owner_wake_generation_ = control.wake_generation"
        )
        prepared_token = accepted_prepare.index(
            "this->graceful_close_prepared_token_ = control.token"
        )
        self.assertLess(owner_session, owner_wake)
        self.assertLess(owner_wake, prepared_token)
        self.assertLess(revoke_success, owner_session)
        self.assertIn("accepted = false", accepted_prepare)
        self.assertIn(
            'revoke_followup_("graceful_prepare_rejected", true)',
            settled_prepare,
        )
        self.assertNotIn("revoke_followup_", ignored_prepare)
        self.assertNotIn("set_phase_", ignored_prepare)
        self.assertNotIn("fire_phase_led_", ignored_prepare)
        self.assertNotIn("lifecycle_", ignored_prepare)
        self.assertNotIn("streaming_", ignored_prepare)

        commit = source[source.index('if (type == "commit_suppress_followup")') :]
        commit = commit[: commit.index('if (type == "cancel_suppress_followup")')]
        self.assertIn("GracefulControlStage::COMMIT", commit)
        self.assertIn("graceful_control_context_", commit)
        commit_accept = commit.index(
            "if (action == GracefulControlAction::ACCEPT)"
        )
        commit_settle = commit.index(
            "} else if (action == GracefulControlAction::SETTLE_CURRENT)",
            commit_accept,
        )
        commit_ignore = commit.index("} else {", commit_settle)
        accepted = commit[commit_accept:commit_settle]
        settled = commit[commit_settle:commit_ignore]
        ignored = commit[commit_ignore:]
        token_commit = accepted.index("this->graceful_close_token_ = control.token")
        local_idle = accepted.index('this->set_phase_("idle")')
        commit_ack = commit.index('send_graceful_close_ack_("committed"')
        self.assertLess(token_commit, local_idle)
        self.assertLess(local_idle, commit_ack)
        self.assertEqual(commit.count('set_phase_("idle")'), 1)
        self.assertNotIn('set_phase_("idle")', settled)
        self.assertNotIn('set_phase_("idle")', ignored)
        self.assertIn(
            'revoke_followup_("graceful_commit_rejected", true)', settled
        )
        self.assertNotIn("revoke_followup_", ignored)
        self.assertNotIn("fire_phase_led_", ignored)
        self.assertNotIn("lifecycle_", ignored)
        self.assertNotIn("streaming_", ignored)
        self.assertNotIn('fire_phase_led_("idle")', commit)

        graceful_idle = source[
            source.index(
                "} else if (this->graceful_close_token_.load() != 0) {"
            ) :
        ]
        graceful_idle = graceful_idle[
            : graceful_idle.index(
                "} else if (this->request_follow_up_token_.load() != 0"
            )
        ]
        for state_change in (
            "this->streaming_ = false",
            "this->followup_pending_ = true",
            "this->waiting_for_speaker_stop_ = false",
            "this->request_follow_up_pending_ = false",
            "this->followup_armed_ = false",
            "this->idle_emit_pending_ = true",
            "return;",
        ):
            self.assertIn(state_change, graceful_idle)

        loop = source[source.index("void VaClient::loop()") :]
        loop = loop[: loop.index("void VaClient::connect_()")]
        ring_empty = loop.index(
            "if (this->followup_pending_ && this->audio_fill_snapshot_() == 0"
        )
        queued_audio_drain = loop.index("this->audio_fill_ -= accepted")
        drain_gate = loop.index("GenerationEffectGate> drain_effect_guard")
        speaker_drain = loop.index("const bool speaker_drained =", ring_empty)
        graceful_tail = loop.index(
            'this->set_timeout("va_graceful_close"', speaker_drain
        )
        idle_emit = loop.index("this->open_followup_window_(0);", graceful_tail)
        self.assertLess(queued_audio_drain, ring_empty)
        self.assertLess(drain_gate, ring_empty)
        self.assertLess(ring_empty, speaker_drain)
        self.assertLess(speaker_drain, graceful_tail)
        self.assertLess(graceful_tail, idle_emit)
        self.assertIn("graceful_close_owner_session_nonce_", loop)
        self.assertIn("graceful_close_owner_wake_generation_", loop)
        self.assertIn("clear_graceful_close_owner_()", loop)

        owner_clear = source[
            source.index("bool VaClient::clear_graceful_close_owner_()") :
        ]
        owner_clear = owner_clear[
            : owner_clear.index("bool VaClient::settle_graceful_close_()")
        ]
        for state_change in (
            "this->graceful_close_prepared_token_.exchange(0)",
            "this->graceful_close_token_.exchange(0)",
            "this->graceful_close_owner_session_nonce_.exchange(0)",
            "this->graceful_close_owner_wake_generation_.exchange(0)",
        ):
            self.assertIn(state_change, owner_clear)

        settlement = source[source.index("bool VaClient::settle_graceful_close_()") :]
        settlement = settlement[
            : settlement.index("void VaClient::clear_request_follow_up_")
        ]
        for state_change in (
            "this->clear_graceful_close_owner_()",
            'this->cancel_timeout("va_graceful_close")',
            "this->request_follow_up_token_ = 0",
            "this->request_follow_up_pending_ = false",
            "this->request_follow_up_callback_in_flight_ = false",
            "this->followup_pending_ = false",
            "this->waiting_for_speaker_stop_ = false",
            "this->followup_armed_ = false",
            "this->idle_emit_pending_ = false",
            "this->streaming_ = false",
            "this->lifecycle_.on_phase_idle()",
            "this->current_phase_.store(static_cast<uint8_t>(Phase::IDLE))",
            'this->fire_phase_led_("idle")',
        ):
            self.assertIn(state_change, settlement)
        self.assertNotIn("open_followup_window_", settlement)
        no_owner = settlement.index("if (!this->clear_graceful_close_owner_())")
        no_owner_return = settlement.index("return false;", no_owner)
        cancel_tail = settlement.index('this->cancel_timeout("va_graceful_close")')
        self.assertLess(no_owner_return, cancel_tail)

        clear = source[source.index("void VaClient::clear_request_follow_up_") :]
        clear = clear[: clear.index("void VaClient::cancel_session_timers_")]
        self.assertIn("settle_graceful_close_()", clear)

        revoke = source[source.index("bool VaClient::revoke_followup_") :]
        revoke = revoke[: revoke.index("void VaClient::revoke_followup()")]
        self.assertIn("clear_request_follow_up_(false)", revoke)

        cancel = source[source.index('if (type == "cancel_suppress_followup")') :]
        cancel = cancel[: cancel.index('if (type == "ack")')]
        self.assertIn("GracefulControlStage::CANCEL", cancel)
        cancel_settle = cancel.index(
            "if (action == GracefulControlAction::SETTLE_CURRENT)"
        )
        cancel_ignore = cancel.index("} else {", cancel_settle)
        settled_cancel = cancel[cancel_settle:cancel_ignore]
        ignored_cancel = cancel[cancel_ignore:]
        self.assertIn(
            'revoke_followup_("graceful_cancel", false)', settled_cancel
        )
        self.assertNotIn("revoke_followup_", ignored_cancel)
        self.assertNotIn("set_phase_", ignored_cancel)
        self.assertNotIn("fire_phase_led_", ignored_cancel)
        self.assertNotIn("lifecycle_", ignored_cancel)
        self.assertNotIn("streaming_", ignored_cancel)

        ack = source[source.index("void VaClient::send_graceful_close_ack_") :]
        ack = ack[: ack.index("void VaClient::fire_phase_led_")]
        ack_fields = (
            "context.token",
            "context.session_nonce",
            "context.wake_generation",
            "ack += accepted ?",
        )
        ack_positions = [ack.index(field) for field in ack_fields]
        self.assertEqual(ack_positions, sorted(ack_positions))
        self.assertNotIn("control_context_()", ack)
        self.assertNotIn("legacy_zero_mode_()", ack)

        mute = source[source.index("void VaClient::revoke_for_mute()") :]
        mute = mute[: mute.index("void VaClient::release_mute()")]
        self.assertIn("clear_request_follow_up_(false)", mute)

        announcement = source[
            source.index("void VaClient::set_announcement_active(bool active)") :
        ]
        announcement = announcement[
            : announcement.index("void VaClient::send_graceful_close_ack_")
        ]
        self.assertIn("graceful_close_prepared_token_.load() != 0", announcement)
        self.assertIn("graceful_close_token_.load() != 0", announcement)
        self.assertIn("graceful_close_owner_session_nonce_.load() != 0", announcement)
        self.assertIn(
            "graceful_close_owner_wake_generation_.load() != 0", announcement
        )
        self.assertIn("clear_request_follow_up_(false)", announcement)

        stop = source[source.index("void VaClient::send_interrupt()") :]
        self.assertIn("clear_request_follow_up_(false)", stop)
        self.assertNotIn("graceful_close_prepared_token_ = 0", stop)
        self.assertNotIn("graceful_close_token_ = 0", stop)
        self.assertNotIn("graceful_close_owner_session_nonce_ = 0", stop)
        self.assertNotIn("graceful_close_owner_wake_generation_ = 0", stop)

        next_wake = source[source.index("uint32_t VaClient::prepare_local_wake()") :]
        next_wake = next_wake[: next_wake.index("bool VaClient::pending_wake_is_safe")]
        revoke_old = next_wake.index('revoke_followup_("new_wake"')
        prepare_new = next_wake.index("lifecycle_.prepare_local_wake()")
        self.assertLess(revoke_old, prepare_new)

    def test_trusted_phase_and_timer_contract_is_structural(self) -> None:
        source = self.read("esphome/components/va_client/va_client.cpp")
        safety = self.read("esphome/components/va_client/follow_up_safety.h")
        lifecycle = self.read("esphome/components/va_client/follow_up_lifecycle.h")
        readme = self.read("README.md")
        changelog = self.read("CHANGELOG.md")
        install = self.read("INSTALL.md")

        self.assertIn(
            '{"type", "value", "session_nonce", "wake_generation"}', source
        )
        self.assertIn("trusted_follow_up_shape", source)
        self.assertIn('"wake_generation", "token"', source)
        self.assertIn(
            '{"type":"phase","value":"listening","session_nonce":S,'
            '"wake_generation":G,"token":T}',
            readme,
        )
        self.assertRegex(
            readme,
            r"backend `0\.22\.0` is required for\s+any explicit follow-up",
        )
        self.assertIn(
            'backend `0.22.5` binds each model-selected graceful close', readme
        )
        self.assertIn(
            '{"type":"prepare_suppress_followup","token":C,'
            '"session_nonce":S,"wake_generation":G}',
            readme,
        )
        self.assertIn(
            '{"type":"suppress_followup_ack","stage":"prepared",'
            '"token":C,"session_nonce":S,"wake_generation":G,'
            '"accepted":true}',
            readme,
        )
        self.assertIn("any tokenized `idle`", readme)
        self.assertIn("HIL exercise of delayed timer", readme)
        self.assertIn("all explicit\nfollow-up OPEN answers fail closed", changelog)
        self.assertIn(
            "compatible only for ordinary physical-wake turns", install
        )
        backend_restore = install.index(
            "restore backend `0.20.6` first"
        )
        firmware_restart = install.index(
            "Restart the still-installed firmware", backend_restore
        )
        legacy_verify = install.index(
            "Verify that the restarted firmware has reconnected in exact legacy",
            firmware_restart,
        )
        firmware_downgrade = install.index(
            "perform the firmware rollback", legacy_verify
        )
        self.assertLess(backend_restore, firmware_restart)
        self.assertLess(firmware_restart, legacy_verify)
        self.assertLess(legacy_verify, firmware_downgrade)
        self.assertIn("trusted session nonce blocks", install)
        self.assertIn(
            "restart the\nstill-installed firmware to clear its trusted session nonce",
            changelog,
        )
        phase_handler = source[source.index('if (type == "phase")') :]
        text_handler = source[source.index("void VaClient::handle_text_") :]
        effect_lock = text_handler.index("generation_effect_gate_")
        phase_dispatch = text_handler.index('if (type == "phase")')
        self.assertLess(effect_lock, phase_dispatch)
        lock = phase_handler.index("portENTER_CRITICAL(&this->followup_mux_)")
        apply = phase_handler.index("apply_trusted_phase(")
        unlock = phase_handler.index("portEXIT_CRITICAL(&this->followup_mux_)")
        effects = phase_handler.index("apply_phase_side_effects_(")
        self.assertLess(lock, apply)
        self.assertLess(apply, unlock)
        self.assertLess(unlock, effects)
        effect_handler = source[source.index("void VaClient::apply_phase_side_effects_") :]
        effect_handler = effect_handler[
            : effect_handler.index("uint32_t VaClient::prepare_local_wake()")
        ]
        current_check = effect_handler.index("phase_effect_plan_current_(transition)")
        runtime_update = effect_handler.index("current_phase_.store(")
        streaming_update = effect_handler.index("this->streaming_ =")
        self.assertLess(current_check, runtime_update)
        self.assertLess(current_check, streaming_update)
        for side_effect in (
            "wait_for_mic_send_barrier_(",
            "clear_request_follow_up_(false)",
            'cancel_timeout("va_session_ceiling")',
            'cancel_timeout("va_graceful_close")',
            'cancel_timeout("va_no_speech")',
            'cancel_timeout("va_followup")',
            'cancel_timeout("va_followup_open")',
            'cancel_timeout("va_tts_tail")',
            "close_audio_ring_()",
            "open_followup_window_",
            "this->defer(",
        ):
            self.assertLess(current_check, effect_handler.index(side_effect))
        self.assertIn(
            "[this, phase_copy, runtime_phase, effect_plan]", effect_handler
        )
        self.assertIn("phase_effect_plan_current_(effect_plan)", effect_handler)
        self.assertNotIn("[this, phase_copy, effect_epoch]", effect_handler)
        self.assertIn("phase_runtime_action(transition.status)", phase_handler)
        self.assertIn("trusted_follow_up_shape, follow_up_token", phase_handler)
        self.assertIn("decide_follow_up_phase_credential(", lifecycle)
        self.assertIn("follow_up_input_ended", source)
        self.assertIn("phase_effect_plan_is_current", source)
        self.assertIn("transition.connection_generation", source)
        self.assertIn("transition.wake_generation", source)
        self.assertIn("GenerationEffectGate", safety)
        self.assertIn('"va_session_ceiling"', source)
        self.assertIn("kAbsoluteSessionMaxMs", source)
        self.assertIn("kRequestFollowUpReadyTimeoutMs = 8000", safety)
        self.assertIn("kAbsoluteSessionMaxMs = 120000", safety)
        self.assertIn("per_answer_grant_available", safety)
        self.assertIn("per_answer_grant_available_", lifecycle)
        self.assertNotIn("one_shot", safety + lifecycle + source)
        self.assertIn("kFollowUpReplayHistorySize = 256", safety)
        self.assertIn("follow_up_token_history_count_", lifecycle)
        self.assertIn("ready_nonce_history_count_", lifecycle)
        self.assertIn(
            "std::array<uint32_t, kFollowUpReplayHistorySize>", lifecycle
        )
        self.assertNotIn("std::vector<uint32_t>", lifecycle)
        self.assertIn("follow_up_deadline_ms_", lifecycle)
        self.assertIn("follow_up_deadline_reached", lifecycle)
        self.assertIn("expire_follow_up_deadline", lifecycle)

        # These narrow source checks cover only integration that cannot execute
        # in the dependency-free host target: ESP-IDF portMUX placement and
        # ESPHome timer installation. The decisions inside those boundaries are
        # dynamically tested through the same helpers VaClient calls.
        commit = source[source.index('if (type == "commit_follow_up")') :]
        commit = commit[: commit.index('if (type == "prepare_suppress_followup")')]
        commit_decision = commit.index("decide_follow_up_runtime(")
        commit_lock = commit.rfind(
            "portENTER_CRITICAL(&this->followup_mux_)", 0, commit_decision
        )
        commit_unlock = commit.index(
            "portEXIT_CRITICAL(&this->followup_mux_)", commit_decision
        )
        timer_install = commit.index('this->set_timeout(', commit_decision)
        self.assertGreaterEqual(commit_lock, 0)
        self.assertLess(commit_lock, commit_decision)
        self.assertLess(commit_decision, commit_unlock)
        self.assertLess(commit_unlock, timer_install)

        expiry = source[source.index("bool VaClient::expire_follow_up_deadline_") :]
        expiry = expiry[: expiry.index("bool VaClient::wait_for_mic_send_barrier_")]
        expiry_lock = expiry.index("portENTER_CRITICAL(&this->followup_mux_)")
        expiry_decision = expiry.index("decide_follow_up_runtime(")
        expiry_mutation = expiry.index("expire_follow_up_deadline(")
        expiry_unlock = expiry.index("portEXIT_CRITICAL(&this->followup_mux_)")
        expiry_barrier = expiry.index("wait_for_mic_send_barrier_(")
        self.assertLess(expiry_lock, expiry_decision)
        self.assertLess(expiry_decision, expiry_mutation)
        self.assertLess(expiry_mutation, expiry_unlock)
        self.assertLess(expiry_unlock, expiry_barrier)
        self.assertIn("send_mic_flush_", expiry)
        self.assertIn("send_interrupt_control_", expiry)

        loop = source[source.index("void VaClient::loop()") :]
        loop = loop[: loop.index("void VaClient::connect_()")]
        self.assertIn("expire_follow_up_deadline_(millis(), 0, 0)", loop)

        mic = source[source.index("void VaClient::on_mic_data_") :]
        mic = mic[: mic.index("VaClient::Phase VaClient::phase_from_string_")]
        first_decision = mic.index("decide_follow_up_runtime(")
        first_lock = mic.rfind(
            "portENTER_CRITICAL(&this->followup_mux_)", 0, first_decision
        )
        lease = mic.index("mic_send_fence_.acquire()")
        first_unlock = mic.index(
            "portEXIT_CRITICAL(&this->followup_mux_)", first_decision
        )
        second_decision = mic.index("decide_follow_up_runtime(", lease)
        second_lock = mic.rfind(
            "portENTER_CRITICAL(&this->followup_mux_)", lease, second_decision
        )
        second_unlock = mic.index(
            "portEXIT_CRITICAL(&this->followup_mux_)", second_decision
        )
        send = mic.index("send_binary_bounded_(")
        self.assertGreaterEqual(first_lock, 0)
        self.assertLess(first_lock, first_decision)
        self.assertLess(first_decision, lease)
        self.assertLess(lease, first_unlock)
        self.assertGreaterEqual(second_lock, 0)
        self.assertLess(second_lock, second_decision)
        self.assertLess(second_decision, second_unlock)
        self.assertLess(second_unlock, send)

        prepare = source[source.index('if (type == "request_follow_up")') :]
        prepare = prepare[: prepare.index('if (type == "cancel_request_follow_up")')]
        prepare_lock = prepare.index("portENTER_CRITICAL(&this->followup_mux_)")
        prepare_mutation = prepare.index("prepare_follow_up(")
        prepare_unlock = prepare.index("portEXIT_CRITICAL(&this->followup_mux_)")
        prepare_send = prepare.index("send_request_follow_up_ack_(")
        self.assertLess(prepare_lock, prepare_mutation)
        self.assertLess(prepare_mutation, prepare_unlock)
        self.assertLess(prepare_unlock, prepare_send)

        ready = source[source.index("bool VaClient::mark_followup_ready") :]
        ready = ready[: ready.index("void VaClient::abort_followup_mic")]
        ready_mutation = ready.index("mark_follow_up_ready(")
        ready_lock = ready.rfind(
            "portENTER_CRITICAL(&this->followup_mux_)", 0, ready_mutation
        )
        ready_unlock = ready.index(
            "portEXIT_CRITICAL(&this->followup_mux_)", ready_mutation
        )
        ready_send = ready.index("send_follow_up_ready_(")
        self.assertGreaterEqual(ready_lock, 0)
        self.assertLess(ready_lock, ready_mutation)
        self.assertLess(ready_mutation, ready_unlock)
        self.assertLess(ready_unlock, ready_send)

    def test_logs_do_not_emit_protocol_credentials(self) -> None:
        source = self.read("esphome/components/va_client/va_client.cpp")
        for forbidden in ("token=%", "nonce=%", "wake=%", "generation=%"):
            self.assertNotIn(forbidden, source)

    def test_external_audio_inputs_are_immutable_and_locked(self) -> None:
        yaml = self.read("home-assistant-voice.realtime.yaml")
        locked = self.read("external-inputs.lock")
        voice_ref = "0579e7b9d8504264719c593474c85447253c9dc1"

        self.assertIn(f"ref: {voice_ref}", yaml)
        self.assertIn('wake_word_model: "models/hey_leonard.json"', yaml)
        self.assertIn("timer_finished_sound_file: sounds/gentle_timer.flac", yaml)
        self.assertIn(
            "micro-wake-word-models/05b65922cc433c9df13e98e32a7fe520758c837e/",
            yaml,
        )
        self.assertIn("ffva_v1.3.1_upgrade.bin", locked)
        self.assertIn("hey_jarvis.tflite", locked)
        self.assertIn("okay_nabu.tflite", locked)
        self.assertIn("stop.tflite", locked)
        self.assertIn("vad.tflite", locked)
        self.assertIn(
            "4b59e2ab922f749f3158116e14f8461c96cff9aab22985c155c0d50bbc4c2a0d "
            "https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-10.0.1.tgz",
            locked,
        )
        self.assertIn("actionlint_1.7.12_linux_amd64.tar.gz", locked)
        self.assertIn("actionlint_1.7.12_linux_arm64.tar.gz", locked)
        self.assertIn("Improv-1.2.4.tar.gz", locked)
        self.assertIn("patch-2.7.6.tar.xz", locked)
        self.assertEqual(locked.count("/sounds/"), 15)

    def test_idf_component_manager_closure_is_exactly_locked(self) -> None:
        locked = self.read("idf-component-manager.lock")
        verifier = self.read("scripts/verify-idf-component-lock.py")

        self.assertIn("schema=1", locked)
        self.assertIn("target=esp32s3", locked)
        self.assertIn("idf=5.5.5", locked)
        self.assertIn("improv_version=1.2.4", locked)
        self.assertIn("improv_file_count=12", locked)
        self.assertRegex(locked, r"improv_tree_sha256=[0-9a-f]{64}")
        self.assertRegex(locked, r"normalized_sha256=[0-9a-f]{64}")
        self.assertIn("dependencies.lock", verifier)
        self.assertIn("path_count != 1", verifier)
        self.assertIn("manifest_count != 1", verifier)
        self.assertIn("resolved Improv component tree differs", verifier)
        self.assertIn("ESP-IDF component-manager closure differs", verifier)

    def test_release_builds_only_the_exact_realtime_overlay(self) -> None:
        workflow = self.read(".github/workflows/build.yml")
        prepare = self.read(".github/workflows/prepare-release.yml")
        package = self.read("scripts/package-release")
        build_requirements = self.read("requirements-build.txt")
        requirements = self.read("requirements-esphome.txt")
        deterministic_env = self.read("scripts/deterministic-build-env")
        patch = self.read("scripts/esphome-2026.7.3-source-date-epoch.patch")
        patcher = self.read("scripts/apply-esphome-source-date-epoch-patch")

        self.assertEqual(workflow.count("./scripts/verify --compile"), 1)
        self.assertEqual(prepare.count("./scripts/verify --compile"), 1)
        for candidate in (workflow, prepare):
            self.assertIn('python-version: "3.13.5"', candidate)
            self.assertNotIn("ubuntu-latest", candidate)
            self.assertIn("runs-on: ubuntu-24.04", candidate)
            self.assertIn("scripts/install-gnu-patch", candidate)
            self.assertNotIn("apt-get install --yes patch", candidate)
            self.assertIn("--require-hashes", candidate)
            self.assertIn("--no-build-isolation", candidate)
            self.assertIn("requirements-build.txt", candidate)
            self.assertIn("requirements-esphome.txt", candidate)
            self.assertIn('SOURCE_DATE_EPOCH: "1785888000"', candidate)
            self.assertIn('TZ: "UTC0"', candidate)
            self.assertIn('LANG: "C"', candidate)
            self.assertIn('LC_ALL: "C"', candidate)
            self.assertIn('PYTHONHASHSEED: "0"', candidate)
            self.assertIn("apply-esphome-source-date-epoch-patch", candidate)
        self.assertIn("pip==26.2.1 \\", build_requirements)
        self.assertIn("setuptools==83.0.0 \\", build_requirements)
        self.assertIn("wheel==0.47.0 \\", build_requirements)
        self.assertIn("packaging==26.2 \\", build_requirements)
        self.assertGreater(build_requirements.count("--hash=sha256:"), 7)
        self.assertIn("esphome==2026.7.3 \\", requirements)
        self.assertGreater(requirements.count("--hash=sha256:"), 100)
        self.assertIn("scripts/package-release", workflow)
        self.assertIn("scripts/package-release", prepare)
        self.assertIn("actions/upload-artifact@", workflow)
        self.assertIn("actions/upload-artifact@", prepare)
        self.assertNotIn("compile-test.yaml", workflow)
        self.assertNotIn("compile-test.yaml", prepare)
        self.assertNotIn("workflows/build.yml@", workflow)
        self.assertNotIn("esphome compile", package)
        self.assertIn(".esphome/build/true-family-voice/build", package)
        self.assertNotIn("home-assistant-voice.factory.yaml", workflow)
        self.assertNotIn("home-assistant-voice.8mb.yaml", workflow)
        self.assertIn("verify-release-package.py", package)
        self.assertIn("scripts/deterministic-build-env", package)
        self.assertIn("compiled build timestamp does not match", package)
        self.assertNotIn("release:", workflow)
        self.assertNotIn("workflow_dispatch:", workflow)
        self.assertIn("workflow_dispatch:", prepare)
        self.assertIn("environment:\n      name: firmware-release", prepare)
        self.assertNotIn("CLOUDFLARE_R2", prepare)

        patch_installer = self.read("scripts/install-gnu-patch")
        self.assertIn("patch-2.7.6.tar.xz", patch_installer)
        self.assertIn("sha256sum -c -", patch_installer)
        self.assertIn('grep -m1 -Fxq "GNU patch 2.7.6"', patch_installer)

        self.assertEqual(self.read("SOURCE_DATE_EPOCH"), "1785888000\n")
        self.assertIn('os.environ["SOURCE_DATE_EPOCH"]', patch)
        self.assertIn("-    build_time = int(time.time())", patch)
        self.assertEqual(
            hashlib.sha256(patch.encode()).hexdigest(),
            "d1e118a2ba2c8eeb261d6b643ccd7820a9395e771c9bc53433192442255fb741",
        )
        self.assertIn(
            "31c9f9b479c7fd0b4d42ff484f58245e9c313f998fc8ec6db177518ce52e1d43",
            patcher,
        )
        self.assertIn(
            "1c2e07a2e634914762bacbbb055c9784ecea5e153b0eedc408b20eb371236b8e",
            patcher,
        )
        self.assertIn("writer.py differs from the pinned patch target", patcher)
        self.assertIn("GNU patch 2.7.6", patcher)
        self.assertIn("SOURCE_DATE_EPOCH differs", deterministic_env)
        self.assertIn("SOURCE_DATE_EPOCH=$EXPECTED_SOURCE_DATE_EPOCH", deterministic_env)

        clean_env = os.environ.copy()
        clean_env.pop("SOURCE_DATE_EPOCH", None)
        clean_env["ROOT"] = str(ROOT)
        exported = subprocess.run(
            [
                "sh",
                "-c",
                '. "$ROOT/scripts/deterministic-build-env"; '
                'printf "%s|%s|%s|%s|%s\\n" "$SOURCE_DATE_EPOCH" '
                '"$TZ" "$LANG" "$LC_ALL" "$PYTHONHASHSEED"',
            ],
            cwd=ROOT,
            env=clean_env,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(exported.returncode, 0, exported.stderr)
        self.assertEqual(exported.stdout, "1785888000|UTC0|C|C|0\n")
        clean_env["SOURCE_DATE_EPOCH"] = "1"
        mismatch = subprocess.run(
            ["sh", "-c", '. "$ROOT/scripts/deterministic-build-env"'],
            cwd=ROOT,
            env=clean_env,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn("differs", mismatch.stderr)

    def test_release_version_and_promotion_gates(self) -> None:
        version = self.read("VERSION").strip()
        realtime = self.read("home-assistant-voice.realtime.yaml")
        build = self.read(".github/workflows/build.yml")
        prepare = self.read(".github/workflows/prepare-release.yml")
        publish = self.read(".github/workflows/publish-release.yml")
        promotion = self.read(".github/workflows/update-latest.yml")
        pages = self.read(".github/workflows/gh-pages.yml")
        package = self.read("scripts/package-release")

        self.assertEqual(version, "0.20.1")
        self.assertIn(f'version: "{version}"', realtime)
        self.assertIn("verify-version", build)
        self.assertNotIn("workflow_dispatch:", build)
        self.assertNotIn("release:", build)
        self.assertIn("EXPECTED_VERSION", package)
        self.assertEqual(package.count('cmp "$BUILD/'), 3)

        self.assertIn("workflow_dispatch:", prepare)
        self.assertIn("source_commit:", prepare)
        self.assertIn("version:", prepare)
        self.assertIn("prerelease:", prepare)
        self.assertIn("confirm_prepare:", prepare)
        self.assertIn("make-release-intent.py", prepare)
        self.assertIn("verify-release-intent.py", prepare)
        self.assertIn("artifact-digest", prepare)
        self.assertIn("artifact-id", prepare)

        self.assertIn("workflow_dispatch:", publish)
        self.assertIn("preparation_run_id:", publish)
        self.assertIn("artifact_id:", publish)
        self.assertIn("artifact_digest:", publish)
        self.assertIn("confirm_publish:", publish)
        self.assertIn("artifact-ids:", publish)
        self.assertIn("run-id:", publish)
        self.assertIn("merge-multiple: true", publish)
        self.assertIn("verify-release-intent.py", publish)
        self.assertNotIn("esphome", publish.lower())
        self.assertNotIn("package-release", publish)
        self.assertNotIn("pip install", publish)
        self.assertLess(
            publish.index("--draft"),
            publish.index("Upload draft assets without clobber"),
        )
        self.assertGreater(
            publish.index('gh release edit "$VERSION" --draft=false'),
            publish.index("Upload draft assets without clobber"),
        )
        self.assertNotIn("uses: ./.github/workflows/gh-pages.yml", publish)

        self.assertIn("workflow_call:", pages)
        self.assertNotIn("workflow_dispatch:", pages)
        self.assertNotIn("release:", pages)
        self.assertNotIn("push:", pages)
        self.assertIn("successfully published version", pages)

        self.assertIn("workflow_dispatch:", promotion)
        self.assertIn("confirm_promotion:", promotion)
        self.assertIn("firmware-${{ inputs.channel }}", promotion)
        self.assertIn("name: firmware-release", promotion)
        self.assertNotIn("release:", promotion)
        self.assertNotIn("esphome", promotion.lower())
        self.assertNotIn("scripts/verify --compile", promotion)
        self.assertNotIn("pip install", promotion)
        self.assertNotIn("download-artifact", promotion)
        self.assertIn("gh release download", promotion)
        self.assertIn("verify-release-package.py", promotion)
        self.assertIn("make-channel-manifest", promotion)

        exact = subprocess.run(
            ["sh", str(ROOT / "scripts/verify-version"), "0.20.1"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(exact.returncode, 0, exact.stderr)
        self.assertEqual(exact.stdout.strip(), "0.20.1")
        wrong = subprocess.run(
            ["sh", str(ROOT / "scripts/verify-version"), "v0.20.1"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(wrong.returncode, 0)

    def test_publication_is_no_clobber_and_promotion_is_byte_bound(self) -> None:
        publish = self.read(".github/workflows/publish-release.yml")
        promotion = self.read(".github/workflows/update-latest.yml")
        non_secret_workflows = (
            self.read(".github/workflows/build.yml"),
            self.read(".github/workflows/prepare-release.yml"),
            self.read(".github/workflows/gh-pages.yml"),
            self.read(".github/workflows/yaml-lint.yml"),
        )

        self.assertIn("Refuse tags and non-resumable releases", publish)
        self.assertIn("resume_draft", publish)
        self.assertIn("targetCommitish", publish)
        self.assertIn("github-readback", publish)
        self.assertNotIn("--clobber", publish)
        self.assertIn('cmp "$package/$file" "github-readback/$file"', publish)
        self.assertNotIn("CLOUDFLARE_R2", publish)
        self.assertNotIn("R2_BUCKET", publish)
        self.assertNotIn("aws s3", publish)
        self.assertIn("sha256:$ARTIFACT_DIGEST", publish)
        self.assertIn(".total_count", publish)
        self.assertIn("environment:\n      name: firmware-release", publish)
        self.assertGreater(
            publish.index('git fetch origin "refs/tags/$VERSION:refs/tags/$VERSION"'),
            publish.index('gh release edit "$VERSION" --draft=false'),
        )

        self.assertIn("gh release download", promotion)
        self.assertIn('cmp "$file" "r2-package/$name"', promotion)
        self.assertIn("verify-release-package.py", promotion)
        self.assertIn("make-channel-manifest", promotion)
        self.assertNotIn("--clobber", promotion)
        self.assertNotIn("download-artifact", promotion)
        self.assertIn("environment:\n      name: firmware-release", promotion)
        for workflow in non_secret_workflows:
            self.assertNotIn("CLOUDFLARE_R2", workflow)

    def test_release_namespaces_belong_to_this_repository(self) -> None:
        factory = self.read("home-assistant-voice.realtime.factory.yaml")
        latest = self.read(".github/workflows/update-latest.yml")
        installer = self.read("static/index.html")
        build = self.read(".github/workflows/build.yml")
        publish = self.read(".github/workflows/publish-release.yml")

        prepare = self.read(".github/workflows/prepare-release.yml")
        pages = self.read(".github/workflows/gh-pages.yml")
        for source in (factory, latest, installer, publish, prepare, pages):
            self.assertNotIn("home-assistant-voice-pe/home-assistant-voice", source)
        self.assertNotIn("esphome.github.io", build)
        self.assertIn("True-Family-Voice-Firmware/home-assistant-voice", factory)
        self.assertIn("True-Family-Voice-Firmware/home-assistant-voice", installer)
        for workflow in (latest, publish, prepare, pages):
            self.assertIn(
                'GITHUB_REPOSITORY" = "TheOnlyHyland/True-Family-Voice-Firmware',
                workflow,
            )

    def test_live_config_is_excluded_and_docs_are_repository_owned(self) -> None:
        ignored = self.read(".gitignore")
        build = self.read(".github/workflows/build.yml")
        prepare = self.read(".github/workflows/prepare-release.yml")
        publish = self.read(".github/workflows/publish-release.yml")
        pages = self.read(".github/workflows/gh-pages.yml")
        docs = "\n".join(
            (
                self.read("README.md"),
                self.read("INSTALL.md"),
                self.read("CONTRIBUTING.md"),
                self.read(".github/ISSUE_TEMPLATE/config.yml"),
            )
        )

        self.assertIn("/home-assistant-voice.live.yaml", ignored)
        self.assertNotIn("home-assistant-voice.live.yaml", build)
        self.assertNotIn("home-assistant-voice.live.yaml", prepare)
        self.assertNotIn("home-assistant-voice.live.yaml", publish)
        self.assertNotIn("home-assistant-voice.live.yaml", pages)
        self.assertNotIn(
            "home-assistant-voice.live.yaml", self.read("scripts/package-release")
        )
        self.assertNotIn("TristanBrotherton", docs)
        self.assertIn("TheOnlyHyland/True-Family-Voice-Firmware", docs)
        self.assertIn("cf73d8dcee605a774229554e946f1fc51e515b2e", docs)
        self.assertIn("dfb598d33c55398b88afa40b5b694ac816963af1", docs)
        self.assertIn("e6f94cd26a0f23961aaf7283e5fdd61661820c1b", docs)

        adoption_sources = (
            self.read("README.md"),
            self.read("INSTALL.md"),
            self.read("esphome-builder.dhcp.yaml"),
            self.read("esphome-builder.static-ip.yaml"),
            self.read("home-assistant-voice.realtime.yaml"),
        )
        for source in adoption_sources:
            self.assertIn("auto-discover later releases", " ".join(source.split()))
            self.assertRegex(source, r"re-adopt(?:ion)?")
            self.assertNotIn("@main", source)
            self.assertNotIn("ref: main", source)
        issue_config = self.read(".github/ISSUE_TEMPLATE/config.yml")
        self.assertIn(
            "TheOnlyHyland/True-Family-Voice-Firmware/blob/0.20.1/INSTALL.md",
            issue_config,
        )
        self.assertNotIn("/blob/main/", issue_config)

    def test_installer_rejects_untrusted_versions_without_html_sinks(self) -> None:
        html = self.read("static/index.html")
        installer = self.read("static/installer.mjs")
        pages = self.read(".github/workflows/gh-pages.yml")
        vendor = self.read("scripts/vendor-installer")
        locked = self.read("external-inputs.lock")

        self.assertNotIn("innerHTML", html)
        self.assertNotIn("innerHTML", installer)
        self.assertIn("textContent", installer)
        self.assertNotIn("unpkg.com", html)
        self.assertIn("./vendor/esp-web-tools/install-button.js", html)
        self.assertIn('FIRMWARE_ORIGIN = "https://firmware.esphome.io"', installer)
        self.assertIn(
            '"/True-Family-Voice-Firmware/home-assistant-voice/"', installer
        )
        self.assertIn("VERSION_PATTERN.test(version)", installer)
        self.assertIn("encodeURIComponent(version)", installer)
        self.assertIn("html.escape(release_version, quote=True)", pages)
        self.assertIn("version_pattern.fullmatch(release_version)", pages)
        self.assertIn("lambda match:", pages)
        self.assertIn("html.escape(current_version, quote=True)", pages)
        self.assertIn("verify-version", pages)
        self.assertIn("scripts/vendor-installer", pages)
        self.assertNotIn("pip install requests", pages)
        self.assertIn("Authorization", pages)
        self.assertIn("per_page=100", pages)
        self.assertIn("esp-web-tools-10.0.1.tgz", vendor)
        self.assertIn("external-inputs.lock", vendor)
        self.assertIn("esp-web-tools-10.0.1.tgz", locked)

    def test_strict_yamllint_uses_locked_dependencies(self) -> None:
        workflow = self.read(".github/workflows/yaml-lint.yml")
        requirements = self.read("requirements-yamllint.txt")

        self.assertIn("yamllint --strict .", workflow)
        self.assertIn("scripts/run-actionlint", workflow)
        self.assertIn("--require-hashes", workflow)
        self.assertIn('python-version: "3.13.5"', workflow)
        self.assertRegex(requirements, r"(?m)^yamllint==1\.37\.1 \\")
        requirement_lines = [
            line
            for line in requirements.splitlines()
            if re.match(r"^[a-z0-9][a-z0-9_.-]*==", line)
        ]
        self.assertEqual(len(requirement_lines), 3)
        self.assertGreater(requirements.count("--hash=sha256:"), 6)


if __name__ == "__main__":
    unittest.main()
