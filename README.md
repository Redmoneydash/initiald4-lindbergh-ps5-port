# Initial D Arcade Stage 4 — native PS5 + Linux port (WIP)

A work-in-progress port of a Sega Lindbergh x86 arcade title to run **natively
as a managed PS5 application** — no external compatibility layer — built on top
of [sp00nznet/lindberghrecomp](https://github.com/sp00nznet/letsgojungle-lindbergh-recomp)
(see also `letsgojungle-lindbergh-recomp`). All credit for the static
recomp/lift toolkit goes to **sp00nz**; this repo only contains our platform
layer and runtime patches, no game code or assets (same policy as upstream).

## Status (verified)

- **PS5 (Orbis)**: boots natively, runs the attract loop — logos, UI text,
  title, versus/loading screens all render. **Input works** (JVS injection:
  coin/start/driving controls). **Sound works**: full software mixer driving
  `sceAudioOut` (48 kHz stereo S16), ADX/CRI streams verified byte-exact.
- **Linux desktop**: windowed EGL/GL build — same runtime, keyboard input,
  working audio via ALSA (`pulse`/`pipewire`/HDMI fallback). Used as the
  bring-up/debug target since it reproduces PS5 behavior 1:1.
- **Windows desktop** (`windows/`): native x86/MSVC build with WGL. Runs the
  attract loop **including the 3D scene** - story cutscenes, car select and
  races render fully. The scene-pass problem below turned out to be the
  post-process shaders, not the FBOs; see [windows/README.md](windows/README.md)
  for the cause and fix (needs the NVIDIA Cg Toolkit 3.1 installed).
- **Not yet** (PS5/Linux): the 3D scene pass. Scene geometry submits correctly
  (`glDrawRangeElements`, VBOs, real ARB vertex programs with matrices landing
  in `program.env`), but content renders into FBOs that read back flat —
  an FBO→composite path issue still under investigation. `LINDBERGH_NO_FBO=1`
  redirects everything to the default framebuffer and the world draws, so the
  gap is in the offscreen/composite path, not geometry or shaders.

## Layout

- `runtime/` — our `src/runtime/` changes vs upstream `f5dc1b4`
  (GL/ARB-program shim layer, libc, sega/syslib, window, overrides,
  crash diagnostics, Sofdec MMX-helper reimplementations).
- `patches/lindberghrecomp-wip.patch` — the same work as a diff you can apply
  to a fresh clone of upstream.
- `linux/` — desktop backend: X11/EGL host, ALSA `libsegaapi` port
  (`libsegaapi_linux.c`), launcher, CMake fragment. Audio is opt-in:
  `LINDBERGH_AUDIO=1`, ALSA device selectable via `LINDBERGH_ALSA_DEV`.
- `orbis/` — the PS5 platform layer: CRT startup, emutls, heap, pthreads,
  `libsegaapi` mixer on `sceAudioOut`, diagnostics (fault.log/startup.log),
  Makefile for the managed app target.
- `windows/` — native Windows build: a self-contained copy of the runtime
  with the Windows paths, MSVC shims, `build.bat`, and the shader/render
  fixes that make the 3D scene draw. See its README.

## Useful knobs (runtime env)

| var | effect |
|---|---|
| `LINDBERGH_WINDOW=1` | open an X11 window instead of headless |
| `LINDBERGH_AUDIO=1` | enable the real SEGAAPI backend (off = silent stubs) |
| `LINDBERGH_ALSA_DEV=` | force an ALSA device name |
| `LINDBERGH_APP0=` | path to the game's data dir (`app0` mount) |
| `LINDBERGH_NO_FBO=1` | bypass the FBO pipeline (debug) |
| `LINDBERGH_FRAME_DUMP=` | dump framebuffer each swap |
| `LINDBERGH_WATCH=` | comma-separated guest VAs → hit counters |

## Legal

No game files are included or needed by the port code itself. You supply your
own dumped game image/data. MIT (matching upstream); runtime changes are
derivative work of sp00nz's lindberghrecomp.
