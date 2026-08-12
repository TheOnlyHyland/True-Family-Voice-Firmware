# Install guide

All installation material for this firmware is maintained in this repository.

## Generic factory

Install `true-family-voice-esp32s3.factory.bin` from a release over USB. The
generic image has no ESPHome native API, no native OTA endpoint, and no shared
management credential. Wi-Fi may be provisioned through Improv, but this does
not expose native Home Assistant or ESPHome management.

## Secure adoption

1. Add `wifi_ssid`, `wifi_password`, `api_encryption_key`, and `ota_password` to
   the local ESPHome `secrets.yaml`. Generate a new API key and OTA password for
   each device; never add their values to this repository. The tracked
   `secrets.yaml.example` contains names and placeholders only.
2. Copy `esphome-builder.dhcp.yaml`, or the static-IP variant, into the local
   ESPHome dashboard and give the device a stable name. Keep both tracked source
   references on the same explicitly approved release tag.
3. Perform the first adopted-device installation over USB. This replaces the
   generic image directly, without creating an unauthenticated network API
   transition.
4. Subsequent Home Assistant communication uses the encrypted native API and
   subsequent native OTA uploads require the private per-device password.
5. The immutable `0.20.2` refs do not auto-discover later releases. To update,
   review a published release, deliberately advance both refs in the local stub
   to that exact tag and compile, or deliberately re-adopt that release's pinned
   stub. The tracked stubs never follow a branch or moving latest reference.

The stubs combine `home-assistant-voice.realtime.yaml` with
`home-assistant-voice.adopted.yaml`. The latter is the only tracked layer that
adds native API and native OTA, and it receives both credentials solely through
local secret substitutions.

## Mandatory rollback order

Rollback is **backend first**. Do not downgrade firmware while backend `0.21.x`
or `0.22.0` still requires the nonce-bearing `hello_ack`; firmware `0.18.0` and
older cannot satisfy that admission contract.

1. Keep firmware `0.20.2` installed and restore backend `0.20.6` first.
2. Restart the still-installed firmware after restoring the backend. Its
   admitted trusted session nonce blocks the nonce-less legacy zero hello until
   restart clears that session.
3. Verify that the restarted firmware has reconnected in exact legacy
   zero mode and that an ordinary single-turn request completes. Explicit
   no-wake follow-up remains unavailable in this compatibility state.
4. Only after that backend verification succeeds, move both immutable firmware
   references to the approved older tag and perform the firmware rollback.
5. If backend `0.20.6` cannot be restored, the firmware cannot be restarted, or
   legacy zero mode cannot be verified, stop. Do not roll back firmware first
   and do not bypass admission checks.

The reverse order is intentionally unsupported. Backend `0.21.x` remains
compatible only for ordinary physical-wake turns: every explicit follow-up OPEN
answer receives tokenless progression phases and fails closed. The safe forward
order is firmware `0.20.2` first, then coordinated backend `0.22.0`; no explicit
follow-up may be enabled before `0.22.0` echoes the current `request_follow_up`
token on OPEN `listening`, `thinking`, and `replying` while keeping terminal
`idle` tokenless. Context-bound graceful close must remain disabled until
backend `0.22.5` sends and validates the exact token, session nonce, and wake
generation on PREPARE, COMMIT, CANCEL, and the PREPARE/COMMIT ACKs. The safe
rollback order is backend `0.20.6`, restart and verify the still-installed
firmware in legacy zero mode, then install older firmware.
