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
The XBE is not included in this documentation; the build workflow obtains the user's
own project copy from the repository's existing private build input.
