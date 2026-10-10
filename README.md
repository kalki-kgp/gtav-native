# gtav-native

Runs the WebAssembly build of GTA V (the one playgta5.com serves to browsers) as a native program on Apple silicon Macs and
on iPhone, without a browser and without a JIT.

This repository contains no game files. You need your own copy of the browser build: `game.wasm`, its `shaders/` folder and
the `data/` folder (about 21 GB).

## How it works

- `game.wasm` is translated to C with a patched `wasm2c` and compiled to arm64 by clang. Nothing is interpreted or compiled
  at run time, so it works on iOS.
- `host/` replaces the browser around the engine: memory, threads and futexes, files, the game-data reader, input, audio.
- `host/gpu.cc` is the renderer. The engine writes Direct3D 11-style commands into a ring buffer; this executes them on
  [Dawn](https://dawn.googlesource.com/dawn) (WebGPU on Metal).
- `ios/app` is the iPhone app: a thin UIKit shell with touch controls, game controller support and a settings page.

Two things are specific to this port:

- **Memory ordering.** The engine's lock-free job queues assume x86 ordering. On ARM they lose or corrupt jobs, which shows
  up as freezes and crashes after a few minutes. The generated code is built so ordinary loads are acquire loads and stores
  are release stores (`ORDERED=1`, the default).
- **Video memory.** The engine is told it has 1 GB of video memory (`-availablevidmem=1024 -disableExtraMem`); otherwise
  its streamer keeps loading until iOS ends the app.

`docs/problems-and-fixes.md` is the full record of what went wrong and what fixed it. `docs/metal-renderer.md` is the plan
for a renderer that talks to Metal directly.

## Status

On an iPhone 17: about 58 fps average at 1392x640 over a 17-minute session, sandbox and story mode both start. On an M1
MacBook Air: 45-56 fps at 1280x720. Controller support, the settings page and background pause are new and lightly tested.

## Building

Requirements: an Apple silicon Mac, Xcode, `cmake`, `ninja`, `xcodegen`, Python 3.

1. **wasm2c.** Clone [wabt](https://github.com/WebAssembly/wabt) into `tools/wabt` (built against commit `b835f1ce`), apply
   `patches/wabt-atomic-wait-notify.patch`, and build it so `tools/wabt/build/wasm2c` exists.
2. **Generate the engine code.** `scripts/gen.sh /path/to/game.wasm` writes about 1 GB of C into `gen/`.
3. **Dawn.**
   - Mac: unpack Dawn's prebuilt Apple package and headers for commit `b73d662f` into `tools/dawn` (the `Makefile` has the
     exact paths).
   - iPhone: clone [Dawn](https://github.com/google/dawn) at the same commit into `tools/dawn-src`, apply
     `patches/dawn-ios-bc.patch` (BC texture formats on iOS), and run `tools/build-dawn-ios.sh`.
4. **Mac:** `make`, then `./run.sh` with `GTA_DATA` pointing at the `data/` folder. `GTA_MODE=story|sandbox` picks the start.
5. **iPhone:**
   - `make PLATFORM=ios` builds `build/ios/libgta.a`.
   - In `ios/app`: copy `Local.xcconfig.example` to `Local.xcconfig` with your team id, add your own `icon-1024.png` to
     `App/Assets.xcassets/AppIcon.appiconset/`, run `xcodegen generate`, and build the `GTA` scheme.
   - Copy the game's `data/` folder to the app's `Documents/data` and the shaders to `Documents/shaders`.

A free Apple developer account works, including the increased-memory entitlement the app needs; the app then has to be
reinstalled every 7 days.

## Layout

| Path | What |
|---|---|
| `host/` | The native host and the renderer |
| `ios/app` | The iPhone app |
| `ios/probe` | A small test app used to check GPU features on the phone |
| `scripts/` | `gen.sh` (wasm to C), `params.py` (lists the engine's command-line options), `phone-watch.sh` |
| `patches/` | The wabt and Dawn patches |
| `tests/` | Checks that every rewritten shader compiles |
| `docs/` | Problems and fixes; the Metal renderer plan |
