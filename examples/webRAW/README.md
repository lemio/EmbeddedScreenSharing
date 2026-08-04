# webRAW

Streams a browser tab, window, or screen share to a LilyGo T-Display AMOLED board over
WiFi as **raw RGB565 pixels, deflate-compressed in the browser** - no JPEG involved
anywhere in the pipeline. See [webJPEG](../webJPEG) for the board setup this reuses
unchanged (WiFi/QR setup flow, panel auto-detection via `amoled.begin()`, PSRAM buffer
strategy) - this example only swaps the wire protocol and render path. See
[webRAW-CYD](../webRAW-CYD) for the original version of this approach, first proven on
a separate, PSRAM-less board (the "Cheap Yellow Display").

**Status: confirmed end-to-end on real hardware** (1.91" board, 536x240, `boardId`
2 - see "Measured performance"). The two fixes ported from webRAW-CYD (see "Ported
findings" below) weren't just precautions - the upload-speed one turned out to need a
board-specific fix of its own too, see "Troubleshooting".

## Why this exists

[webJPEG](../webJPEG)'s own measurements found JPEG decode/render dominated by
per-call overhead (hundreds of small `pushImage()`/bus calls), not pixel count or
decode complexity. Raw RGB565 has no such per-block cost, and this board's PSRAM
means the whole frame can be decompressed and pushed in one shot rather than split
into pieces - see "How it works". The tradeoff is the same as webRAW-CYD's: a raw
payload is bigger than an equivalent JPEG unless the content compresses well with
deflate, which flat UI/screen-share content usually does.

## How it works

This reuses webRAW-CYD's wire protocol exactly (`stream.html`'s sending code is
shared, unmodified, between both boards) but exercises a different branch of it:

1. **Browser**: identical to webRAW-CYD - captures a frame to a `<canvas>`, converts
   it to RGB565, splits it into `rawStripRows`-tall horizontal strips (from
   `/boardinfo`), and deflate-compresses each strip independently
   (`CompressionStream('deflate-raw')`), concatenated with a 4-byte length prefix each
   into one WebSocket binary message.
2. **Device**: this is where it differs from webRAW-CYD. `/boardinfo` reports
   `stripRows` equal to the *whole panel height*, so the browser's strip loop runs
   exactly once - the "message" is really just one length-prefixed blob covering the
   entire frame. The device decompresses it in a single
   `tinfl_decompress_mem_to_mem()` call (ESP32 ROM's built-in miniz, no external
   library - same as webRAW-CYD) into a PSRAM buffer sized for the whole panel, then
   pushes it to the display in one `amoled.pushColors()` call.

webRAW-CYD splits frames into strips because its board has no PSRAM and can't
allocate a whole decompressed frame (153,600 bytes) as one contiguous block. This
board has PSRAM to spare - up to ~527KB (600x450, the largest panel in the lineup, see
the table below) is a trivial allocation here - so splitting buys nothing and is
skipped. It also isn't knowable at compile time *how* to split evenly: unlike the
CYD's fixed 320x240, this firmware auto-detects the panel at boot (same as webJPEG),
and picking a strip height that evenly divides every possible board's height in the
table below isn't guaranteed to have a clean answer. One strip covering the whole
frame sidesteps that entirely.

## Board support

Same auto-detection as [webJPEG](../webJPEG) - see that README's "Board support"
section for the full explanation. Same caveat applies: developed/read from the
library source, not verified against all rows below on physical hardware.

| Board | Panel | Native resolution |
| ----- | ----- | ------------------ |
| T-Display AMOLED Lite 1.47" | SH8501 | 194x368 |
| T-Display AMOLED 1.91" (QSPI) | RM67162 | 240x536 |
| T-Display AMOLED 1.91" (SPI) | RM67162 | 240x536 |
| T-Display AMOLED 2.41" / T4-S3 | RM690B0 | 600x450 |

## Measured performance

Real numbers from the serial log, 1.91" board (`/boardinfo` confirms
`"name":"1.91 inch","width":536,"height":240,"boardId":2`), 60 seconds of live "Force
WebRAW" screen-share streaming in the default "Immediate" ack mode:

| | Value |
| --- | --- |
| Frames rendered | 886 |
| "Mutex busy" drops | 2,043 (69.8%) - expected under "Immediate" mode, see below |
| Decompress failures | 0 |
| Decompress | avg 14.3ms (9-23ms) |
| Swap (see "Troubleshooting" for why this step exists) | avg 14.1ms (9-21ms) |
| Push (`amoled.pushColors()`) | avg 16.1ms (11-25ms) |
| Total | avg 55.6ms (31-99ms) |
| Free heap over the run | 234,708 -> 234,972 bytes (30s apart) - flat, no leak |
| Crashes | 0 |

The 69.8% drop rate is the same "Immediate" ack-mode signature seen throughout this
project's testing (see webJPEG's and webRAW-CYD's own READMEs): the browser sends
faster than the device can render regardless of Frame Rate, since nothing paces it.

**"Wait for device ack" mode**, measured separately (88.7s, same board, same session
- device uptime confirms it had already been streaming crash-free for 5+ minutes
before this window started):

| | Value |
| --- | --- |
| Frames rendered | 688 |
| "Mutex busy" drops | 121 (15.0%) - down sharply from Immediate mode's 69.8%, since the ack now paces the browser |
| Decompress failures | 0 |
| Achieved frame rate | 7.75fps |
| Recv (network + browser encode, ack-gated) | avg 84.7ms (2-212ms) |
| Decompress / Swap / Push | avg 25.3ms / 14.5ms / 16.3ms |
| Total | avg 140.9ms (37-275ms) |
| Gaps >500ms | 4, all under 650ms - minor jitter, not the multi-second stalls webRAW-CYD's ack-recovery fix targeted |
| Free heap | 234,204 -> 235,108 -> 234,908 bytes (30s apart) - flat |
| Crashes | 0 |

Both runs together confirm the ack-recovery fix carried over cleanly: no multi-second
stalls on this board either, in either ack mode.

**Faster than webJPEG on the same board.** webJPEG's own README measures this same
1.91" panel at ~5.9fps with decode+render+push averaging ~169ms (1.1 + 156.6 + 11.4ms).
webRAW's isolated render pipeline (Decompress+Swap+Push from the Immediate-mode run
above, i.e. with network time excluded the same way webJPEG's per-stage numbers are)
averages ~55.6ms combined - about **3x faster** - and even with real network/ack
round-trip time included ("Wait for device ack" above), sustained throughput is
~7.75fps, noticeably higher than webJPEG's ~5.9fps. Matches "Why this exists" above:
JPEG's cost was never really about where the decoded pixels went, it was making
hundreds of small per-block calls at all - raw mode's one-call-per-frame push sidesteps
that entirely, same finding webRAW-CYD made on the CYD board.

One real disconnect happened mid-capture (WS client left, a new one connected 11.4s
later, during the Immediate-mode run) - not a stall, and no crash or corrupted state
resulted; streaming picked back up cleanly once the new client connected.

**Colors confirmed correct on real hardware** - the byte-swap direction in
`drawRawFrame()` (see "Troubleshooting") was inferred from reading
`LilyGo_AMOLED::pushColors()`'s source before this, unverified by eye; a real-hardware
test (1.91" board) confirmed it's right as written, no swap-direction fix needed.

Raw per-run data behind both tables above (plus webJPEG's and webRAW-CYD's numbers,
with extra derived columns like per-pixel throughput) is in
[`benchmarks/`](../../benchmarks) at the repo root, as CSV.

## Ported findings

Two things webRAW-CYD only found by testing on real hardware, applied here
pre-emptively since both are properties of the shared ROM decompressor and ack
protocol, not anything CYD-specific:

- **`ARDUINO_LOOP_STACK_SIZE=32768`** (see `platformio.ini`'s `[env:webRAW]`):
  `tinfl_decompress_mem_to_mem()`'s internal decompressor state lives on the *caller's*
  stack inside the ROM function, not the heap, and is ~10.7KB by itself - more than
  Arduino's default 8KB `loopTask` stack. Confirmed the ROM header
  (`esp32/rom/miniz.h`) is byte-identical between the plain ESP32 and ESP32-S3 SDKs in
  this repo's toolchain, so the same crash risk applies here.
- **Ack-loss recovery** (`ACK_NUDGE_INTERVAL_MS` in `webRAW.cpp`, `rawStallWatchdog`
  in `stream.html`): under "Wait for device ack" mode, webRAW-CYD's real-hardware
  testing found the device's flow-control ack can occasionally sit delayed in
  AsyncTCP's send queue for seconds under load, and the browser had no way to recover
  from that on its own (see webRAW-CYD's README "Troubleshooting" for the full
  writeup). Both fixes - the device resending its ack if it's seen no data in a
  while, and the browser running an independent watchdog timer instead of relying on
  a call chain that could stall out - are already in this example's code, not
  something to add later if the same symptom shows up here.

## Wire protocol

Identical to [webRAW-CYD's](../webRAW-CYD#wire-protocol) - see that README. The only
difference is this board's `/boardinfo` reports `stripRows` equal to its own height,
so in practice exactly one strip is ever sent per frame.

## Using it

Same flow as [webJPEG](../webJPEG#using-it) - flash, WiFi connects, visit the board's
address, redirected to [stream.html](../stream.html), which auto-detects this
firmware via `/boardinfo`'s `"variant":"raw"` field (or force it manually with the
Mode dropdown's "Force WebRAW"). Screen share only (no webcam) - resolution is fixed
by the auto-detected panel, not user-editable. See stream.html's Ack Mode option for
flow control.

## Flashing

Not on the browser flasher - PlatformIO only:

```bash
pio run -e webRAW --target upload
```

WiFi credentials: same mechanism as webJPEG - set via the browser flasher's normal
flow, or edit the `"|*S*|"`/`"|*P*|"` placeholders directly in `webRAW.cpp`.

## Troubleshooting

- **Upload fails with `Changing baud rate to 921600` / `No serial data received`, or
  `Unable to verify flash chip connection` right after `Stub running...`:** confirmed
  on real hardware - `[env]`'s default 921600 upload speed (documented there as fine
  on this lineup's native-USB-JTAG path in general) failed consistently over this
  particular board's native USB-CDC connection, 4/4 attempts. Already fixed:
  `platformio.ini`'s `[env:webRAW]` overrides `upload_speed` down to 115200. If a
  plain `pio run -e webRAW --target upload` still fails intermittently after that
  (a *different*, one-off symptom - not the same repeatable failure), it was USB-CDC
  stub-handoff flakiness, not a baud problem; retrying, or adding `--no-stub` to a
  manual `esptool.py write_flash` (at offsets `0x0`/`0x8000`/`0x10000` for
  bootloader/partitions/firmware.bin), worked around it here.
- **Wrong/garbled colors:** confirmed correct as written on real hardware (1.91"
  board) - the swap direction doesn't need flipping. If you still see wrong colors on
  a *different* panel: `LilyGo_AMOLED::pushColors()` writes pixel bytes straight to
  the SPI bus with no byte-swap of its own (see `LilyGo_AMOLED.cpp`). webJPEG.cpp gets
  correctly-ordered bytes "for free" because its `TFT_eSprite` has `setSwapBytes(true)`
  set, which swaps as `spr.pushImage()` draws into the sprite; this example
  decompresses straight into a plain buffer with no sprite involved, so
  `drawRawFrame()` swaps each pixel explicitly after decompression instead (its "Swap"
  timing stage) - that's the first thing to check.
- **Blank/garbled/shifted image, or `RAW: decompress failed`/`truncated strip header`
  in the serial log:** points at a length-prefix framing bug - check the browser
  console (F12) too. See webRAW-CYD's equivalent troubleshooting entry.
- **Every frame reported oversized / dropped client-side:** the content isn't
  compressing well enough to fit `maxFrameSize` (512KB) - more likely on the largest
  panel (600x450, ~527KB raw) than the smaller ones, which have far more headroom.
  Either accept webJPEG is the better fit for that content, or see webRAW-CYD's
  equivalent entry for the general tradeoff.
- **`Guru Meditation Error` / stack canary crash inside `drawRawFrame()` /
  `tinfl_decompress_mem_to_mem()`:** should already be prevented by
  `ARDUINO_LOOP_STACK_SIZE=32768` in `platformio.ini`'s `[env:webRAW]` - see "Ported
  findings" above. If you still hit it, raising that further is the fix.
- **Multi-second stalls under "Wait for device ack":** should already be prevented by
  the ack-nudge/watchdog fix - see "Ported findings" above and webRAW-CYD's README for
  the full real-hardware writeup of the original symptom.
- Everything else (stream not starting, mixed-content WebSocket block, board
  detection) - see the root README and [webJPEG's README](../webJPEG); both apply
  unchanged here.
