# CPU Burn

A diagnostic tool for the Tanmatsu: it loads both cores of the ESP32-P4 and shows
what that does to the power system and to the temperatures.

| Key | What it does |
|---|---|
| F6 | Start / stop the CPU load |
| F4 / F5 | Load down / up in 5 % steps |
| F3 | Optional external VFD: off / all segments lit |
| F1, ESC | Back to the launcher |

The report is the launcher's **Power information** dialog (battery type, charging
state and currents, battery voltage, remaining charge, power supply, system and
input voltage, SoC temperature) plus the load measured per core, the chip
information and what the BQ25895 charger reports.

The panel is 480x800 and the BSP rotates it, so the usable area is 800x480. That
is wide enough for two columns and too short for one long list: the load and chip
readings go left, the power and temperature readings right. Nothing is dropped
silently — a line that does not fit is truncated with `~` or skipped, and the
footer says how many.

The burn tasks run one per core at priority 1, above the idle tasks and below the
services graceloader and the BSP bring up. Each busy chunk is followed by a one
tick delay: the coprocessor task that delivers keyboard events runs at priority 0
and would otherwise never get the CPU, which would make the load impossible to
switch off. That is also what the load percentage means — the busy share of a
busy-plus-yield cycle, measured per core rather than assumed. Because of that tick
the load tops out at 95 % (190 ms busy, 10 ms yield at the 10 ms tick), which is as
close to 100 % as the scheduler gets while the keyboard keeps working. `Target` is
what the generator aims for, the per-core percentage is what it measured; the
chunk size is learned from the chunks that actually ran, so the two agree.

Keyboard latency follows from the same tick: the priority-0 coprocessor task only
gets the core during the yield window, so at 90 % a key press can take up to
~100 ms to show up.

The BQ25895 has no readable temperature on the Tanmatsu: its TS pin carries no
thermistor but a fixed 10 kΩ / 10 kΩ divider to REGN (R52, R53), so `ADC_TS` is a
constant ~50 % and the NTC cold/hot faults can never fire. What the IC does say
about its own temperature is the thermal shutdown fault, which is shown together
with the TS percentage. The coprocessor reports `ADC_TS` in units of 0.01 %
(2100..7956 for 21.00 %..79.56 %), so the UI divides it by 100 before printing it.

## Optional external VFD

Some units carry the I2C VFD add-on (NE-HCS12SS59T-R1, twelve ASCII characters,
jumpered to address 0x13). It is probed on the CATT bus first and on the internal
bus second, the same probe as SynthMiner's VFD, which this is ported from; a badge
without one just shows `VFD: no display found` and every call here is a no-op.

F3 toggles it between switched off — the default, and how the app leaves it on the
way out — and switched on with all twelve characters showing `8`, the busiest glyph
of the five-by-seven font: 23 of its 35 dots lit, against 21 for `&` and 20 for
`#`, `@` and `B`. That is the most the tube can be asked to light through its text
buffer, which makes it the display-side counterpart of loading both cores: the
pattern draws the most filament and grid current, so the tube can be part of what
the power readings on the right column are measuring. Filament current stays at
the device default of 110 — pushing it higher only shortens the tube's life.

All I2C traffic runs in its own task at the burn tasks' priority, so it never takes
a slice from the load generator; it runs in the yield window every burn cycle
leaves.

## Template project

This project is based on the [Graceloader](https://github.com/nullislandspace/tanmatsu-graceloader) template app.

## 3D: SynthEngine3D

[SynthEngine3D](https://github.com/nullislandspace/synthengine3D) is the 3D engine for
graceloader apps: a software rasteriser (z-buffer and raycast), PPA compositing, meshes,
textures, lighting, audio and UI helpers, with its own `se_run()` main loop.

The engine is **not** shipped with the template, so apps that do not want it are not
carrying it around. What the template does ship is the build wiring, which sits idle until
an app adds the engine. In a new app that wants 3D:

```sh
make engine     # git submodule add -b main git@github.com:nullislandspace/synthengine3D.git synthengine3D
git add .gitmodules synthengine3D && git commit -m "Add SynthEngine3D"
```

`CMakeLists.txt` picks it up by itself: when `synthengine3D/CMakeLists.txt` exists it builds
the engine, propagates its include directory (so app sources can `#include "synthengine3d.h"`)
and folds its objects into `app.so`. When it does not, the link line is exactly the plain
one, which is why every app can keep this template as `upstream` whether it uses 3D or not.

Two things follow from it being a submodule:

* clone such an app with `git clone --recursive`, or run `git submodule update --init` in it;
* `git submodule update --remote synthengine3D` moves it to the newest engine, and an app
  that wants a fixed version pins it (`ENGINE_REF=V2.0 make engine`, or check out the tag
  inside `synthengine3D/` and commit the new pointer).

Engine settings (list caps and the like) are compile definitions that must reach the
`synthengine3d` target, so set them with `add_compile_definitions()` **before**
`add_subdirectory(synthengine3D)` — see the engine's `docs/configuration.md`.

## Automated device tests

`main/testkit/` is a ready-made test loop for an app on real hardware. The host
sends a command over the debug console, the app runs it inside its own frame
loop, reports machine-readable records, and returns to the launcher by itself —
so `make cycle` builds, installs, runs, tests and comes back with a verdict
without anyone touching the badge.

```sh
make cycle       TEST="perf  scene=title secs=20"       # frame rate, phase split, primitive counts
make cycle       TEST="shots scene=title ms=0,1500,4000" # render exact instants, save PNGs + hashes
make testrefs    TEST="shots scene=title ms=0,1500,4000" # store those hashes as the references
make testcompare TEST="shots scene=title ms=0,1500,4000" # compare against them: a regression test
make recover                                             # after a crash or a hang
```

Results land in `results/<UTC>-<test>-<scene>/` as `console.log` + `result.json`;
references live in `tests/refs/manifest.json`. Exit codes: 0 ok, 1 link, 2 crash,
3 the test reported bad, 4 an image mismatch, 5 usage.

### What it is

| File | What |
|---|---|
| `testkit/debugcon.*` | The console listener: `PING`, `RUN <test> k=v`, `EXIT`, `BADGELINK`, read through the USB-serial/JTAG **driver** (graceloader 2.4.0+ exports it). Emits a `READY` record every 2 s while idle, so the host can find the app without sending anything. |
| `testkit/report.*` | The record format: `@@SR-<KIND>@@ <json> @@<crc32>@@`, one line, CRC'd so a line another task interleaved into it is dropped rather than believed. |
| `testkit/devtest.*` | The two tests (`perf`, `shots`) and the runner that drives them. |
| `testkit/profile.*` | Per-phase frame timing (`prof_begin`/`prof_end`), reported in each `PERF` record. The phase names are yours. |
| `testkit/screenshot.*` | Framebuffer → PNG on the SD card, with a deflate *stored* stream so it needs 64 KB of PSRAM rather than a ~130 KB compressor in scarce internal RAM. |
| `testkit/showtime.*` | The clock everything hangs off: real time, or fixed steps of 1/fps, or set outright. |
| `tools/testrun.py` | The host side: connect, identify, refuse a stale build, run, collect, compare, write results. |
| `tools/recover.py` | Get a wedged badge back. |

### Wiring it into an app

1. Add `main/testkit/*.c` to `APP_SOURCES`, and `main` to `APP_INCLUDES` (it is
   there already). Set the app's paths and name while you are there:

   ```cmake
   add_compile_definitions(SCREENSHOT_DIR="/sd/myapp")
   # REPORT_PREFIX="SR" by default; change it only if two apps' logs mix,
   # and pass the same to testrun.py with --prefix.
   ```

2. Tell the kit how to address your content — one struct, five functions:

   ```c
   static devtest_content_t const CONTENT = {
       .select    = level_select,     // play this one from its start; false if unknown
       .duration  = level_duration,   // seconds, <= 0 for endless
       .started   = level_started,    // show time at which it began
       .name      = level_name,       // what is selected now
       .shot_name = level_section,    // sub-section for per-shot stats, or ""
   };
   static devtest_config_t const TEST = {
       .app = "tld.username.myapp", .shot_dir = "/sd/myapp/test", .content = &CONTENT,
   };
   ```

3. Call it from the frame loop:

   ```c
   on_init:    devtest_start(&TEST);
   on_update:  showtime_frame(); devtest_update(); /* then advance your own content */
   on_render:  /* draw the frame */ devtest_after_render(fb, rast_us);
   per second: devtest_period(fps, frame_ms);
   ```

An app without SynthEngine3D compiles the kit with `TESTKIT_NO_ENGINE`: it then
reports timings and heap, and the primitive counts read zero.

### The one precondition

The `shots` test renders *chosen instants*: it sets the clock instead of running
it. That is only meaningful if what you draw is a **pure function of
`showtime_now()`** — same t, same picture. Get that right and a stored hash is a
real regression test, a host-side checker can replay the same instant (see the
engine's `docs/testing.md`), and a video export can render far slower than real
time without changing a frame. An app that accumulates per-frame `dt`, or draws
from unseeded randomness, can still use `perf`, but its shot hashes will wobble
and the references mean nothing.

## License

This software is under the [MIT license](https://opensource.org/license/mit). The MIT license allows others to build upon your work without restrictions while also making sure you retain your attribution.

(C) 2026 Rene Schickbauer
