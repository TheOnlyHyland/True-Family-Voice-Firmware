# Changelog

## 0.20.2

- Treat the explicit follow-up deadline as a microphone-aperture limit. A valid
  ordered `listening -> thinking` endpoint closes the microphone and clears the
  10-second deadline, so model response time cannot cancel an accepted answer.
- Keep silence, partial speech without an endpoint, malformed phase order, and
  exact-deadline races fail closed. The independent 120-second whole-session
  ceiling and all protocol schemas remain unchanged.
- Update immutable package refs, factory metadata, installer links, tests, and
  release version checks to `0.20.2`.

## 0.20.1

- Make accepted `commit_suppress_followup` enter terminal idle locally after
  PREPARE revokes lifecycle ownership. Silent responses with no audio and
  responses with queued audio now share the existing speaker-drain and
  negotiated tail-delay path before the LED returns to idle.
- Keep the microphone closed and follow-up suppressed throughout graceful
  close. Ordinary request follow-up, Stop, mute, credentials, and protocol
  schemas are unchanged.
- Make revocation of a prepared or committed graceful close deterministic.
  Mute, Stop, cancellation, rejected or replayed COMMIT, disconnect, and an
  immediate replacement wake now burn both close tokens, cancel the tail timer,
  and settle runtime and LED idle without reopening the microphone.
- Bind graceful close to an exact token, session nonce, and wake generation.
  PREPARE stores the active wake owner; COMMIT and CANCEL require that complete
  tuple; PREPARE/COMMIT ACKs echo it. Stale controls are ignored without
  mutating a replacement wake. This flow requires coordinated backend `0.22.5`.
- Update immutable package refs, factory metadata, installer links, tests, and
  release version checks to `0.20.1`.

## 0.20.0

- Replace the physical-wake one-shot with one PREPARE grant per genuine answer.
  A successful ordered `OPEN -> listening -> thinking -> replying` round rearms
  exactly one grant while retaining the original physical wake generation.
- Permit serialized follow-up rounds inside the existing generation-bound
  120-second whole-session ceiling, with fixed non-evicting 256-value token and
  READY-nonce histories that fail closed if exhausted.
- Store each OPEN microphone aperture's absolute 10-second deadline at COMMIT.
  Check it from lifecycle transitions, the main loop, mic lease admission, and
  the cooperative timer so delayed callbacks cannot admit late phases or rearm.
- Require OPEN `listening`, `thinking`, and `replying` phases to echo the exact
  current follow-up token. Delayed phases from an older round are harmless
  no-ops and cannot progress a later OPEN transaction.
- Keep terminal `idle` tokenless. Any tokenized `idle` is invalid and fails the
  current wake closed rather than being treated as delayed progression traffic.
- Reset the fixed round histories only for a fresh hello; reconnect recovery
  preserves them and never evicts an older accepted credential.
- Never rearm after denied, failed, cancelled, malformed, stale, disconnected,
  muted, stopped, media-conflicted, timed-out, or session-ceiling paths. These
  paths continue to close locally before bounded protocol cleanup.
- Update immutable package refs, factory metadata, installer links, tests, and
  release version checks to `0.20.0`.

### Deployment Compatibility

Firmware `0.20.0` retains ordinary single-turn compatibility with backend
`0.20.6` and the nonce-bearing protected protocol introduced by backend
`0.21.x`. Backend `0.21.x` still sends tokenless trusted phases, so all explicit
follow-up OPEN answers fail closed; only ordinary physical-wake turns are
compatible. Coordinated backend `0.22.0` is required for any explicit follow-up.
Deploy firmware before backend `0.22.0`. Rollback remains backend-first as
documented in `INSTALL.md`: after restoring backend `0.20.6`, restart the
still-installed firmware to clear its trusted session nonce before verifying
legacy zero mode or downgrading firmware.

## 0.19.0

- Add nonce- and token-bound model-selected follow-up requests in closed,
  single-turn mode.
- Enforce one no-wake follow-up per local wake generation. Cancellation,
  speech, completion, timeout, hello, and reconnect never replenish the budget.
- Add PREPARE, READY, and final COMMIT stages. The microphone remains closed
  through reply drainage, optional follow-up chime, echo guard, READY, and the
  exact bounded `commit_follow_up_ack`; it opens only after a final local recheck.
- Revoke pending or open follow-ups on mute, stop, disconnect, recovery,
  competing control, malformed credentials, and replayed credentials.
- Make one local lifecycle state machine authoritative for physical-wake grants,
  enrollment, mute, mic ownership, follow-up stages, generations, and replay
  histories. Delayed YAML wake paths carry the captured generation.
- Close locally before finite-timeout WebSocket controls. Reject partial sends,
  and bind trusted ACKs, controls, timers, and callbacks to current credentials.
- Synchronize PCM producer, reset, and consumer state. Playback drains through a
  private scratch copy and commits only against the same ring generation.
- Require the exact nonce-less backend `0.20.6` zero-mode hello or exact
  nonce-bearing `0.21.0` hello. Unknown fields and non-zero automatic follow-up
  are rejected.
- Add host-executable parser and lifecycle state-machine tests. CI compiles and
  releases only the deployable Realtime overlay against checked-out `va_client`
  source with ESPHome `2026.7.3`.
- Document that RAPID-PILOT transport remains unauthenticated plaintext
  `ws://`; HMAC, PSK, and provisioning are intentionally not included.
- Separate local delayed-wake reservations from transmitted protocol
  generations. Aborted or restarted reservations no longer create backend
  sequence gaps; a successful wake send commits exactly one generation.
- Add an independent generation-bound 120-second whole-session mic ceiling that
  survives audio ACKs, PCM, replies, and explicit follow-up stages.
- Fence every bounded mic send with a revocation epoch and 50 ms drain barrier
  before any flush, interrupt, or client-revoke control.
- Require trusted phase messages to carry the exact current `session_nonce` and
  `wake_generation`; preserve the nonce-less exact legacy phase schema.
- Use negotiated `follow_up_open_delay_ms`, expand the READY callback deadline
  to cover its maximum, and include the separate announcement speaker/state in
  READY and final COMMIT checks.
- Remove protocol token, nonce, ready nonce, and wake-generation values from
  runtime logs.
- Add a generic no-shared-password factory target, immutable external-input
  checks, repository-owned installer/update namespaces, and single-build
  release packaging that promotes the exact verified artifact.
- Make trusted `phase=thinking` the strict single-turn endpoint: close the mic
  and reject further PCM while retaining wake generation, whole-session budget,
  and response ownership through `replying` until terminal `idle`.
- Preserve `OPEN -> thinking -> replying -> idle` ownership without revoking the
  explicit follow-up response, and keep model-selected PREPARE available during
  the original physical wake's reply.
- Validate trusted phase context and apply the transition under one lifecycle
  lock. Delayed same-session phases for an older wake are harmless no-ops.
- Add bounded WebSocket message reassembly across ESP-IDF transport chunks and
  RFC continuation frames, including strict opcode/FIN/offset/size validation
  and final PCM16 alignment checks.
- Remove installer HTML sinks, constrain versions to a strict allowlist, pin
  manifest construction to the repository-owned origin/path, and escape release
  metadata generated by the Pages workflow.
- Serialize phase validation, lifecycle mutation, timer effects, mic effects,
  wake replacement, and delayed automations with one generation-effect gate.
  Full connection/session/wake effect plans make callbacks from superseded
  phases true no-ops.
- Quarantine and reconnect the transport when an authoritative `flush`,
  `interrupt`, or `client_revoke` send fails. Replacement wakes remain blocked
  until a new exact backend hello is admitted.
- Base the candidate on exact public-main merge
  `cf73d8dcee605a774229554e946f1fc51e515b2e`, including public change
  `dfb598d33c55398b88afa40b5b694ac816963af1`, while retaining all eight
  candidate commits through `e6f94cd26a0f23961aaf7283e5fdd61661820c1b`
  and the dirty lifecycle, transport, prebuffer, and release protections.
- Require release tags and package versions to equal embedded `0.19.0`. Beta
  and production promotions use separate protected-environment approvals, and
  release publication no longer promotes production.
- Remove ESPHome native API and native OTA from the common/public factory
  layers. The tracked adopted-device overlay adds encrypted API and private OTA
  only when local secret substitutions are supplied.
- Exclude local live YAML from version control and publication, and move all
  installer and documentation links into this repository.
- Require the ESPHome two-worker cap and a non-optional process-wide compile
  lock, and byte-compare every packaged binary with the single compiled source.
- Make both production promotion and production-latest updates explicit manual
  workflows guarded by the production environment and the exact release tag.
- Quarantine the transport inside the mic-send barrier helper itself. Thinking,
  timeout, mute, announcement, enrollment, Stop, revoke, and follow-up closure
  failures all close locally, force a WebSocket boundary and reconnect, and
  block wakes until a fresh admitted hello.
- Build and package releases before any tag or public release exists. Protected
  preparation emits one source/tree/version/hash-bound immutable artifact;
  approved publication downloads that exact artifact by run and ID, verifies its
  digest, publishes no-clobber GitHub assets through a private draft, reads every
  asset back, and makes the release public last. Rapid-pilot publication leaves
  Pages advertisement and channel migration to separate post-pilot work.
- Make beta and production promotions consume public versioned GitHub bytes and
  matching versioned R2 bytes without rebuilding. Scope every R2 secret reference
  to the protected `firmware-release` environment.
- Lock Actionlint `1.7.12`, GNU patch `2.7.6`, and the PlatformIO Improv `1.2.4`
  archive; verify the exact extracted Improv tree and normalized ESP-IDF
  component-manager closure after compilation.
- Document mandatory backend-first rollback: restore backend `0.20.6`, restart
  the still-installed firmware to clear its trusted session nonce, and verify
  legacy zero mode before downgrading firmware.
- Make ESPHome `2026.7.3` release timestamps reproducible with one hash-bound,
  fail-closed `SOURCE_DATE_EPOCH` patch and the same deterministic local and
  protected-CI environment.
- Clarify that adopted devices pinned to immutable `0.19.0` refs do not discover
  later releases; updates require deliberate ref advancement or re-adoption.

### Deployment Compatibility

Deploy firmware `0.19.0` before backend `0.21.0`. Firmware `0.19.0` continues
normal single-turn operation with backend `0.20.6` through its nonce-less
legacy hello. Explicit safe follow-up remains unavailable until the nonce
protocol is present. Backend `0.21.0` must require the matching `hello_ack`, so
firmware `0.18.0` and older fail admission rather than receiving follow-up
controls they cannot enforce.
