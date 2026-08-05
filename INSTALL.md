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
5. The immutable `0.19.0` refs do not auto-discover later releases. To update,
   review a published release, deliberately advance both refs in the local stub
   to that exact tag and compile, or deliberately re-adopt that release's pinned
   stub. The tracked stubs never follow a branch or moving latest reference.

The stubs combine `home-assistant-voice.realtime.yaml` with
`home-assistant-voice.adopted.yaml`. The latter is the only tracked layer that
adds native API and native OTA, and it receives both credentials solely through
local secret substitutions.

## Mandatory rollback order

Rollback is **backend first**. Do not downgrade firmware while backend `0.21.0`
still requires the nonce-bearing `hello_ack`; firmware `0.18.0` and older cannot
satisfy that admission contract.

1. Keep firmware `0.19.0` installed and restore backend `0.20.6` first.
2. Verify that the still-installed firmware has reconnected in exact legacy
   zero mode and that an ordinary single-turn request completes. Explicit
   no-wake follow-up remains unavailable in this compatibility state.
3. Only after that backend verification succeeds, move both immutable firmware
   references to the approved older tag and perform the firmware rollback.
4. If backend `0.20.6` cannot be restored and verified, stop. Do not roll back
   firmware first and do not bypass admission checks.

The reverse order is intentionally unsupported. The safe forward order remains
firmware `0.19.0` before backend `0.21.0`; the safe rollback order is backend
`0.20.6` before older firmware.
