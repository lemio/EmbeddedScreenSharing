# Learnings

Notes on non-obvious decisions in this repo and what led to them. Written so the
reasoning survives even after the code around it changes.

## PlatformIO project structure

**One shared `src_dir`, not `PLATFORMIO_SRC_DIR` per example.** The repo originally
picked which example to build by setting the `PLATFORMIO_SRC_DIR` environment variable
before calling `pio run`. That only worked when something remembered to set it - CI's
workflow script did, but a plain `pio run -e webH264` from a terminal or from VSCode's
PlatformIO sidebar didn't, silently falling back to the ini's default `src_dir` and
building the wrong example's code under the `webH264` environment. The fix was a single
`src_dir = examples` shared by both environments, with `build_src_filter` scoping
`webJPEG` and an explicit `examples/CMakeLists.txt` scoping `webH264` (see next point).
Lesson: anything that depends on an environment variable being set externally will
eventually run in a context that doesn't set it.

**`build_src_filter` does nothing under the ESP-IDF/CMake builder.** `webH264` builds
Arduino as an ESP-IDF component (`framework = arduino, espidf`) because `esp_h264`
ships as a prebuilt IDF component the plain Arduino/SCons builder can't consume. That
builder uses CMake's own source discovery, which needs every file listed explicitly in
`examples/CMakeLists.txt` - `build_src_filter` is a SCons concept and is silently
ignored. This is also why `webJPEG.ino` had to become `webJPEG.cpp`: PlatformIO's
`.ino`-to-`.cpp` conversion only scans the top level of `src_dir`, not subdirectories,
so once both examples shared one `src_dir`, the `.ino` file nested a level down was
invisible to it - not a CMake issue at all, just a second consequence of the same
restructuring.

**`board_build.partitions` in `platformio.ini`, not just `sdkconfig.defaults`.**
`webH264` needs a large app partition (esp_h264 + WiFi + display libs don't fit in the
board's default). `sdkconfig.defaults` sets `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`
correctly, but PlatformIO's own espressif32 platform independently derives a partition
table from `board_build.partitions` for its own flashing-offset bookkeeping - and that
one wins if the two disagree. Without also setting it in `platformio.ini`, the build
silently used a 1MB app partition instead of the intended 6MB one; flash usage looked
like it was at 99% instead of the true 16%. Caught by chance while checking flash usage
after adding code, not by any error - a wrong partition table doesn't fail the build,
it just quietly caps how much room you have.

**Two example projects sharing `src_dir` means a library can silently duplicate.**
`webH264`'s `platformio.ini` used to list `xinyuan-lilygo/LilyGo-AMOLED-Series` (the
unmodified upstream package) in `lib_deps`, alongside this repo's own modified fork at
`src/`. Under the ESP-IDF builder, each `lib_deps` entry becomes its own component, so
both the upstream package and the local fork got compiled - and the linker picked
whichever one it resolved first, which turned out to be the *upstream* one. A local fix
to `src/LilyGo_AMOLED.cpp` (see below) silently never took effect for `webH264` because
of this. Fixed by compiling `src/` explicitly via `examples/CMakeLists.txt` and
removing the registry package from `lib_deps` entirely. Lesson: if the same class can
be sourced two ways, prefer breaking silently in "used the wrong one" rather than
"duplicate symbol error" - the former is much harder to notice.

## Firmware behavior

**`beginAMOLED_241()`'s default panics without an SD card.** The T4-S3 board's
auto-detecting `begin()` used to call `beginAMOLED_241()` with its default
`disable_sd=false`, which tries to mount an SD card unconditionally; with none
inserted, the failure path hits an uninitialized FreeRTOS queue and panics (boot loop).
`webH264` originally worked around this by skipping auto-detection entirely and calling
`beginAMOLED_241(true)` directly - which meant it only ever worked on the T4-S3, not
the other boards. The actual fix was in the shared library: `begin()`'s T4-S3 branch
now passes `disable_sd=true`. This also fixed the same latent bug in `webJPEG`, which
had been calling plain `begin()` all along and would have hit it too on a T4-S3 with no
SD card - nobody had filed that issue yet.

**One `/boardinfo` endpoint, one shared setup flow, one shared streaming page.**
`webJPEG` and `webH264` used to have separate streaming pages
(`webrtc_stream.html`/`h264_stream.html`) and different on-device WiFi connect screens.
Consolidating them into `examples/stream.html` (which auto-detects which firmware it's
talking to via a `variant` field in `/boardinfo`) and porting `webJPEG`'s WiFi
spinner/QR-code-on-failure flow onto `webH264` meant both examples now behave
identically up to the point where streaming starts - one thing to get right instead of
two, and one thing to test.

**Rotation for sideways-mounted displays happens entirely in the browser, never on the
device.** Both firmwares only ever receive and render a plain landscape image - a
sideways physical mount is handled by rotating the captured content in
`stream.html` before it's sent, via the standard canvas
`translate`+`rotate`+`drawImage` trick (draw into a virtual swapped-dimension area,
then rotate that into the real landscape output size). This keeps both device-side
render paths exactly as they were - no new firmware code, no new wire format, nothing
to break the render-timing work above. The one place this wasn't free: webH264 normally
hands a captured `VideoFrame` straight from `MediaStreamTrackProcessor` to
`VideoEncoder.encode()` with no canvas step at all, specifically because that path's
throughput is already tight (see the whole "H.264 decode performance" section above).
Rotation needs pixels to go through a canvas, and `VideoEncoder.encode()` only accepts
a `VideoFrame`, so a non-zero rotation reconstructs one from the rotated canvas
(`new VideoFrame(canvas, { timestamp: original.timestamp })`) before encoding - real
extra cost, but only paid when rotation is actually in use; the default (0°) path is
untouched.

## Debugging the flasher

The browser flasher stopped letting users set SSID/password after `webH264` was added.
Root cause: `flasher-manifest.yml`'s `examples:` key still said `T-Display-AMOLED`,
which had been the PlatformIO environment name before an earlier rename to `webJPEG`.
The packaging script matches manifest entries to built firmware by exact environment
name; the mismatch meant the lookup silently returned nothing, so the firmware got
packaged with no `variables` (the SSID/password/mDNS fields) at all - no error, just a
flasher card with nothing to fill in. Renaming things has a way of leaving stale string
references in config files that don't get type-checked against anything.

## H.264 decode performance

Video comparing H.264 vs webJPEG



https://github.com/user-attachments/assets/b82c2d7a-fd30-4112-9a69-beb496bad723



**Resolution dominates, not the decoder.** A YouTube demo of the same chip and decoder
running much faster turned out to be decoding at 160x128 versus this example's
600x450 - about 13x fewer pixels, which lines up almost exactly with the measured
decode time difference. Software H.264 decode cost scales with pixel/macroblock count;
there's no hardware decode block on ESP32-S3 (only ESP32-P4 has one), so at a given
resolution the decode time is close to fixed regardless of encoder tuning.

Confirmed with real hardware, not just estimation: the T4-S3 (600x450) and a 1.91"
board (536x240, streaming a mostly-static YouTube animation) measured 215ms and 105ms
average decode time respectively - a 2.04x difference, against a 2.10x difference in
pixel count. The two panels' `pushColors()` display-transfer time scaled less cleanly
(3.19x for 2.10x the pixels), which is more likely differences in the two panels'
interface overhead (RM690B0 vs RM67162) than anything this app controls. See
`examples/webH264/README.md`'s "Measured performance" for the full numbers. (Also
surfaced in passing: `webH264.cpp` explicitly sets `setRotation(0)`, which reported the
1.91" panel as 536x240 landscape - the opposite orientation from the 240x536 portrait
listed as its native resolution. `webJPEG` doesn't set rotation, so the two examples
may report different orientations for the same physical board.)

**Bitrate and keyframes matter too, just less than resolution.** Decode cost also
scales with how much residual data there is per frame - this example's own per-frame
timing log showed a 1038-byte frame decode in 188ms versus 280ms for a 6392-byte frame
at the same resolution. Keyframes are the expensive case within that: every macroblock
is intra-predicted from scratch, with no equivalent of an inter-frame's cheap "skip
macroblock" (copy unchanged regions straight from the reference frame) available. For
screen content specifically - large static areas, small changing regions - lowering
bitrate and switching to variable bitrate mode both let the encoder actually take
advantage of that structure instead of spending a steady bit (and decode) budget
regardless of content. See `examples/webH264/README.md`'s "Tuning bitrate for screen
content" for the practical guidance this produced.

**Limited-range vs. full-range YUV is a classic, easy-to-miss bug.** Streamed video had
washed-out grays instead of true blacks, while on-device status text (drawn directly in
RGB, no YUV involved) looked fine - a strong hint the bug was in the YUV->RGB
conversion, not the display path. H.264 conventionally encodes luma/chroma in
limited/studio range (Y: 16-235) rather than full range (0-255) - unlike JPEG, which
genuinely is full-range by specification, so the same conversion-matrix code that's
correct for the JPEG decoder was wrong here. Confirmed by adding a debug print of the
decoded top-left pixel's color: it read back as `(16,16,16)` for content that should
have been pure black - exactly what you'd get from feeding limited-range `Y=16` through
a conversion that assumes full range. The fix was a small rescale before the existing
(already-correct) conversion matrix, not a different matrix.

**webJPEG's cheap decode doesn't mean a faster overall pipeline.** On the same 1.91"
board, webJPEG measured slower end-to-end than webH264 (~5.4fps vs ~8.2fps) despite JPEG
decode itself being roughly 65x cheaper than H.264 decode (~1.6ms vs ~105ms) - the
opposite of what "JPEG decode is cheap" would suggest on its own. The actual bottleneck
turned out to be how `webJPEG.cpp` pushes pixels to the display: when the incoming
frame matches the display size exactly, it pushes each decoded 16x16 MCU block to the
display individually as the JPEG decoder produces it (510 separate `pushColors()` calls
for a 536x240 frame, ~360us each) rather than assembling a full frame buffer and
pushing once, which is what webH264 always does. Two lessons here: first, decode time
alone doesn't tell you the whole story - measure the full pipeline, not just the part
you assumed was the bottleneck. Second, webJPEG also has no flow control equivalent to
webH264's ack-based pacing (see above); it just drops fully-received frames it doesn't
have time to render (measured: ~70% of received frames dropped in the same session),
wasting browser-side encode and network work for nothing. webH264's ack protocol exists
specifically to avoid that waste.

There's also a structural reason the gap should widen further in webH264's favor on
more realistic (non-flat, detailed) content than this repo has been measured against so
far: webJPEG decodes every frame independently, so a detailed frame costs the same
whether it's the first frame or the ten-thousandth - nothing carries over between
frames. H.264 can reference the previous frame, so once a complex scene has been sent
once (as a keyframe), unchanged regions in subsequent frames decode as cheap "skip"
macroblocks - cost tracks how much *changed*, not how detailed the picture is. This is
the same "skip macroblock" mechanism behind the bitrate-tuning findings above, just
framed as a comparison to webJPEG's lack of any equivalent.

**The "line by line" render effect and the render bottleneck were the same bug.**
webJPEG used to have two special-cased render paths in `drawJPEG()`/`drawMonoJPEG()`:
when the incoming JPEG matched the display resolution exactly, each decoded 16x16 MCU
block was pushed straight to the display as it came off the decoder; otherwise, frames
went through a `TFT_eSprite` buffer and were pushed once. The direct-render path looked
like an optimization (skip the buffer, skip a copy) but was actually the opposite: 510
separate `pushColors()` calls for a 536x240 frame, at ~360us each, versus one bulk push
for the whole frame - see "webJPEG's cheap decode doesn't mean a faster overall
pipeline" above. It also visibly rendered top-to-bottom as blocks landed on screen one
at a time, which is what prompted the question that led here. The fix removed the
special case entirely: every frame now renders into the sprite and reaches the display
in one push, matching webH264's approach, which never had this problem because
`esp_h264`'s decoder only hands back whole frames anyway - it never had the *option* to
push partial blocks. Lesson: a path that avoids an extra buffer copy isn't necessarily
faster if it trades one bulk transfer for many small ones; measure the actual
display-bus cost, not just the memory-copy cost you're trying to avoid.

**Re-measured on real hardware: the drop rate improved a lot, the frame rate barely
did.** Same 1.91" board, same content: render time went from 183ms to 156.6ms average
(~5.4fps to ~5.9fps), while the frame-drop rate roughly halved (~70% to ~32% of
received frames dropped). That gap between "small time improvement" and "big drop-rate
improvement" makes sense once you separate the two: dropped frames happen when a new
frame arrives *while* the device is still rendering the previous one, so shaving even
~15% off render time meaningfully shrinks that window. But the *total* render time
barely moved, which means the original 183ms was never really "510 small SPI/QSPI
bus transactions are slow" - swapping those for 510 `spr.pushImage()` calls into RAM
(same call count, cheaper destination) recovered almost none of it. The real cost is
in making 510 separate calls at all: each one repeats bounds-checking and byte-swap
logic that would only need to happen once per frame if the MCU blocks were assembled
into the sprite's memory directly instead of going through the sprite's public
per-block API 510 times. Lesson: when a "fix" doesn't produce the improvement you
expected, don't assume it failed - check whether you fixed a different problem than
the one you thought you were fixing (here: the visible line-by-line artifact and the
drop rate, not raw decode throughput).

**A sustained-streaming crash surfaced that the render-path fix didn't cause or fix.**
After a period of successful streaming, real hardware testing hit a burst of
`JPEG decode failed!` errors, then a `Malloc failed for 0 bytes!`, then a `task_wdt`
abort on the `async_tcp` FreeRTOS task and a reboot. The shape of it - failures that
start clean and cascade, ending in a zero-size allocation failure - pointed at
internal-heap fragmentation: `webJPEG.cpp`'s WebSocket handler used to `malloc()` a
freshly-sized `wsAssemblyBuffer` for every incoming frame and `free()` it right after,
and different-sized allocations at a sustained rate are a classic way to fragment a
heap even when nothing is technically leaked. `webH264.cpp` already avoided this
(`wsAssemblyBuf`, allocated once at boot from PSRAM, reused for every message - see
its comment for the same reasoning), which is also why it never showed this failure
mode in testing despite doing conceptually the same job. Fixed by porting the same
pattern to webJPEG: `wsAssemblyBuffer`, `wsMonoAssemblyBuffer` and `frameBuffer` are
now fixed-size (`MAX_FRAME_SIZE`, 512KB) PSRAM buffers allocated once in `setup()`,
never freed; oversized frames are dropped (checked against `info->len` before copying
anything) instead of risking an allocation. The one behavior change this requires:
`frameBuffer` used to change *which* allocation it pointed to every frame (ownership
transferred from the WebSocket handler); now it's a fixed address that gets `memcpy`'d
into under the same mutex, which costs a small fixed copy (tens of KB) in exchange for
removing the allocation entirely. Not yet re-verified against a real long streaming
session - the crash was intermittent under sustained load, so absence of a repeat in
a short test wouldn't be strong evidence either way.

**Per-frame logs need an absolute timestamp, not just relative deltas.** Every
`Timing:`/`Frame #N:` line already broke down its own frame into Decode/Render/Push/etc,
but nothing tied that line to wall-clock time - so a serial log full of clean-looking
170ms frames gives no way to tell whether they arrived back-to-back or with multi-second
gaps between them (a stall, a WiFi retry, the runup to the crash above). Reading a
one-shot lastFrameTime figure or a burst of decode failures in isolation looked like
"streaming is working fine, then instantly broke" - the missing dimension was how much
real time separated any two lines. Both examples now prefix every diagnostic line with
device uptime via a `LOGF`/`LOGLN` macro wrapping `Serial.printf`/`millis()` (see
`webJPEG.cpp` and `webH264.cpp`) - cheap (`millis()` is a single counter read) and
applied only to lines that already stood alone (frame timings, connect/disconnect,
drops, fatal errors), not the multi-`Serial.print()` WiFi-connect screen, where a
timestamp would land mid-sentence.

**Once timestamped, webJPEG's real-world frame rate turned out much lower than the
per-frame timings suggested - and the gap was invisible to the device's own
instrumentation.** Real logs at both 5fps and 30fps targets showed frequent 400ms-1.25s
silences between rendered frames, even though each frame's own Decode/Render/Push add
up to only ~150ms - well under budget. The device is idle, not slow, during those
gaps, which is exactly why `Timing:` never caught them: it only starts its clock once
`drawJPEG()`/`drawMonoJPEG()` is called with a complete frame already in hand. Whatever
happens before that (network transit, WebSocket reassembly, or the device simply not
noticing yet) was completely unmeasured.

Leading suspect: `drawJPEG()`'s MCU-copy loop ran ~500 `spr.pushImage()` calls back to
back with no yield, holding `frameMutex` (and the CPU) for the entire ~150ms. AsyncTCP's
own task has no fixed core affinity by default, so it can share a core with `loopTask` -
and a core held solid for 150ms can delay that task long enough to delay TCP ACK
generation. A sender not getting timely ACKs looks exactly like packet loss to a TCP
stack, triggering retransmission backoff - and TCP's minimum RTO (commonly ~200ms,
doubling per retry: 200/400/800/1600ms) lines up closely with the observed gap sizes.
Two changes test and mitigate this without evidence of a different root cause yet:
1. `Timing:`/`Mono Timing:` now include a `Recv=` figure - wall-clock time from the
   first WebSocket fragment of a frame (`info->index==0`, timestamped in the
   `WS_EVT_DATA` handler) to when `drawJPEG()` actually started decoding it. This
   number, not `Total=`, is what will confirm or rule out the CPU-starvation theory:
   if `Recv=` is what balloons during a stall, the delay is upstream of the device
   even noticing a new frame; if `Total=`'s other fields balloon instead, it's
   something in the render path itself.
2. Both MCU-copy loops now call `taskYIELD()` every 64 blocks - lets the scheduler run
   other ready tasks (like AsyncTCP's) without the fixed-delay cost of `delay()`/
   `vTaskDelay()`, which would add real latency 8 times a frame for no benefit if the
   scheduler had nothing else to do.

Not yet confirmed on real hardware - this is a plausible, testable hypothesis backed by
how the ESP32's task scheduling and TCP's RTO backoff both work, not a verified root
cause. The next real log will show whether `Recv=` (not `Total=`) is where the gap time
actually lives, and whether the periodic yield changed anything.

## Adding a board outside the AMOLED lineup (webJPEG-CYD)

**Check the actual decoder library's supported targets before assuming a codec choice
is portable.** Asked to port webH264 to a "Cheap Yellow Display" (ESP32-2432S028R -
plain ESP32, not S3), the real blocker wasn't the obvious one (no PSRAM - see below),
it was structural: `managed_components/espressif__esp_h264/idf_component.yml` lists
`targets: [esp32s3, esp32s31, esp32p4]`, and there's no `esp32/` folder at all under its
prebuilt `sw/libs/` (only `esp32s3`, `esp32s31`, `esp32p4` exist there as prebuilt
`libtinyh264.a`/`libopenh264.a`). Espressif simply never built this component for the
original ESP32 chip. No amount of memory tuning or build-flag rework changes that - it
was worth checking the vendored component's own manifest directly (two minutes) before
spending real implementation time on a path that couldn't work regardless. webJPEG has
no such restriction (`JPEGDecoder` is a portable C library), so that's what
webJPEG-CYD is built on instead.

**No PSRAM changes which architecture is "correct," not just the numbers.**
`examples/webJPEG/webJPEG.cpp`'s AMOLED port deliberately moved *away* from per-MCU
direct-to-display rendering, toward buffering a full frame in a `TFT_eSprite` before
one bulk push (see "webJPEG's cheap decode doesn't mean a faster overall pipeline"
above) - that fix was correct there, backed by real measurement, and shouldn't be
second-guessed for that board. But the CYD has no PSRAM at all, and that same sprite
would be ~150KB - a large fraction of the entire ~320KB internal-SRAM budget once
WiFi/BT/FreeRTOS/Arduino overhead is accounted for. Porting the "improved" architecture
byte-for-byte would have been copying a conclusion without its premise: the earlier fix
was worth ~150KB of PSRAM (abundant, 8MB) to save render time; on a board where that
150KB is a large fraction of all available memory, the trade inverts. webJPEG-CYD goes
back to direct-to-display per-MCU pushes on purpose - the same visible top-to-bottom
render artifact the AMOLED boards fixed, deliberately reintroduced, because the
resource that fix spent (RAM) is the scarce one here and the resource it saved (render
time) is comparatively cheap to give back. Lesson: a performance fix is really "spend
resource X to save resource Y" - re-examine that trade on hardware where X and Y have
different relative scarcity, don't just port the conclusion.

**`-DARDUINO_USB_CDC_ON_BOOT=1` on a plain ESP32 doesn't just do nothing - it deletes
`Serial`.** This repo's shared `[env]` sets that flag because every other board here is
an ESP32-S3 with native USB, where it controls whether `Serial` is the USB-CDC object
or the native UART. A plain ESP32 has no native USB peripheral at all (Serial always
goes over its UART-to-USB bridge chip, unaffected by this flag on real hardware) - but
leaving the flag defined anyway made the Arduino core's conditional compilation skip
defining the global `Serial` object entirely, turning every `Serial.print()` call in
the file into a hard compile error ("'Serial' was not declared in this scope").
`[env:webJPEG-CYD]` undoes it with `-UARDUINO_USB_CDC_ON_BOOT`. Caught immediately by
the first build attempt, not something that needed real hardware to find - a reminder
to actually build a new env early, not just read through the config and assume it's
fine because it "shouldn't matter" for a flag that looks S3-specific.

**Real hardware found four more things no amount of reading the datasheet would have:**

1. **"Free enough in total" isn't "free enough to allocate."** The board's boot-time
   log showed 302,844 bytes free before this example's three fixed frame buffers -
   comfortably more than the 192KB a 64KB `MAX_FRAME_SIZE` would need. That allocation
   *still failed*. The problem was never total free memory, it was the largest single
   *contiguous* block - something in Arduino core/TFT_eSPI init had already carved the
   heap into pieces smaller than 64KB, even this early in `setup()`. 40KB succeeded
   (120KB total, 179,916 bytes free afterward). Lesson: a "free heap" number on its own
   doesn't tell you what a single large `malloc()` can get away with; test the actual
   allocation size on real hardware, don't just check that the sum looks big enough.

2. **A slower upload speed made a CH340 upload *less* reliable, not more.** Intuition
   says "if uploads are failing, slow down" - the opposite was true here. 115200 baud
   reliably died partway through the ~900KB firmware write (8 consecutive real-hardware
   attempts, unaffected by trying a different USB port or a different physical cable -
   both were ruled out one at a time). 460800 completed cleanly, repeatedly, and about
   4x faster. Root cause unconfirmed (a CH340 driver/timing quirk at low baud seems
   likeliest, given cable and port were both eliminated as variables), but the fix was
   simple once tested empirically rather than assumed: `upload_speed = 460800` in
   `[env:webJPEG-CYD]`. Lesson: "make it more conservative" is not a safe default
   troubleshooting move without evidence - it happened to make this specific failure
   mode worse, and the only way to find that out was to actually try the opposite.

3. **`setRotation()` isn't guaranteed to work just because TFT_eSPI compiles for the
   driver.** `tft.setRotation(1)` (landscape) produced portrait-oriented content with a
   corrupted "noise" band at the bottom - a real, board-specific ILI9341/TFT_eSPI
   incompatibility, not a config typo (pin mapping, SPI frequency, and rotation 0 all
   work fine on the same board). The pragmatic fix was to stop asking the panel to
   rotate at all: boot into rotation 0 and use `stream.html`'s browser-side Rotation
   feature (added earlier this session for sideways-mounted displays) to get
   landscape content onto it instead - built for exactly this kind of situation, a
   display that can't or won't rotate itself, even though it was originally motivated
   by AMOLED boards that rotate fine in firmware. **Update, see item 6 below:** the
   width/height mismatch this investigation kept running into at every rotation value
   (0 included) turned out to be a *different* bug than "this driver can't rotate" -
   TFT_eSPI's own internal `_width`/`_height` for this driver ignore build-flag
   overrides entirely, no matter the rotation. Rotation 0 was still the right choice
   (rotation 1 really is broken on this panel), but it wasn't sufficient on its own.

4. **A flicker "obviously" required a framebuffer to fix - it didn't.** The WiFi-connect
   spinner redrew the *entire* screen every 500ms (`fillScreen(BLACK)` then redraw
   everything), which is fine when going through a sprite buffer (the AMOLED boards'
   approach) but produces a visible black flash every tick when drawing directly to the
   panel, which this board does by necessity (see the "no PSRAM" entry above). The fix
   wasn't to add a framebuffer back (defeating the whole point of removing it) - it was
   to stop redrawing the *static* parts (labels, SSID text, the progress bar's outline)
   at all, and only touch the two things that actually change per tick (the spinner
   character, the progress bar's fill), relying on opaque-background text/fills to
   overwrite the previous frame in place. Same lesson as the AMOLED sprite fix, in
   miniature: figure out what actually needs to be cheap to redraw before reaching for
   a bigger structural fix.

5. **A test observation phrased as a shape ("only covers part of the screen") is easy
   to mis-diagnose as a rotation/geometry problem when it's actually a value silently
   not applying at all.** The numbered-grid diagnostic (labeled gridlines instead of a
   plain border, so results could be read out as concrete numbers) was what finally
   separated "this rotation is geometrically wrong" from "this rotation is
   geometrically right but capped at a fixed limit regardless of what's requested" -
   the latter was the real bug (item 6), and no amount of trying different
   `setRotation()` values could have found it, because none of them were the actual
   cause.

6. **`TFT_eSPI(width, height)`'s constructor argument overrides the driver's internal
   state; `-DTFT_WIDTH=`/`-DTFT_HEIGHT=` build flags don't, for this driver.**
   `ILI9341_Defines.h` hardcodes `TFT_WIDTH`/`TFT_HEIGHT` per rotation-swap case
   regardless of any build-flag override, so `-DTFT_WIDTH=320 -DTFT_HEIGHT=240` was a
   silent no-op the whole time (confirmed: changing it produced literally zero change
   in what `tft.width()` reported). The fix that actually worked -
   `TFT_eSPI tft = TFT_eSPI(WIDTH, HEIGHT);` - passes dimensions to the constructor,
   which sets the driver instance's `_width`/`_height` directly, bypassing the
   compile-time logic entirely. Lesson: TFT_eSPI has (at least) two different
   "configure the display size" mechanisms with different scopes, and this driver
   silently prefers its own hardcoded values over the build-flag one - a working
   third-party example ("just add `-DTFT_WIDTH=`") turned out not to transfer to this
   driver/version combination without checking the actual driver source.

7. **A `-D` build flag set to the wrong *kind* of value fails silently, not loudly.**
   `TFT_RGB_ORDER` looked like it should take a symbolic constant (`TFT_BGR`/`TFT_RGB`,
   the names TFT_eSPI itself defines for the resulting MADCTL bits) - so
   `-DTFT_RGB_ORDER=TFT_BGR` looked correct and compiled without any warning. It
   wasn't: `ILI9341_Defines.h` checks `#if (TFT_RGB_ORDER == 1)`, i.e. it wants the
   literal integer `1` for RGB order, anything else (including an undefined
   identifier like `TFT_BGR`, which the preprocessor treats as `0` inside `#if`)
   falls through to the `#else` - which is BGR, the same result as not setting the
   flag at all. Every real-hardware color report ("red shows as blue", "yellow shows
   as light blue", "blue shows as brown" - all consistent with a straight R/B channel
   swap) was the same symptom before and after this "fix," which in hindsight was the
   tell. The actual fix was a one-character change, `-DTFT_RGB_ORDER=1`. Lesson: a
   `-D` flag that compiles clean is not evidence it did anything - `#if`/`#ifdef`
   conditions on flags with non-obvious expected value types (integer vs. symbol vs.
   presence-only) are exactly the kind of bug that survives a successful build and
   only shows up as a confusing runtime symptom that can look like something else
   entirely (this one was first mistaken for a gamma/noise problem).

## webRAW-CYD (raw RGB565 + browser-side deflate, skipping JPEG entirely)

**A `tinfl_decompress_mem_to_mem()` call overflowed the default Arduino loop task
stack - a stack problem that looked, from the crash log alone, like it could have
been anything.** webRAW-CYD decompresses ESP32-ROM-provided deflate streams
(`esp32/rom/miniz.h` - no external library needed, confirmed linking on real
hardware with zero extra dependencies) directly into a per-strip buffer. First real
test crashed instantly on the first received frame: `Stack canary watchpoint
triggered (loopTask)`, inside `tinfl_decompress_mem_to_mem()` itself. The function's
internal `tinfl_decompressor` state - three `tinfl_huff_table`s, each holding a
1024-entry lookup table plus a 288-entry Huffman tree - is ~10.7KB by itself, declared
as a local (stack) variable *inside the ROM function*, not the heap. Arduino's default
`loopTask` stack is 8KB. Fixed with one build flag,
`-DARDUINO_LOOP_STACK_SIZE=32768` (see `main.cpp`'s `ARDUINO_LOOP_STACK_SIZE`/
`CONFIG_ARDUINO_LOOP_STACK_SIZE` `#ifndef` chain for where that's read). Lesson:
a large stack-resident struct inside a library/ROM function you didn't write and
can't see the source of is invisible until it overflows - the crash symptom (a canary
watchpoint on a specific task) was the actual useful clue, not the fact that it
happened inside a decompress call.

**Fewer, bigger display pushes really did win big - confirmed, not just
theorized.** webJPEG-CYD's own real-hardware numbers (see its section above) showed
JPEG's per-MCU-block `pushImage()` calls, not pixel count, dominating render time
(~150-250ms). Once the stack crash above was fixed, webRAW-CYD's decompress+push for
a whole 320x240 frame (10 strips, 10 `pushImage()` calls) measured ~40-44ms on the
same board - a 4-6x improvement, and every strip decompressed cleanly (0 failures).
This wasn't a lucky guess dressed up as a hypothesis after the fact - it was
predicted in the file header comment before the first real-hardware test, from the
JPEG measurements alone, and the prediction held.

**Streams API objects being partly native-backed means "reduce the call rate" and
"the code is correct" can both be true while memory still balloons.** Real-hardware
testing (well, real-*browser* testing - the memory problem was client-side, not on
the ESP32) hit a runaway ~50GB Chrome memory footprint. The proximate cause was an
uncontrolled loop: with no rate limiting, `sendRawFrame()`'s scheduling could fire far
faster than the intended Frame Rate setting (the device's own serial log showed
frames arriving ~7-10ms apart - roughly 100-140/sec, not the handful/sec a "Frame
Rate: 6" setting implies), each call constructing 10 fresh
`CompressionStream`/`ReadableStream` pairs (one per strip) plus a fresh
300KB-ish `ImageData`. None of that is a classic reference leak - nothing was being
retained past its natural scope - but `CompressionStream` and friends carry real
non-JS-heap state (internal buffering, native stream controllers) that V8's garbage
collector doesn't schedule around the same way it does plain JS object pressure, so
allocation rate can outpace reclaim rate even for "correct" code. Three separate
fixes, each addressing a different layer of the same problem: a re-entrancy guard
(`rawSending`) so overlapping executions can never compound; a hard floor
(`RAW_MIN_FRAME_DELAY_MS`) under the computed frame delay so a bad/extreme Frame Rate
input can't collapse toward a near-0ms loop; and cutting incidental per-frame
allocation that had nothing to do with the rate at all - `canvas.width`/`height` were
being reassigned every frame despite never changing (forces a full backing-store
reallocation on every assignment, regardless of whether the value differs), and
`deflateRaw()` was wrapping each strip's output in `new Response(...).arrayBuffer()`
instead of reading `cs.readable` directly, adding one more heavier object per strip
for no benefit. Lesson: when a "just cap the rate" fix doesn't feel sufficient on its
own for an API with real native-side cost, look for allocations that are happening on
every call regardless of rate, not just ones gated by it - the canvas resize matched
that description exactly.

**"Close RGB565 colors look identical on the CYD" turned out not to be a bug anywhere
in this repo, after isolating each stage independently.** Reported symptom: several
distinct-but-close RGB565 values (e.g. `0xf75a` vs `0xefbf`, differing by a handful of
bits per channel - all pale/near-white tones) rendered indistinguishably on the
device, while fully-saturated primaries looked fine. Root-caused by testing each
layer in isolation rather than guessing at the whole pipeline at once: (1) hand-decoded
the reported hex values bit-by-bit to confirm the browser's packing formula
(`(r&0xF8)<<8 | (g&0xFC)<<3 | b>>3`) was mathematically correct - it was; (2) added a
compile-time `COLOR_TEST` diagnostic (`webRAW-CYD.cpp`, guarded by `#define
COLOR_TEST 0/1`) that draws hardcoded RGB565 swatches directly on boot, no WiFi/
streaming involved - first via `tft.fillRect()` (showed a real, if subtle, difference,
ruling out a panel/driver bit-depth ceiling), then rewritten to go through
`tft.pushImage()` from a malloc'd buffer instead, matching `drawRawStrips()`'s actual
call shape exactly (also rendered distinctly, ruling out that code path too); (3) built
a static HTML test page with bands colored to reproduce the exact reported RGB565
values after quantization, shared it through the *real* end-to-end pipeline (actual
`getDisplayMedia()` capture, not a synthetic buffer) - also rendered distinctly. With
packing, decompression+pushImage, and the full real capture pipeline all independently
confirmed correct, the original observation was almost certainly just human
perception: RGB565 has its coarsest quantization (5 bits R, 6 bits G, 5 bits B) right
at the high-luminance/near-white end, and pale, low-saturation content is exactly
where banding is hardest to see past versus genuinely identical. Lesson: when a
reported bug survives contact with "convert conditions to a synthetic test I can run
in isolation," check the pipeline stage-by-stage with the *narrowest* possible
reproduction at each stage (hardcoded values, then real API calls, then real capture)
before assuming the bug is real and hunting for it in code that may be innocent -
three independently-clean isolation tests is strong evidence, not a coincidence.

**Calling `client->text()` unconditionally at the end of every WS_EVT_DATA callback
crashed with a `LoadProhibited` deep inside ESPAsyncWebServer's own mutex lock, on
real hardware, under sustained high-throughput streaming.** Confirmed via a fully
symbolicated backtrace (this toolchain resolves ESP32 addresses to source
lines/inlines out of the box): `AsyncWebSocketClient::text()` ->
`_queueMessage()` -> `std::unique_lock<std::recursive_mutex>::lock()` ->
`pthread_mutex_lock` -> `xQueueSemaphoreTake`, faulting on a garbage address
(`0xad567861` - not a valid ESP32 SRAM address, i.e. freed/stale memory, not a null
pointer) while trying to take `client`'s own `_queue_lock`. The log line immediately
before every occurrence was that same client disconnecting. Root cause (from reading
ESPAsyncWebServer's own source, `AsyncWebSocket.cpp`'s `_onDisconnect()`/
`_handleDataEvent()`): a WS_EVT_DATA callback can still be mid-flight (already
dispatched to application code) for a connection whose disconnect has, by that point,
already been processed elsewhere in the async task's event queue - the object backing
`client` is at best mid-teardown, at worst already freed, by the time our own callback
gets around to calling `client->text("a")` at the very end. Fixed by checking
`client->status() == WS_CONNECTED` immediately before every `client->text()` call
in both webJPEG-CYD.cpp and webRAW-CYD.cpp (both handlers in the latter) -
`status()` is a plain field read, not a queue/mutex operation, so it stays safe to
call even this late; `_onDisconnect()` sets `WS_DISCONNECTED` before touching
anything else. This wasn't reachable in earlier, lower-throughput testing (webJPEG
mode's ~3-6fps) - it took webRAW's much higher message rate (15+ fps, every frame a
fresh WS_EVT_DATA dispatch) to make the disconnect-vs-in-flight-callback race actually
land during a real session. Lesson: an unconditional "send a reply inside the event
callback that just received a message" pattern is only as safe as the framework's
guarantee that the callback's own connection object outlives the callback - that
guarantee doesn't hold here under load, and the failure mode is a hard crash with a
backtrace that (without symbolication) looks like generic heap corruption anywhere in
the WiFi/TCP stack, not an obvious pointer-into-application-code bug.

## Capture resolution and rotation blur (stream.html)

**A blurry *preview* pointed at the capture request, not the canvas/rotation code -
and that separation of symptoms was the fastest way to the real bug.** The `<video>`
preview element renders the raw MediaStream directly; it never passes through
`drawRotatedFrame()` or any other canvas code (only the hidden send-canvas does). So
"even the unrotated preview looks blurry" ruled out the rotation transform as the
*primary* cause before any real hardware/browser testing was needed - it had to be
something upstream of all our drawing code, in the capture itself.

**Requesting `getDisplayMedia`'s `width`/`height` `ideal` constraints set directly to
a device's tiny, unusually-shaped target resolution triggers a real Chrome tab-capture
bug.** All three pipelines (JPEG, H264, RAW) used to request capture at exactly the
target device resolution (e.g. 536x240 - a 2.23:1 aspect ratio, wider-and-shorter than
any normal browser tab). Verbose logging of `track.getSettings()` alongside the
*actual* decoded `video.videoWidth`/`videoHeight` (plus a `resize` event listener and
delayed re-checks, to rule out a transient early frame settling later) showed a
persistent, stable mismatch on real hardware: `getSettings()` confidently reported the
full requested 536x240 the entire time, while the video element actually decoded only
106x240 - not a proportional downscale (which would preserve the 2.23:1 aspect ratio),
but a squashed, effectively-portrait 0.44:1 shape. Ruled out both plausible mundane
causes before concluding it was a browser bug: confirmed via a follow-up question that
the shared tab's window was maximized/full-width (not a small floating window), and
confirmed the shared tab wasn't this same page with DevTools docked into it (which
would have shrunk this page's own viewport and explained a narrow capture
legitimately). With a normal-sized window and unrelated tab content, an extreme
requested aspect ratio was the only variable left, and it was the same variable that
was already known-unusual (this repo's target displays are far wider-and-shorter than
typical screen content). The fix: stop asking Chrome to crop-and-scale into that odd
shape at all. Request a generous, ordinary-aspect-ratio capture instead (1920x1080,
`CAPTURE_IDEAL_WIDTH`/`HEIGHT` in stream.html), and let this page's own canvas step -
already resizing into the exact device resolution via `drawImage()`'s target
width/height regardless of source shape, needed anyway for the rotation transform -
do 100% of the actual resize instead of trusting Chrome's scaler for an aspect ratio
it mishandles. Lesson: when a platform API's own "what did you give me" accessor
(`getSettings()`) and the actual delivered data disagree and *stay* disagreeing (not a
one-time startup race - checked via a `resize` listener plus delayed re-reads, not
just a single reading), stop trusting that accessor for this API/parameter combination
entirely rather than trying to reconcile the two readings.

**`imageSmoothingEnabled` defaults to `true` and blurs *any* transformed
`drawImage()` call, including a pure 90/180/270deg rotation - a real, separate
contributor, just not the dominant one in this investigation.** `ctx.rotate(Math.PI/2)`
doesn't land on exact integer source coordinates in floating point, so even a
same-resolution rotated source gets bilinear-blended with its neighbors instead of
copied pixel-for-pixel. Fixed by setting `ctx.imageSmoothingEnabled = false` in
`drawRotatedFrame()` before any of the rotation cases. This fix is still correct and
still shipped - multiples of 90 degrees should always be an exact nearest-neighbor
copy - it just turned out to be secondary to the capture-resolution bug above for the
specific "very blurry" report that prompted investigating in the first place. Lesson:
two real bugs can contribute to one reported symptom; fixing the first one you find
(and confirming it's real) doesn't mean the report is fully explained - "that is a lot
better" after the *second* fix (the capture resolution one) is what actually confirmed
the investigation was complete, not either fix in isolation.

**A device-side WebSocket protocol error (ESPAsyncWebServer's own low-level frame
parser closing the connection with raw binary bytes as the close reason - Chrome
correctly rejects that as "Received a broken close frame containing invalid UTF-8")
turned out to be compounded by a real bug in this repo's own reconnect logic, not just
the device-side error itself.** First fix (giving webRAW mode the same auto-reconnect
`attemptRawReconnect()` pattern webJPEG mode already had) turned out to be necessary
but not sufficient - the stream still died permanently on this exact error. Root cause
in the reconnect logic itself: `ws.onerror` always fires before `ws.onclose` for an
abnormal closure (which this always is), and `onerror` used to set the shared
`wsConnected = false` itself; by the time `onclose` read that same variable into
`wasConnected` to decide whether to retry, it had *already* been cleared by the
preceding `onerror` - so the reconnect gate read `false` on literally every occurrence
of this specific error, no matter how long the stream had been running successfully
beforehand. Fixed by tracking "did this connection ever successfully open" as a local
variable captured in `connectRawWebSocket()`/`connectJpegWebSocket()`'s own closure
(`wasEverOpen`, set only by `onopen`) instead of reading the shared, order-sensitive
`wsConnected`; also made a reconnect *attempt* that itself fails to establish chain
directly into another attempt (rather than depending on that failed attempt's own
`onclose`, which never had a `wasEverOpen` to work with in the first place). Lesson:
when a reconnect/retry mechanism looks structurally right but a specific failure mode
still doesn't recover, check literally what values are read at the moment of the
recovery decision and in what order they were last written - "the retry logic exists"
and "the retry logic actually fires for this exact error" are different claims, and
event-ordering bugs like this are invisible from reading the retry function alone.

## General

**Comments should describe the code as it is, not the story of how it got there.**
Comments accumulated during heavy debugging tend to narrate the journey ("this used to
break because...", "confirmed via the debug log added earlier...") - useful in the
moment, stale the moment the next change lands. Worth a periodic pass to convert
"why we changed this" into "why this is the way it is," or delete it if the reason no
longer applies.

**A stale comment is worse than no comment.** Several bugs above were adjacent to
comments that were still confidently describing behavior that had already changed
(a function name that was renamed, a dependency that was removed, a workaround for a
bug fixed elsewhere). None of these caused the bugs directly, but they made the code
harder to trust while debugging something else nearby.
