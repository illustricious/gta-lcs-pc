# Roadmap

Status as of 13 November 2026. Items are in priority order.

## 1. Menu

- Original button glyphs (A/B/X/Y and keycaps) in the hint line instead of text. The hints already switch between keyboard and controller (`plat.pad_input`).
- Native-style sliders for values such as FPS and draw distance, instead of `< 60 >`.
- Extend the host menu style to the remaining native leaf pages.
- Mouse: right click = Back, and hover on native pages.
- A confirmation prompt before Exit Game.
- Pause map: keyboard zoom (+/-), WASD panning, and a legend toggle.

## 2. Graphics

- FSR 1.0 (EASU + RCAS) as a final pass on the scene framebuffer (`rt/present.c`), tied to the internal resolution.
- Sharpening (CAS), FXAA/SMAA, 16x anisotropic filtering.
- Draw distance beyond the current conservative scaling. This needs building LOD and sector streaming to grow together without pop-in, for example by raising `dvStreamerCreateNum*PerFrame`.
- Later: SSAO and colour grading, if a readable depth buffer is available.
- Not realistic: FSR 2/3, DLSS, real ray tracing. The engine provides no motion vectors and uses an old GLES2 renderer.

## 3. Stability

- Automated regression scripts for New/Load/Delete/Settings/Controls with `--press`/`--mpress`, plus crash checks in the log.
- C++ exception unwinding and `setjmp`/`longjmp`.
- The intro video.
- Vehicle smoke at high FPS.

## 4. Distribution

- Test the phone (ADB) import and the radio OBB path on real devices.
- Signed installer builds.
