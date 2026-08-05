# Contributing

Changes and review happen in
[True-Family-Voice-Firmware](https://github.com/TheOnlyHyland/True-Family-Voice-Firmware).

Firmware-specific bar:

- **You flashed it.** State the device revision and that wake word, timers,
  voice enrollment, and the phase LED states still work after your change.
- The firmware stays a **thin audio client** — session logic, tools, and
  memory belong in the backend add-on. PRs that grow a second brain on the
  device will be redirected there.
- Substitutions are the public API of this package: renaming or removing one
  breaks every user's device stub. Add, don't rename.
- Run `./scripts/verify` for host tests. Firmware changes must also pass
  `./scripts/verify --compile`, which builds the deployable Realtime YAML from
  the checked-out local component rather than the currently published tag. The
  host gate includes checksum-locked Actionlint; CI also runs hash-locked strict
   yamllint. The compile gate requires checksum-built GNU patch `2.7.6`, verifies
   the exact ESP-IDF component-manager and Improv closures, and remains protected
   by the process-wide compile lock. Local and protected release builds use the
   same version-controlled `SOURCE_DATE_EPOCH` and fail-closed ESPHome `2026.7.3`
   source patch.
