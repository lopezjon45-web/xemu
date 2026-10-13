# xemu on Xbox Series X|S Dev Mode (UWP) - experimental port

**UWP port by [Skitzellez](https://github.com/skitzellez).**

This branch (`uwp`) makes [xemu](https://github.com/xemu-project/xemu), the original Xbox emulator, run as a UWP
app on an Xbox Series X in Dev Mode. It is based on upstream master `478b4f4`. Every change is inside
`#ifdef XEMU_UWP`, except one generic fix in `block/file-win32.c`.

**This code was written by an AI assistant (Claude) working with the repository owner.** QEMU and SDL both
decline AI-generated contributions, so nothing here is meant to be sent upstream. It is published as a fork so
that others can read it, test it, and take ideas from it.

No BIOS, MCPX ROM, hard disk image or game is included. You need your own, as with desktop xemu.

Packaging scripts, the Device Portal tools and the launcher live in the companion repository
[xbox-game-vault](https://github.com/skitzellez/xbox-game-vault) (folder `XemuUWP`).

## Setup on the console

There is no ready-made download yet; build it first (see "Building"). Then:

1. Xbox Series X|S in Dev Mode, Device Portal switched on, a profile signed in.
2. Install `Xemu_x64.appx` through the Device Portal (`https://<console>:11443`, "Add").
3. In Dev Home, open the app's details and set **App type** to **Game**. As "App" it has about 145 MB of
   memory and games will not run.
4. With the Device Portal's file explorer, upload your own files to the app's `LocalState\xemu\xemu` folder:
   `mcpx_1.0.bin`, a BIOS image and `xbox_hdd.qcow2` (see https://xemu.app/docs/required-files/), and an
   `xemu.toml` that names them. A template is `XemuUWP/xemu.toml` in the companion repository; the folder's
   path on the console is `Q:/Users/UserMgr0/AppData/Local/Packages/<package family name>/LocalState/xemu/xemu/`.
5. Games: disc images (XISO or full dumps, `.iso`) on an NTFS USB drive on which app packages have full
   control (`icacls X:\ /grant "*S-1-15-2-1:(OI)(CI)F"`). Either set `dvd_path` in `xemu.toml`, or start
   games from the Game Vault launcher, which passes the disc with a link.
6. Start xemu. `uwp_debug.log` in the same folder shows progress, frame rates and profiler lines.

## Contributing

Test reports, fixes and ideas are welcome: open an issue or a pull request against the `uwp` branch. The
rules (no games or BIOS files, say what you tested, AI-assisted work is fine if disclosed) are in
[CONTRIBUTING.md](https://github.com/skitzellez/xbox-game-vault/blob/main/CONTRIBUTING.md) of the companion
repository. Keep changes inside `#ifdef XEMU_UWP`.

## What works (seen on a Series X, app type "Game")

- Boots the BIOS, the dashboard and retail games; output through Mesa's OpenGL-on-D3D12 (GL 4.6).
- Sound, through an XAudio2 bridge (`ui/xemu-uwp-audio.c`).
- Controller on port 1 (Windows.Gaming.Input through SDL).
- 1080p and 4K output.
- Starting a disc from another app with a link: `xemu:?disc=<escaped path>&launchOnExit=<uri>`.
- Full disc dumps (redump style) play without converting: the game partition is found and mounted at its offset.
- Homebrew used for testing: `nxdk_pgraph_tests`, `xemu-perf-tests`.

## What does not work, or is unfinished

- Movies that are decoded with MMX/SSE in software run at a few frames a second in at least one game. The
  emulated CPU is the limit (same reports exist for desktop xemu, issues #2938, #1266, #1232).
- One racing game stays near 30 fps in races although the renderer is not saturated; see "Open leads".
- Disc swap while xemu is running and returning to the launcher on quit are written but untested.
- There is no way to quit from the controller.
- Debug leftovers are still in: `UWP_MARK` markers (`system/vl.c`, `hw/xbox/xbox.c`,
  `hw/xbox/mcpx/apu/monitor.c`), a listing of `*.iso` files on all drives at start-up (`ui/xemu.c`, it runs
  before the main loop and can delay start-up on a slow drive), `pgraph read/write` trace lines and two lock
  probes.
- Only a handful of games were tried. Expect breakage.

## What had to change to run in a UWP app on the console

| Problem on the console | What this branch does |
| --- | --- |
| No Win32 windows; the app needs a CoreWindow | Entry point is `WinMain` -> `SDL_RunApp`, with a UWP build of SDL3 |
| `VirtualAlloc` cannot give writable + executable memory | TCG code buffer from `VirtualAllocFromApp` / `VirtualProtectFromApp`, resolved at run time (`include/qemu/xemu-uwp.h`, `tcg/region.c`, `util/osdep.c`) |
| `GetDefaultCommConfigA` does not exist (the app would not start) | Skipped (`chardev/char-win.c`) |
| Only one window and no pbuffers in Mesa's UWP build | `gloffscreen` reuses the single window's context |
| SDL audio initialisation deadlocks off the main thread (WASAPI activation never completes) | The APU writes to a device-less `SDL_AudioStream`; a Win32 thread feeds XAudio2 (`xaudio2_9.dll`, loaded at run time) |
| `SDL_CreateThread` fails from MinGW code in an AppContainer | Plain `CreateThread` for the audio feeder |
| DXGI present path is desktop only | Stubbed (`ui/xui/win32-dxgi-present.cc`) |
| stderr is lost | Progress log `uwp_debug.log` in `LocalState\xemu\xemu` |

Generic fix: `block/file-win32.c` did not check the result of `GetDiskFreeSpace`; an uninitialised sector size
led to an assert.

## Performance work (OpenGL on D3D12 specific)

On this GL-on-D3D12 stack a `glReadPixels` surface read-back costs about 2 ms, and games can cause ten a frame.
The changes keep data on the GPU:

- Whole-surface `SRCCOPY` image blits stay on the GPU (`gl/blit.c`, `pgraph_gl_surface_copy_to`).
- A render target may serve as level 0 of a mipmapped texture.
- Surface-to-texture uses `glCopyImageSubData` when formats match, and allocates texture storage once.
- Cube maps whose six faces are render targets are copied face by face on the GPU (`gl/texture.c`).
- Depth surfaces sampled as textures (16 and 24 bit, formats 0x30, 0x2E, 0x12) are copied or drawn on the GPU.
- A depth surface larger than the colour target stays bound instead of being read back and rebuilt.
- `glFlush` instead of `glFinish` at flip.

Measured on the console: one platformer's attract demo went from 20-28 fps to 59-60; another game's demo from
29-30 to 59-60 with read-backs at zero.

`target/i386/ops_sse.h` also has native host versions of the busiest MMX/SSE helpers (`UWP_NATIVE_SSE`). They
are correct but did not change the frame rate of the slow movies: the cost is the helper calls, not the bodies.

### Built-in profiler

Every 3 s `uwp_debug.log` gets a `prof` line: CPU % per thread (vcpu, pfifo, ui), wall % per probe
(`UWP_PROF_BEGIN/END`), nv2a counters per frame, read-backs by cause, and now and then a short event trace
and the hottest guest code blocks. `tools/sum.py` and `tools/sym.py` in the companion repository print it.

## Open leads

- The 30 fps racing game: about ten PGRAPH register accesses per frame from the vcpu wait on the renderer's
  locks (2-5 ms a frame). They are the guest's handler for a PGRAPH interrupt (bit 20) raised once a frame;
  `pgraph_write` takes `pfifo.lock` and `pg->lock` while the pfifo thread holds them.
- Slow software-decoded movies: runs of register-only MMX/SSE instructions could be executed natively as one
  stub per run instead of one helper call per instruction. Not started.
- `gen_enter_mmx` emits nine stores per MMX instruction with hard FPU on; the SSE/MMX helpers are declared
  without `TCG_CALL_NO_RWG`.

## Building

Needs MSYS2 (UCRT64, gcc), Visual Studio 2022 Build Tools with the UWP workload, the SDL3 UWP fork
(`SDL3-uwp`, branch `uwp-3.4.16` plus the local fixes) built with CMake for `WindowsStore`, and Mesa's UWP
build **26.2.2** from XboxEmulationHub/mesa-uwp (`opengl32.dll`, `libgallium_wgl.dll`, `z-1.dll`) plus
`dxil.dll`. Older Mesa UWP builds crash when a GL context is made current off the UI thread.

```
# inside MSYS2 UCRT64; PKG_CONFIG_PATH must reach a sdl3.pc that points at the UWP SDL3.lib
mkdir build-uwp && cd build-uwp
../configure --extra-cflags='-DXBOX=1 -DXEMU_UWP=1 -Wno-error -lIphlpapi -lCrypt32' --target-list=i386-softmmu -Db_lto=true -Dx86_version=3
make -j qemu-system-i386w.exe
```

The executable is then marked `editbin /APPCONTAINER`, packed with its DLLs and a manifest that declares
`codeGeneration`, `broadFileSystemAccess` and `expandedResources`, and signed; `XemuUWP/package.sh` in the
companion repository does that. On the console set the app type to **Game** (about 840 MB instead of 145 MB).

## Licence

Same as xemu / QEMU: GPL version 2. See `COPYING`. The UWP port changes are Copyright (c) 2026 Skitzellez.
Under the GPL you must keep the copyright notices, mark your own changes as changes, and pass on the source
with any copy you distribute. Please keep the credit to Skitzellez and to the xemu project.
