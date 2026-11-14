# Windows build (x86, MSVC)

A native Windows build of the same port: WGL instead of EGL, Win32 threads,
the guest mapped into a 32-bit process exactly as on the other targets. It
runs the attract loop end to end - logos, menus, car select, the story
**cutscenes**, and the race scene with the full 3D world.

This folder is self-contained: it carries its own copy of the runtime
(`hle_*.c`, `guest.c`, `dispatch.c`, ...) with the Windows paths filled in,
plus the small POSIX shims MSVC needs (`win_compat.h`, `pthread.h`,
`unistd.h`, `dirent.h`, `win_stubs.c`). `runtime/`, `linux/` and `orbis/` are
untouched.

## Build

Needs Visual Studio 2022 (Build Tools are enough) with the x86 toolset.

```
windows\build.bat <gen_dir>
```

`<gen_dir>` is the lifter output for your own `id4.elf`: `recomp_funcs_*.c`,
`recomp_funcs_list.h`, `recomp_imports.h`, `recomp_plt.h`,
`recomp_symbols.h`. None of it is in this repo (it is lifted game code).
The result is `windows\id4.exe`.

## Run

```
set LINDBERGH_APP0=C:\path\to\game\tree
windows\id4.exe C:\path\to\game\tree\id4.elf
```

**Install the NVIDIA Cg Toolkit 3.1** (free, from NVIDIA's developer site;
default install folder). The game compiles ~500 Cg shaders at startup and
45 of them - the whole post-process chain (combiner, glare, DOF, blur,
downsample, gamma) - have no precompiled copy on the disc; the cabinet
compiled them live. With the toolkit present, `hle_cg.c` loads its 32-bit
`cg.dll` and compiles **every** shader from the source the game hands over,
with the profile it asks for. Without it the runtime falls back to matching
precompiled `.asm_gl`/`.asm_lbg` files, and the post chain and many
materials render wrong (black cutscenes, black cars).

## What was wrong with the 3D scene

The README's open item - "content renders into FBOs that read back flat" -
had three causes, all fixed here:

1. **Fallback shader stub.** Shaders with no precompiled match got a
   pass-through pair whose fragment half multiplies by `fragment.color`,
   but whose vertex half never wrote `result.color`. An unwritten vertex
   result is undefined; current NVIDIA drivers read it as 0, so every
   fallback post pass - the scene composite and the copy to screen among
   them - output black while the scene itself rendered fine. (`hle_cg.c`;
   the same stub is in upstream lindberghrecomp.)
2. **No compiler for the 45 post-process shaders**, and one precompiled file
   standing in for whole families of `#define` permutations (`car_p.fx`,
   the course shaders), which put constants in the wrong registers: black
   car bodies, a flat dark plane over Lake Akina. Fixed by compiling with
   the real Cg runtime (above). `fp30` requests are compiled as `fp40`,
   since `!!FP1.0` text cannot go through `glProgramStringARB`.
3. **Generic/conventional vertex attribute aliasing.** The engine feeds
   every stream through `glVertexAttribPointerARB`, but many cgc programs
   read `vertex.color`/`vertex.normal`/`vertex.texcoord`. The 2005 NVIDIA
   driver aliased generic attributes 2/3/8-15 onto those; current drivers do
   not. `hle_gl.c` now mirrors those attributes onto the conventional arrays.

Also: the PS5-only `pcarbonDSF` upload skip is opt-in here (NVIDIA compiles
those programs fine; skipping them left car bodies empty).

## Runtime fixes in this copy

A review pass over the hand-written runtime also fixed: `memalign` returning
misaligned pointers and heap splits losing 16-byte alignment; an
out-of-bounds write in the silent SEGAAPI buffer table; `__fixunsdfdi`/
`__fixunssfdi` returning 0; `pthread_self` not matching `pthread_create`'s
handle; a `pthread_create` handle race; 64-bit `lseek64`/`ftello64`;
`fstat`/`__fxstat64` field layout; every exit path reporting code 0; guest
`errno`; unchecked heap carve-outs; `pthread_once`/mutex init races;
sub-millisecond `usleep` busy-spinning; missing `pthread_cancel`/
`pthread_key_delete`; x86 shift flags.

## Knobs (in addition to the main README's)

| var | effect |
|---|---|
| `LINDBERGH_CG_REAL=0` | never load the Cg runtime |
| `LINDBERGH_CG_ALL=0` | use the Cg runtime only for shaders with no precompiled copy |
| `LINDBERGH_CG_DLL=` | explicit path to `cg.dll` (32-bit) |
| `LINDBERGH_NO_ATTR_ALIAS=1` | turn off the vertex attribute aliasing |
| `LINDBERGH_SKIP_CARBON=1` | restore the PS5 `pcarbonDSF` upload skip |
| `LINDBERGH_SHOT=` / `_FRAME` / `_EVERY` | dump presented frames as BMP |
| `LINDBERGH_TRACE_FRAME=` + `LINDBERGH_FBO_DUMP=<dir>` | write every render pass of that frame as a BMP |
| `LINDBERGH_FBO_DUMP_EVERY=` / `_ONLY=,1,3,` | repeat that dump every N frames / only those FBOs |
| `LINDBERGH_FP_SOLID=1..6` | replace fragment programs (solid / texture / uv / colour tests) |
| `LINDBERGH_FAULT_LOG=` | crash log path (default `fault.log`) |

## Known issues

- One cutscene close-up ("So, I just saw your car") shows a black band
  across the face.
- Audio is silent (SEGAAPI is stubbed on this target).
- Only verified on attract mode with an RTX 5060 Ti / driver 617.42.
