# True Family Voice Realtime Firmware

ESPHome firmware that turns a **Home Assistant Voice PE** into the thin,
low-latency audio client half of
**[True Family Voice Realtime](https://github.com/TheOnlyHyland/True-Family-Voice-Firmware)**.
Wake-word detection (default: **"Hey Leonard"**, switchable to Hey Jarvis /
Okay Nabu from Home Assistant) runs on-device; everything else — speech
understanding, replies, smart-home control — streams over a local WebSocket to
the backend add-on's OpenAI Realtime session.

## Documentation

- [Secure installation and adoption](INSTALL.md)
- [Building and release verification](#building-from-source)
- [Backend protocol contract](#follow-up-protocol-compatibility)
- [Issues and support](https://github.com/TheOnlyHyland/True-Family-Voice-Firmware/issues)

## Building from source

You don't build anything by hand for a normal install — the per-device stub
([`esphome-builder.dhcp.yaml`](esphome-builder.dhcp.yaml) or
[`esphome-builder.static-ip.yaml`](esphome-builder.static-ip.yaml)) pulls
[`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml) from
this repo at an immutable release tag, and ESPHome Builder compiles and flashes
it. A stub pinned to `0.19.0` does not auto-discover later releases. Updating is
deliberate: review a release, advance both pinned refs in the local stub to that
exact tag and compile, or deliberately re-adopt the newer release's pinned stub.
To hack on the firmware itself, point the stub's `packages:` block at your
fork/branch instead. Wake-word models live in
[`models/`](models/) (previous model kept in `models/previous/` for rollback).

The local verification gate compiles
[`home-assistant-voice.realtime.factory.yaml`](home-assistant-voice.realtime.factory.yaml),
the generic publishable target, with `va_client` resolved from this checkout. It
verifies every remote model, sound, and XMOS image against
[`external-inputs.lock`](external-inputs.lock), checks the exact cached bytes
consumed by ESPHome, and compares every generated C++ component file
byte-for-byte with the checkout:

```sh
./scripts/verify
./scripts/verify --compile
```

The second command requires the hash-locked Python build tools and ESPHome
`2026.7.3` closure, ESP-IDF `5.5.5`, and GNU `patch` `2.7.6`. Source-only Python
dependencies build without an isolated or unpinned backend. The generated
ESP-IDF component-manager closure is normalized only for its workspace path and
derived manifest hash, then checked against
[`idf-component-manager.lock`](idf-component-manager.lock). Host verification
also locks the PlatformIO Improv `1.2.4` archive and verifies the exact extracted
component tree consumed by ESP-IDF, requires checksum-built GNU patch `2.7.6`,
and runs checksum-locked Actionlint `1.7.12`. The compile produces the factory,
OTA, and ELF artifacts under
`.esphome/build/true-family-voice/build/`. CI runs that compile once, packages
those exact files with
[`scripts/package-release`](scripts/package-release), and promotes the uploaded
artifact without rebuilding it. The generic image has no shared OTA password;
per-device private Wi-Fi, encrypted API, and OTA credentials remain in local
stubs and are not release inputs. The public factory has no ESPHome native API
and therefore exposes no unauthenticated native API endpoint.

Release builds read the fixed epoch from [`SOURCE_DATE_EPOCH`](SOURCE_DATE_EPOCH)
and export the same UTC, C-locale, and Python hash environment locally and in
protected CI. A checksum-bound one-line patch changes only ESPHome `2026.7.3`'s
`writer.py` build timestamp source from `time.time()` to that required epoch.
Verification refuses a different ESPHome version, upstream source hash, patch
hash, patched result, environment epoch, or generated build timestamp.

The tracked layering is deliberate:

- `home-assistant-voice.realtime.yaml` is the common firmware without native
  API or native OTA.
- `home-assistant-voice.realtime.factory.yaml` adds only generic provisioning,
  repository-owned HTTP updates, and release metadata.
- `home-assistant-voice.adopted.yaml` is included by the tracked per-device
  stubs and adds encrypted native API plus password-protected native OTA from
  local `secrets.yaml` values.
- `home-assistant-voice.factory.yaml` is a compatibility alias to the same
  secure generic factory. The obsolete upstream 8 MB and duplicate common
  configs are not publication targets and have been removed.

## Follow-up protocol compatibility

Firmware `0.19.0` is firmware-first compatible with backend `0.20.6`: a legacy
nonce-less hello keeps ordinary single-turn voice operation working, but cannot
authorize explicit no-wake follow-up. Backend `0.21.0` must send a nonce-bearing
hello and require the matching `hello_ack`; that requirement rejects firmware
`0.18.0` and older before any protected control is sent. See
[`CHANGELOG.md`](CHANGELOG.md) for the deployment order.

### Exact hello schemas

JSON key order and whitespace are irrelevant, but each object must contain
exactly the listed keys. Unknown, missing, duplicated, non-integer, negative, or
out-of-range fields reject admission. `follow_up_ms` must be `0`;
`follow_up_open_delay_ms` and `wake_open_delay_ms` must be at most `5000`, and
`playback_prebuffer_ms` must be at most `2000`.

Backend `0.20.6` legacy zero mode sends:

```json
{"type":"hello","audio_out":"pcm","follow_up_ms":0,"follow_up_open_delay_ms":800,"wake_open_delay_ms":700,"playback_prebuffer_ms":120}
```

The firmware sends no legacy `hello_ack`. Legacy mode permits ordinary
single-turn wake/audio operation only. Automatic and explicit no-wake follow-up
remain disabled, and legacy `wake`, `ack`, `interrupt`, and `flush` retain their
nonce-less schemas.

Backend `0.21.0` trusted mode sends the same exact object plus a positive,
CSPRNG-generated 31-bit `nonce`:

```json
{"type":"hello","nonce":123456789,"audio_out":"pcm","follow_up_ms":0,"follow_up_open_delay_ms":800,"wake_open_delay_ms":700,"playback_prebuffer_ms":120}
```

Firmware answers with this exact key set:

```json
{"type":"hello_ack","nonce":123456789,"accepted":true,"audio_out":"pcm","follow_up_ms":0,"follow_up_open_delay_ms":800,"wake_open_delay_ms":700,"playback_prebuffer_ms":120}
```

Backend `0.21.0` must compare the nonce, acceptance, audio mode, zero-mode value,
and all three echoed timing values before sending any protected control. A
missing, rejected, failed, or mismatched ACK must fail the connection closed.
The most recent nonce may recover after reconnect only with identical tuning
values. An older nonce replay is rejected. Session, follow-up-token, and
ready-nonce histories each hold 256 values and fail closed when full.

### Two-phase explicit follow-up

All tokens and nonces below are positive signed 31-bit integers. Objects use
exact key sets.

1. During the reply belonging to a current physical wake, backend sends
   `{"type":"request_follow_up","token":T,"session_nonce":S}`.
2. Firmware atomically spends that wake's one-shot grant, closes the mic, and
   sends `{"type":"request_follow_up_ack","token":T,"session_nonce":S,"accepted":true}`.
   This is PREPARE only; it never authorizes backend audio consumption.
3. After a bound backend `phase=idle`, the PCM ring and TTS speaker chain drain.
   Firmware runs the optional chime and the negotiated
   `follow_up_open_delay_ms` echo guard while the mic stays closed. The READY
   deadline is 8 seconds, covering the 2-second chime wait, the allowed
   5-second negotiated delay, and scheduling margin.
4. Firmware creates an opaque fresh `R` and sends
   `{"type":"follow_up_ready","token":T,"session_nonce":S,"ready_nonce":R}`.
5. Backend sends
   `{"type":"commit_follow_up","token":T,"session_nonce":S,"ready_nonce":R}`.
6. Firmware sends
   `{"type":"commit_follow_up_ack","token":T,"session_nonce":S,"ready_nonce":R,"accepted":true}`
   with the mic still closed. It then rechecks mute, enrollment, connection,
   ownership, token/nonces, audio generation, empty PCM ring, drained TTS
   speaker, and inactive/drained announcement speaker before opening the mic.
   An announcement beginning after READY revokes the transaction.
7. The aperture closes after an absolute 10 seconds, including time after the
   backend reports speech. Mute, Stop, disconnect, new local wake, late audio,
   malformed or competing control, failed/partial send, cancellation, and every
   stale timer close locally first and cannot reopen it. A separate
   generation-bound 120-second whole-session ceiling remains armed across ACK,
   PCM, reply, PREPARE, READY, and OPEN until authoritative session closure.

Backend must not emit `phase=listening` or consume a continuation before the
accepted final COMMIT ACK. It must also honor a later bound `client_revoke`,
`interrupt`, or `flush`; a race discovered by the firmware's post-ACK recheck
can cancel the transaction while keeping the mic closed.

Backend cancellation is
`{"type":"cancel_request_follow_up","token":T,"session_nonce":S}`; firmware
answers with the same values plus `accepted` and `cleared`. Backend audio ACKs
in trusted mode are exactly
`{"type":"ack","session_nonce":S,"wake_generation":G}`. Legacy ACK is exactly
`{"type":"ack"}`. Trusted `wake`, `interrupt`, `flush`, and `client_revoke`
controls carry the current `session_nonce` and `wake_generation`; `interrupt`
and `client_revoke` also carry a firmware-defined `reason` string.

Trusted backend phases are exact, ownership-bound objects:

```json
{"type":"phase","value":"listening","session_nonce":S,"wake_generation":G}
```

The same exact key set applies to `thinking`, `replying`, and `idle`. A stale,
same-session wake generation is a harmless no-op so delayed traffic cannot
close a newer wake. Missing, extra, unknown, or foreign-session phase data fails
closed. Legacy backend `0.20.6` keeps the exact unbound shape
`{"type":"phase","value":"listening"}`.

Trusted `phase=thinking` is the strict single-turn endpoint: firmware closes
the mic gate, invalidates its send epoch, waits for the bounded send barrier,
and rejects all further PCM for that turn. The same physical wake generation,
120-second budget, and response ownership remain active through `replying`, so
the model may send one `request_follow_up` during that reply. Terminal `idle`
closes the wake unless PREPARE/READY already owns it. For an explicit follow-up,
`OPEN -> thinking -> replying -> idle` closes input at `thinking`, retains the
response owner through `replying`, consumes no additional wake generation, and
closes authoritatively at `idle`.

Incoming WebSocket events are reassembled before dispatch. Text controls are
limited to 2048 bytes and binary PCM messages to 64 KiB. Frame offsets, opcode,
FIN, continuation, control-frame, and total-length semantics must be exact;
duplicate, malformed, oversized, empty, or final odd-length PCM messages are
rejected. Transport chunks and continuation fragments may individually split a
PCM16 sample on an odd byte boundary as long as the complete message is even.

Local delayed wake callbacks carry a private reservation id. Aborting or
restarting a reservation does not advance `wake_generation`; the protocol value
advances only after the bounded `wake` frame is transmitted. If a post-send
mute or disconnect race prevents mic opening, the transmitted generation stays
committed and is revoked, preserving a contiguous backend sequence.

Every mic frame acquires an epoch-bound bounded-send lease. Local mute, Stop,
disconnect, timeout, phase close, announcement start, enrollment exit, and
revoke close the mic gate first, invalidate its epoch, and wait at most 50 ms
for in-flight 20 ms sends before emitting `flush`, `interrupt`, or
`client_revoke`. If any barrier does not drain, the same local quarantine makes
the transport ineligible, closes or destroys the WebSocket, schedules a
reconnect, and blocks every wake until a fresh exact hello is admitted on the
replacement transport. No barrier-failure path returns with the old transport
eligible.

### Release integrity

All upstream Voice PE sounds and the `voice_kit` component use commit
`0579e7b9d8504264719c593474c85447253c9dc1`. The VAD uses commit
`05b65922cc433c9df13e98e32a7fe520758c837e`; stock wake models and the XMOS
firmware use named release versions plus locked SHA-256 values. The custom Hey
Leonard model and gentle timer sound are repository-local. Installer, update,
R2, and Pages paths use the `True-Family-Voice-Firmware` repository namespace.
The Pages build stages ESP Web Tools `10.0.1` from its SHA-256-locked npm archive
and serves only its verified local import closure. The installer accepts only
canonical numeric semantic versions with optional `alpha`, `beta`, or `rc`
suffixes, constructs manifests under the fixed repository-owned firmware
origin/path, and writes version labels with DOM text properties rather than HTML
parsing.

The exact public upstream base is
`cf73d8dcee605a774229554e946f1fc51e515b2e`, which contains public change
`dfb598d33c55398b88afa40b5b694ac816963af1` (`Select::current_option()`). The
eight candidate commits remain above that base through
`e6f94cd26a0f23961aaf7283e5fdd61661820c1b`; the dirty RAPID-PILOT work is
preserved on top.
Release tags must exactly equal `VERSION` and the embedded project version.
Normal CI cannot publish. A protected `workflow_dispatch` checks out one exact
40-character commit, verifies its tree and unused tag intent, compiles once, and
uploads one immutable artifact containing the five-file package plus canonical
source/version/hash metadata. A separately approved publication dispatch accepts
the exact preparation run, artifact ID, and artifact digest. It downloads by ID,
rechecks source, tree, version, and every package hash, and never invokes a
compiler. Publication refuses existing tags, releases, assets, and versioned R2
objects. It creates a private draft, uploads and reads back GitHub and atomic
no-overwrite R2 bytes, then makes the release public as its final operation.
Pages is a reusable workflow called only after that publication job succeeds.

Beta and production channel promotions are separate manual approvals. They
download the public GitHub package, require byte equality with its published
versioned R2 objects, and derive only the moving channel manifest; they never
reuse a preparation artifact or rebuild firmware. Configure `firmware-beta` and
`firmware-production` as approval-only environments. Configure
`firmware-release` as a protected environment and store all four R2 values there
as environment secrets only: `CLOUDFLARE_R2_ACCESS_KEY_ID`,
`CLOUDFLARE_R2_SECRET_ACCESS_KEY`, `CLOUDFLARE_R2_ACCOUNT_ID`, and
`CLOUDFLARE_R2_BUCKET`. Do not retain repository-scoped copies. Every job that
references those values declares `firmware-release`.

### Pilot transport limitation

This remains a RAPID-PILOT LAN protocol over plaintext `ws://`. The nonce,
generation, and token checks bind state transitions and reject stale/replayed
controls, but they do not authenticate the peer or provide confidentiality or
integrity against an active LAN attacker. There is deliberately no HMAC, PSK,
certificate pinning, or provisioning flow in firmware `0.19.0`.

The generic factory image contains neither ESPHome native API nor native OTA,
so there is no unauthenticated native management interval. Secure adoption uses
a one-time local USB install of a per-device stub with an encrypted API key and
private OTA password; see [`INSTALL.md`](INSTALL.md). The verification gate
proves host state transitions, resolved build inputs, source identity, and
compilation, but does not replace a physical-device/backend interoperability
test.

---
*Based on / inspired by xandervanerven's and maxmaxme's Voice PE work and the
official esphome/home-assistant-voice-pe — with thanks.*
