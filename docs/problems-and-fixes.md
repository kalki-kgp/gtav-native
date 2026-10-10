# Problems we hit and how they were solved

A record of the work from 2026-10-07 to 2026-10-10: taking the playgta5.com browser build of GTA V and making it run as a native
program on an M1 MacBook Air and then an iPhone 17. Each entry says what went wrong, why, what fixed it, and whether the fix is
proven. Open problems are listed at the end.

Where things stand: on the iPhone 17 the game ran for 39 minutes of scripted play at an average of 59.1 fps with no crash or
freeze. On the Mac it runs at about 45-56 fps at 1280x720.

## 1. Getting the engine to run outside a browser

The download contains no source code, only `game.wasm` (the engine compiled to WebAssembly) and JavaScript around it. The route
taken: translate the wasm to C with `wasm2c`, compile that natively, and write a native "host" that does what the JavaScript did.

| # | Problem | Cause | Fix | Status |
|---|---|---|---|---|
| 1.1 | `wasm2c` stopped partway through the engine with "unimplemented" | It has no support for the two instructions threads use to sleep and wake each other (`memory.atomic.wait`, `memory.atomic.notify`) | Patched `wasm2c` to forward them to host functions (`patches/wabt-atomic-wait-notify.patch`); the host implements them as a futex | Fixed |
| 1.2 | The generated C did not compile | `wasm2c` only generates SIMD memory helpers for unshared memories, and this engine's memory is shared | `scripts/gen.sh` rewrites the generated header so the SIMD helpers take the shared memory type | Fixed |
| 1.3 | One file compiled for over 10 minutes using 2.4 GB | It holds a single 227,000-line start-up function the optimizer chokes on | First the whole file was built unoptimized; then, at Krishna's request, only that function and the memory initializer are marked unoptimized, so the audio code in the same file is optimized | Fixed |
| 1.4 | The runtime would have moved the engine's heap when it grew | For a 64-bit memory `wasm2c`'s runtime uses `realloc`, which is fatal while 50 threads hold pointers into it | The host reserves the whole address range once and only raises the size | Fixed |
| 1.5 | 86 functions the engine imports from the browser had no native equivalent | They were JavaScript: threads, files, timers, the game-data reader, input, audio | About 1,000 lines of C in `host/`: one module instance per engine thread, file calls mapped to real folders, the data reader's shared-memory protocol served with `pread` | Fixed; the engine booted on the first run |

## 2. Getting a picture

| # | Problem | Cause | Fix | Status |
|---|---|---|---|---|
| 2.1 | The engine draws nothing itself | It writes Direct3D 11-style commands into a ring buffer; a 2,969-line JavaScript file executed them on WebGPU | Rewrote that file in C++ on Dawn, Google's WebGPU library (`host/gpu.cc`), plus a native window | Fixed; first frames were correct with 0 validation errors |
| 2.2 | Shaders need rewriting before use | The browser renderer edits shader text with regular expressions (merging constant buffers, alpha-test and flat-interpolation variants) | Ported as hand-written text scanning (`host/wgsl.cc`); a test compiles all 4,808 rewritten shaders | Fixed; 0 rejected |
| 2.3 | 45 alpha-test shader variants failed to compile | A few shaders write a single number, not a colour, to the first target | The variant is only made when the target is a four-component colour | Fixed |
| 2.4 | First screenshot was black | It was taken during a 33-second stall while 208 render pipelines compiled | Not a bug; see 3.4 for the stall | Explained |

## 3. Speed

| # | Problem | Cause | Fix | Result |
|---|---|---|---|---|
| 3.1 | 40 fps with the renderer thread at 100% | The engine reads GPU data back about 4 times a frame and the renderer stopped to wait each time: 25 ms per frame | Readbacks made asynchronous, as the browser version does | Waits went to 0 |
| 3.2 | Still about 39 fps | Dawn records every command, then translates it to Metal at submit: 43% of the thread | Moved all submit work to a second thread, with an ordered queue | 39 to 46 fps |
| 3.3 | The second thread did not double the speed | Dawn has one device lock; the recording thread waited on it about 40% of the time | None within Dawn. This is the reason for the Metal plan in `docs/metal-renderer.md` | Open |
| 3.4 | Skipping Dawn's validation | Tried as a cheap win | Gains only 5-10% | Not adopted |
| 3.5 | The engine itself topped out at 67 fps | Every memory access compiled to a volatile atomic access through a reloaded pointer, plus a bounds check | Generated code changed to a constant heap address with no bounds checks, built at -O2 | 67 to about 100 fps; program 624 MB to 134 MB; build 17 min to 6.4 min |
| 3.6 | The heap could not be mapped at the chosen fixed address | macOS on Apple silicon reserves 0x1000000000-0x7000000000 for the GPU | Moved the heap to 0x8000000000 | Fixed |

## 4. Getting it onto the iPhone

| # | Problem | Cause | Fix | Status |
|---|---|---|---|---|
| 4.1 | A test app exited immediately with code 0 | Built with the iOS 27 SDK, an app must use the scene-based lifecycle | Rewrote the app shell with a scene delegate | Fixed |
| 4.2 | Dawn's prebuilt iPhone library has no BC texture support, and the game's textures are BC | Dawn only maps BC formats on macOS, and its own builds target iOS 14, below the 16.4 that BC needs | Built Dawn from source with a two-line patch and a newer deployment target (`patches/dawn-ios-bc.patch`, `tools/build-dawn-ios.sh`) | Fixed; the A19 reports all 11 wanted GPU features |
| 4.3 | An app gets about 3.4 GB of memory; the game used more on the Mac | Default settings | The browser page's low-memory profile (quarter texture memory, no shadows, minimum variety). Later the increased-memory entitlement, which works with a free developer account and roughly doubles the limit | Fixed |
| 4.4 | The host did not compile for iOS, and there is no room for a fixed heap address | `mach_vm.h` is unavailable on iOS; an app can reserve 6 GB of address space but not 8 | iOS uses a variable heap base and a 4.6 GB reservation, with smaller thread stacks | Fixed |
| 4.5 | The game could not open its data files on the phone | The default limit is 256 open files and the engine keeps over a thousand archives open | Raised the limit to 8,192, with a fallback that closes and reopens files | Fixed |
| 4.6 | Copying 20.9 GB to the phone dropped after 10 minutes | The transfer connection closed, leaving 20 empty files | Compared every file's size against the Mac copy and resent the 20 | Fixed; all 5,817 files match |

## 5. Playing it: stutter, crashes and freezes

This was the hardest part and took several wrong turns.

**Stutter.** In the first play session the game froze for a moment every few seconds. The log showed 20-29 new render pipelines
being compiled every 10 seconds, each stopping the game: 50 freezes totalling 42 seconds in two and a half minutes. Pipelines
now compile in the background and anything not ready is skipped for a frame or two. The next session had no stutter.

**Running out of memory.** In the same session free memory fell from 1,000 MB to 390 MB. The entitlement in 4.3 removed the
immediate pressure. A slower growth remains; see the open problems.

**The crash and the freeze.** Two failures kept returning on the phone:
- a crash: the engine reported "out of memory" although it had 127 MB free in one block;
- a freeze: one frame stuck on screen, nothing responding.

What was tried, in order:

| Step | Idea | Outcome |
|---|---|---|
| 1 | The crash is real memory pressure | Wrong: with 3.3 GB free the freeze still happened |
| 2 | The speed change in 3.5 let the compiler skip or repeat reads of memory other threads write | A real hazard, and fixed (every read is now one real read, at no speed cost), but not the cause: both failures came back |
| 3 | Stop guessing: build tools | A scripted player (`GTA_AUTOPLAY=1`) so the failure reproduces without a person; a settings file on the phone so options change without a rebuild; a watchdog that dumps every thread's call stack when frames stop |
| 4 | Narrow it down | Phone with the real renderer: hung every time, between 1.5 and 5 minutes. Phone with the renderer off: 8 minutes clean. Mac with the phone's settings: 9 minutes clean, with and without the renderer |
| 5 | Read the watchdog's dump of a hang | The renderer was idle and healthy. The engine's main thread was waiting for visibility results while every worker thread sat idle: the engine had lost a job. The crash was a worker picking up a garbage job |
| 6 | The engine's job queues assume PC processors | PC (x86) processors guarantee that one core's memory writes become visible to other cores in order. ARM processors do not, and the iPhone's shows it far more than the Mac's |
| 7 | Turn on the processor's PC-compatibility mode | Not available to apps |
| 8 | Give the guarantee in software | Rebuilt the engine so ordinary reads are "acquire" loads and writes are "release" stores (`ORDERED=1`) |

Result of step 8: 39 minutes of scripted play on the phone with no crash, no freeze and no rendering errors, at an average of
59.1 fps. Every earlier build had failed within 5 minutes of the same test. The cost is about 15% of the engine's speed on the
Mac (roughly 100 fps down to 88 for the engine alone).

## 6. Smaller things

- A crash on the Mac from the two renderer threads both touching the blit pipeline at start-up. The pipeline is now handed over
  by value.
- The app could not be launched from the Mac while the phone was locked, which once made a fix look untested when it was simply
  not running.
- File transfers from the phone hung while the game had been running a long time, even after restarting the Mac's device
  service. They worked again seconds after the app was stopped.
- Installing the wasm tools with Homebrew pulled in `openssl@4` and unlinked `openssl@3`.

## 7. Still open

| Problem | What is known |
|---|---|
| Slow memory drift | See section 8. After the fix free memory still fell about 18 MB a minute late in a 17-minute phone session. The host now logs the footprint split into engine heap and everything else every 10 seconds, so the next long session shows which side it is |
| Frame-rate dips on the phone | 60 fps for the first 9 minutes of that session, then stretches at 46-56. Either heavier scenes at the Dawn ceiling or thermal throttling; not yet told apart |
| The phone runs warm | The ordering fix in section 5 costs about 15% more engine CPU. A 30 fps cap or lower resolution would reduce it; neither is in |
| Dawn limits the frame rate on the Mac | See 3.3. The plan is a renderer that talks to Metal directly: `docs/metal-renderer.md` |
| One unexplained trap on the Mac | Once in about 15 launches, in the engine's start-up code, before any of the fixes above. It may have been the ordering problem in section 5, but that is not shown |
| The app expires after 7 days | A free developer account signs for a week; the app has to be reinstalled from the Mac. The game data stays |
| Touch controls are a first draft | They work; nobody has said yet how they feel |

## 8. Memory growth and late objects (2026-10-10)

| # | Problem | Cause | Fix | Status |
|---|---|---|---|---|
| 8.1 | Free memory fell about 72 MB a minute while roaming | The port told the engine it had 4 GB of video memory ("Using 4240958464 bytes video memory"), so the streamer kept loading and never released. `gameResourceMB` does not limit this | `-availablevidmem=1024 -disableExtraMem` in the iPhone app's arguments | Confirmed on the phone: textures flat at about 360 MB and buffers at about 425 MB from minute 4 to 17; drift down to about 18 MB a minute |
| 8.2 | Objects appeared late after loading and in new areas | Each new render pipeline compiles in the background and its draws are skipped meanwhile | Every pipeline recipe is saved to `pipelines.bin` in the user folder and all are compiled at the next launch | Confirmed on the phone: 500 compiled in 1.2 s at launch, 105 new ones and 628 skipped draws in 17 minutes |
| 8.3 | The bind-group cache only emptied itself at 36,000 entries, all at once | Keys hold resource ids that are never reused, so dead entries pile up; it reached 15,751 in 17 minutes | Entries not drawn with for 3,600 frames are dropped in a sweep every 600 frames | Works on the Mac; not yet seen in a long phone session |

