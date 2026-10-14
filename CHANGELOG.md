# Changelog

## 2026-10-14 — Epoch 8 source update

This updates the public source to `aipet-8.0.0-240`. It is not a new tagged
binary release or a claim of physical acceptance on every supported board.

### Changed

- Replaced the previous three-pet, dual-application layout with
  `one-pet-10m-v5`: one factory application, one pet pack up to 10,739,712
  bytes and a dedicated 2 MiB story sound bank.
- Firmware updates now use USB only; compatible signed pet and sound content
  still installs over Wi-Fi.
- Added asynchronous frame streaming and bounded resident/cache handling for
  larger animation packs.
- Expanded story support with character and blessing screens, animated events,
  atmosphere loops, sound-bank playback and speech interaction controls.
- Added saved-network auto-join, live Wi-Fi switching, backend-provided network
  settings and the USB command to forget a saved network.
- Added BOOT-button push-to-talk, LCD 1.54 power-button handling and more
  natural idle animation.
- Updated onboarding and the control-deck presentation, and refreshed bundled
  font/audio attribution.
- Updated public build, install, backend and pet-pack documentation, removing
  obsolete two-app-slot flashing instructions and unpublished-backend promises.
- Updated release packaging to read application offsets from the build's
  partition table. Full images erase all settings and content; manifests now
  record the partition-table hash and distinguish untested physical acceptance.

### Supported ESP32 devices

- Waveshare ESP32-S3-Touch-LCD-1.85B.
- Waveshare ESP32-S3-Touch-LCD-1.54.
- Waveshare ESP32-S3-Touch-AMOLED-2.06.

Use the exact board build; see the [hardware table](README.md#supported-boards).

### Migration

Older partition layouts require a layout-aware migration or a deliberate
fresh installation that erases settings and content. Do not install the new
application alone over an older partition table, or write a second copy at
`0x3b0000`: that address is now the pet journal. Follow
[getting started](docs/getting-started.md#flash-and-migrate).

### Verification

- ESP-IDF 5.5.3 release builds passed for all three supported boards. Each
  application fits its 3.5 MiB partition with 22% remaining.
- The 160-test host run passed 159 tests and exposed an undersized public
  streaming fixture. After replacing it with a generated multi-window pack,
  both affected frame-player tests passed. The other tests were unchanged.
- Four packaging regression tests cover factory and legacy layouts,
  oversized applications and overlapping image regions.
- Packaging of all three actual builds passed hash, partition-offset and
  erased-owner-region checks. The neutral-artwork LCD 1.54 menu preview rendered.
- Public-source secret and private-marker scans, documentation link checks
  and independent publication review passed.

No physical-device flashing or acceptance is included in this source update.
Hosted GitHub Actions are intentionally paused.

Before resuming automated binary releases, update the disabled Release
workflow's legacy two-slot release-note template to use the packaged manifest's
offsets. Its old `0x3b0000` app-write instruction is incompatible with this
layout. No workflow was changed or enabled by this source update.
