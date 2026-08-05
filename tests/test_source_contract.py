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
        self.assertIn('firmware_version: "0.19.0"', factory_config)
        self.assertIn('va_url: "ws://homeassistant.local:8080/"', factory_config)
        self.assertNotIn("ota_password", factory_config)
        self.assertNotIn("\napi:\n", factory_config)
        self.assertNotIn("\napi:\n", realtime)
        self.assertNotIn("platform: esphome", realtime)
        self.assertNotIn("dashboard_import:", factory_config)
        self.assertNotIn("compile-only", factory_config)
        self.assertIn(
            'va_client_source: "github://TheOnlyHyland/True-Family-Voice-Firmware@0.19.0"',
            realtime,
        )
        self.assertIn("- source: ${va_client_source}", realtime)
        self.assertIn('version: "0.19.0"', realtime)

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

    def test_trusted_phase_and_timer_contract_is_structural(self) -> None:
        source = self.read("esphome/components/va_client/va_client.cpp")
        safety = self.read("esphome/components/va_client/follow_up_safety.h")

        self.assertIn(
            '{"type", "value", "session_nonce", "wake_generation"}', source
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
        self.assertIn("PhaseApplyStatus::STALE", phase_handler)
        self.assertIn("follow_up_input_ended", source)
        self.assertIn("phase_effect_plan_is_current", source)
        self.assertIn("transition.connection_generation", source)
        self.assertIn("transition.wake_generation", source)
        self.assertIn("GenerationEffectGate", safety)
        self.assertIn('"va_session_ceiling"', source)
        self.assertIn("kAbsoluteSessionMaxMs", source)
        self.assertIn("kRequestFollowUpReadyTimeoutMs = 8000", safety)
        self.assertIn("kAbsoluteSessionMaxMs = 120000", safety)

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

        self.assertEqual(version, "0.19.0")
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
        self.assertIn("verify-release-intent.py", publish)
        self.assertNotIn("esphome", publish.lower())
        self.assertNotIn("package-release", publish)
        self.assertNotIn("pip install", publish)
        self.assertLess(
            publish.index("--draft"),
            publish.index("Publish exact versioned R2 bytes"),
        )
        self.assertGreater(
            publish.index('gh release edit "$VERSION" --draft=false'),
            publish.index("Publish exact versioned R2 bytes"),
        )
        self.assertIn("needs:\n      - publish", publish)

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
            ["sh", str(ROOT / "scripts/verify-version"), "0.19.0"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(exact.returncode, 0, exact.stderr)
        self.assertEqual(exact.stdout.strip(), "0.19.0")
        wrong = subprocess.run(
            ["sh", str(ROOT / "scripts/verify-version"), "v0.19.0"],
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

        self.assertIn('--if-none-match "*"', publish)
        self.assertIn("Refuse any existing tag, release, asset, or R2 object", publish)
        self.assertIn("github-readback", publish)
        self.assertIn("r2-readback", publish)
        self.assertNotIn("--clobber", publish)
        self.assertIn('cmp "$package/$file" "r2-readback/$file"', publish)
        self.assertIn('cmp "$package/$file" "github-readback/$file"', publish)
        self.assertIn("sha256:$ARTIFACT_DIGEST", publish)
        self.assertIn(".total_count", publish)
        self.assertIn("environment:\n      name: firmware-release", publish)

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
            "TheOnlyHyland/True-Family-Voice-Firmware/blob/0.19.0/INSTALL.md",
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
