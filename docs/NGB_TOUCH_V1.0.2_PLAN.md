# NGB Touch development line — upstream v1.0.2

This branch starts from the original author's signed-off source tag `v1.0.2`
(commit `7481d84b7cfab7532d07a0d0c9db74151076c898`), not from the older
pre-release fork snapshot.

Upstream changes intentionally retained as the baseline:
- The original author's Classic touch layout and its saved in-game layout switch.
- The upstream overlay dirty-region rendering path and presenter integration.
- Upstream touch artwork generation, gamepad mapping, and Android packaging.

NGB Touch design direction:
- Dark, restrained editor surfaces with a technical-blue accent.
- Keep the in-game overlay light and avoid an always-running Android UI layer.
- Preserve upstream gameplay behavior while adding per-control customization.

Planned editor work, in order:
1. Port the existing fork's per-control move/resize/opacity/shape/skin persistence onto
   the v1.0.2 touch implementation without removing Classic layout support.
2. Add a floating edge handle and a compact properties panel.
3. Expand custom combo configuration from the current fixed mappings into a user-editable
   input sequence / simultaneous-button mapping.
4. Add user artwork import and per-control texture assignment.
5. Verify dirty-region updates and compare 2K frame pacing and power use on device.

The Android CI workflow is included on this branch. APK builds use versionCode 1003 and
versionName `1.0.2-ngb-touch` to distinguish this fork build from the author's release.
The XBE is not included in this documentation. The build workflow expects the project's
own XBE input to be available in the build repository; do not redistribute copyrighted game data.

## Follow-on version: input and presentation extensions

These are deliberately kept behind stable touch-control interfaces so they can be
added without changing the renderer's per-control dirty-region contract:

- Display filters: selectable shader/presentation effects, with a neutral default,
  per-device performance cost checks, and no full-resolution CPU post-processing.
- Button haptics: configurable short vibration on press, with a global toggle,
  intensity/duration limits, and Android's native vibration API; never vibrate
  continuously while a finger is held.
- Native swipe-to-camera: an explicitly configurable gesture zone that translates
  horizontal/vertical finger deltas into right-stick camera input. It must coexist
  with normal buttons and avoid stealing gestures from Android system navigation.
- 360Y and other techniques: a user-editable gesture/sequence engine supporting
  ordered inputs, simultaneous inputs, timing windows, release gaps, repeat policy,
  and cancellation. 360Y should be a named preset built on that engine, not a
  hard-coded special case.
- Additional actions: design the input model so future gestures, macros, and
  per-control actions can be registered without growing a fixed enum for every move.

The sequence engine must define timing in game-input frames (with a documented
frame-rate policy), cancel safely when focus is lost or the editor opens, and
never block the render/present thread. Haptics and gesture recognition run from
input events; filter selection changes renderer state only when the setting changes.

## Acceptance gates

1. The Android ARM64 CI build completes from a legally supplied local XBE input.
2. Editor changes preserve upstream v1.0.2 dirty-rectangle overlay updates; no
   unconditional full overlay upload on finger movement.
3. Layout, per-control appearance, artwork references, and combo definitions
   survive app restart and are versioned/migratable.
4. Editor can be entered/exited without leaving virtual buttons stuck down.
5. Swipe camera can be disabled or assigned a dedicated region and does not
   interfere with buttons, sticks, or Android edge navigation.
6. Combo presets are tested in a repeatable input trace, including 360Y timing.
7. Filters are benchmarked on target Adreno hardware at the device's native
   resolution; effects that materially reduce frame pacing remain opt-in.
