# Android touch editor — first release

The Android touch editor is developed on top of upstream tag `v1.0.1`. Keep the upstream game/runtime changes; do not copy the older pre-1.0.1 touch implementation over this branch.

## First-release behaviour

- Keep the original Xbox 360 touch-pad mapping and the upstream 1.0.1 rendering/input path.
- Keep the Ninpo and Wind Run shortcuts, and allow the user to add, rename, move, resize, and remove arbitrary combo controls. A combo is a simultaneous set of two or more Xbox inputs; it must release every member when the finger lifts.
- Allow ordinary controls and combo controls to be moved, resized, and assigned an opacity.
- Provide a colour palette and shape presets (circle, rounded square, square, diamond) for editable button controls.
- Provide bundled sprite choices and PNG import for button artwork. Imported images are copied into the app's private files area; do not write into the game installation or ship copyrighted game art.
- Keep the left stick simple: one touch zone, direct relative direction, circular clamping, and a visible movable knob. Do not require a separate calibration step.
- Persist layout and appearance across restarts. Provide a one-action reset to defaults.
- Keep touch controls hidden while a physical controller is connected.

## Editing interaction

A dedicated edit-mode toggle opens a compact editor overlay. In edit mode, dragging a control moves it; dragging a corner/size handle resizes it; selecting a control opens its properties; the Add button creates a normal button or combo; Delete removes the selected custom control. Normal gameplay gestures must not accidentally enter edit mode.

The editor must expose the control's label/input mapping, colour, sprite, shape, size, opacity, and position. Combo construction must use an explicit input picker, not a hidden gesture that is hard to discover.

## Persistence and compatibility

Use a versioned, validated JSON layout in the app-private files directory. Unknown fields and invalid/out-of-range coordinates must be ignored safely; a malformed file must fall back to defaults rather than preventing startup. Keep built-in controls as defaults and store user changes as overrides plus custom controls, so future upstream updates can add or rename defaults without losing the user's custom controls.

## Release and update rules

- Android application ID remains `io.github.gamboozino.ngbrecomp`.
- Upstream 1.0.1 uses versionCode `1001`; this development branch uses `1002` / `1.0.2-touch-dev` for test APKs.
- Every later APK must have a strictly greater versionCode.
- An in-place Android update also requires the same signing certificate as the installed APK. Verify certificate identity before claiming that an APK can overwrite the installed app.
- Do not commit generated game code, disc images, game data, saves, or extracted game artwork. The Android build needs the user's locally generated `src/recomp/gen/*.c` sources, which must be supplied to the build environment without publishing them.


## NGB Touch 1.0.3 — blue editor foundation

The dedicated development line is `feature/ngb-touch-ui-v1.0.3`. Its visual direction is
a restrained dark overlay with a technical-blue accent. The first implementation adds a
compact in-game editor toolbar with one-tap size, opacity, accent, shape and artwork-style
controls, plus add/delete and a clear Done action. These controls are rendered by the
existing native overlay rather than an always-running Android view, so the editor does
not introduce a second UI rendering loop during gameplay.

This is a foundation, not the finished Winlator-style editor. The next implementation
steps are a side-edge floating settings handle, a dedicated properties panel, explicit
combo input selection, arbitrary user-created controls, drag handles for precise resizing,
and private PNG artwork import/preview. Until those pieces land, the current four custom
combo slots and six built-in button-pair mappings remain the limits of combo editing.
