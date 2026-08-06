# webRAW-CYD

Streams a browser tab or screen share to a "Cheap Yellow Display" (CYD) board over
WiFi as **raw RGB565 pixels, deflate-compressed in the browser** - no JPEG involved
anywhere in the pipeline. See [webJPEG-CYD](../webJPEG-CYD) for the board itself
(pin mapping, no-PSRAM constraints, WiFi/QR setup flow, ack protocol) - this example
reuses all of that unchanged and only swaps the wire protocol and render path.

**Status: confirmed end-to-end on real hardware**, including a real crash and fix along
the way, and a sustained ~114s "Wait for device ack" run with zero multi-second
stalls after the ack-recovery fix described under "Troubleshooting" - see "Measured
performance."

## Why this exists

[webJPEG-CYD](../webJPEG-CYD)'s own real-hardware measurements
([learnings.md](../../learnings.md)) found that render time there is dominated by
*how many* `tft.pushImage()` calls happen per frame (hundreds, one per 8x8/16x16 JPEG
MCU block), not by pixel count or decode complexity. Raw RGB565 pixels have no
block-size constraint, so this example pushes whole horizontal strips - an order of
magnitude fewer calls for the same frame - and skips JPEG's lossy compression
entirely, so there's no ringing/blocking artifacts (relevant for the "gamma/noise"
kind of visual complaints JPEG mode has had). The cost: a raw payload is bigger than
an equivalent JPEG unless the content compresses very well with deflate - which flat
UI/screen-share content (repeated colors, whitespace, sharp uniform edges) usually
does, but a busy photograph or video won't.

## How it works

1. **Browser**: captures a frame to a `<canvas>` (same rotation/capture machinery as
   the other examples), converts it to a flat RGB565 pixel buffer, splits it into
   fixed-height horizontal strips, and deflate-compresses **each strip
   independently** (`CompressionStream('deflate-raw')` - no zlib/gzip header, just a
   raw deflate stream per strip). The strips are concatenated with a 4-byte
   little-endian length prefix each and sent as one WebSocket binary message - same
   "one message per frame, one ack per message" shape as webJPEG's protocol.
2. **Device**: reassembles the message into a fixed-size buffer (same pattern as
   webJPEG-CYD's `MAX_FRAME_SIZE`), then for each strip: decompresses it with the
   ESP32 ROM's built-in `tinfl_decompress_mem_to_mem()` (no external library - see
   `webRAW-CYD.cpp`'s file header) into a small reusable buffer, and pushes it
   straight to the display with one `tft.pushImage()` call before moving to the next
   strip.

Splitting into strips isn't just for fewer/bigger pushes - it's a hard RAM
requirement. A full 320x240 RGB565 frame is 153,600 bytes, far more than this
no-PSRAM board can allocate as one contiguous block (webJPEG-CYD's real-hardware
testing found 64KB already fails - see its learnings.md entries). Strips keep every
allocation small and proven-safe instead.

## Wire protocol

`/ws-raw` WebSocket, binary messages. Each message is `STRIPS_PER_FRAME` (device
reports this as `stripRows` in `/boardinfo`; `HEIGHT / stripRows` strips) entries of:

```
[4 bytes: compressed strip length, little-endian] [that many bytes: raw deflate stream]
```

Each strip decompresses to exactly `WIDTH * stripRows * 2` bytes of RGB565 pixels
(`(r&0xF8)<<8 | (g&0xFC)<<3 | b>>3` per pixel, matching what TFT_eSPI/JPEGDecoder
already produce in the JPEG examples - no special-casing needed on the device side for
color format). `/boardinfo` additionally reports `maxFrameSize` - the cap on the whole
compressed message; `stream.html` checks against this before sending, same as JPEG
mode, and drops (doesn't send) anything over it rather than wasting the encode+network
cost on a frame the device would only reject.

## Measured performance

Real numbers from the serial log on the same 320x240 CYD board used throughout
webJPEG-CYD's own measurements:

| | Value |
| --- | --- |
| Free heap before buffer allocation | 280,920 bytes |
| Free heap after buffer allocation (2x48KB + 15KB strip buffer) | 167,208 bytes |
| Decompress + push, whole frame (10/10 strips) | ~40-52ms |
| Strip decompress failures observed | 0 |

For comparison, webJPEG-CYD's equivalent render step measured ~150-250ms on the same
board/resolution - roughly 3-6x slower, consistent with "Why this exists" above
(hundreds of small `pushImage()` calls vs. 10 large ones).

**Sustained "Wait for device ack" run (113.85s live capture, after the ack-nudge fix
below):**

| | Value |
| --- | --- |
| Frames rendered | 1,451 |
| Achieved frame rate | 12.74 fps |
| Gaps >150ms between frames | **0** |
| "Mutex busy" drops | 104 (6.7%) - see note below, not a correctness issue |
| Decompress failures | 0 |
| Free heap over the run | 87,968 -> 84,808 -> 84,652 -> 88,176 bytes (30s apart) - drops fast early, then plateaus/recovers rather than continuing to decline |

The "Mutex busy" drops are expected and harmless: the device sends its flow-control
ack the moment a WS message finishes *reassembling*, slightly before `loop()`
finishes *rendering* it (see `webRAW-CYD.cpp`'s `WS_EVT_DATA` handler - the
`client->text("a")` call is unconditional, outside the mutex `if`/`else`, specifically
so a dropped frame never stalls "wait" mode). Occasionally the next frame's data
arrives while the previous one is still mid-render and gets dropped instead of
queued - no corruption, no effect on later frames (every *rendered* frame still
reports 10/10 strips), just a small throughput tax. Not worth chasing unless it
starts limiting real usage.

Raw per-run data behind these numbers (plus webJPEG's and webRAW's, with extra
derived columns like per-pixel throughput) is in
[`benchmarks/`](../../benchmarks) at the repo root, as CSV.

## Using it

Same setup flow as webJPEG-CYD (flash, WiFi connects, visit the board's address,
redirected to [stream.html](../stream.html)) - it auto-detects this firmware via
`/boardinfo`'s `"variant":"raw"` field, or force it manually with stream.html's
Mode dropdown ("Force WebRAW"). Screen share only (no webcam option, matching
webH264's approach) - Resolution is fixed by the device's decode buffers, not
user-editable. See stream.html's Ack Mode option (shared naming/pattern with
webJPEG's) for flow control.

## Flashing

Screen Rotation (0°/180°) and Invert Colors options are available in the browser
flasher, same convention and same underlying reason as webJPEG-CYD's - see that
example's README's "Orientation" and "Troubleshooting" sections for the full
explanation (`rotationStr`/`invertStr` in `webRAW-CYD.cpp` if flashing via
PlatformIO instead).

Now listed in the [browser flasher](https://lemio.github.io/EmbeddedScreenSharing/wizard.html)
as "WebRAW Stream Display (CYD)" - same not-yet-hardware-confirmed caveat as
webJPEG-CYD's README (the flasher tool itself documents itself as ESP32-S3-only; this
board is a plain ESP32 over CH340). Fall back to PlatformIO if it doesn't work:

```bash
pio run -e webRAW-CYD --target upload
```

WiFi credentials: same `wifi_credentials.h` mechanism as webJPEG-CYD (gitignored, own
copy in this directory - copy `wifi_credentials.h.example` and fill in real values),
or edit the `"|*S*|"`/`"|*P*|"` placeholders directly in `webRAW-CYD.cpp`.

## Troubleshooting

- **`Guru Meditation Error: ... Stack canary watchpoint triggered (loopTask)`,
  crashing inside `drawRawStrips()`/`tinfl_decompress_mem_to_mem()`:** already fixed
  in `platformio.ini`'s `[env:webRAW-CYD]` (`-DARDUINO_LOOP_STACK_SIZE=32768`) - hit
  on real hardware during initial testing and documented here in case a future change
  reintroduces it. `tinfl_decompress_mem_to_mem()`'s internal decompressor state is
  declared on the *caller's* stack inside the ROM function (not the heap) and is
  ~10.7KB by itself (three `tinfl_huff_table`s, each with a 1024-entry lookup table
  plus a 288-entry Huffman tree) - comfortably more than Arduino's default 8KB
  `loopTask` stack. If you see this after changing `STRIP_ROWS` or otherwise touching
  the decompress path, raising `ARDUINO_LOOP_STACK_SIZE` further is the fix, not
  shrinking buffers. See learnings.md.
- **Won't boot / "Buffer allocation failed!" in the serial log:** lower
  `MAX_FRAME_SIZE` and/or `STRIP_ROWS` in `webRAW-CYD.cpp` and reflash - the serial
  log prints free heap before/after allocation, same diagnostic pattern as
  webJPEG-CYD. See that example's learnings.md entries for why "total free" isn't the
  same as "largest single allocation you can make."
- **Blank/garbled/shifted image:** check the browser console (F12) for
  `RAW: strip N/M decompress failed` or `truncated strip header` messages from the
  device's serial log, which would point at a length-prefix framing bug; or check that
  colors look scrambled in a pattern matching a channel-order issue (see webJPEG-CYD's
  `TFT_RGB_ORDER` learnings.md entry - this example shares that fix already, but worth
  ruling out first if geometry looks right but colors don't). Not observed in initial
  real-hardware testing (0 strip decompress failures - see "Measured performance"
  above), but that test used simple/mostly-static screen content, not a stress case.
- **Every frame reported oversized / dropped client-side:** the content isn't
  compressing well enough to fit `maxFrameSize` (48KB by default) - expected for
  busy/photographic content, less expected for flat UI content. Either raise
  `MAX_FRAME_SIZE` in `webRAW-CYD.cpp` (mind the RAM budget - see above) or accept
  JPEG mode is the better fit for that content.
- **Lots of "Mutex busy - frame dropped" in the serial log:** expected under Ack
  Mode "Immediate" if the browser can compress+send faster than the device's ~40ms
  render time (see "Measured performance") - switch to "Wait for device ack" to let
  the device pace the browser instead of flooding it. A low rate of these (single-digit
  percent) is also normal under "Wait for device ack" itself - see "Measured
  performance" for why - and isn't something to fix.
- **Multi-second stalls under "Wait for device ack" (streaming pauses for seconds at
  a time, then resumes on its own):** already fixed, but documented here in case it
  reappears. Root cause, found via real-hardware serial capture: the device's
  `client->text("a")` ack can occasionally sit in AsyncTCP's send queue for seconds
  under load before actually reaching the browser (visible as free heap dropping in
  step with frames received - the queue itself costs memory while it's backed up).
  Since the browser only sends its next frame after receiving that ack in "wait"
  mode, a delayed ack meant a delayed frame - and the browser's own
  `RAW_ACK_STALL_TIMEOUT_MS` (3000ms) retry-after-timeout logic in `stream.html` was
  dead code: nothing but the ack handler itself ever called `sendRawFrame()` again,
  so if the ack was slow, nothing was left to trigger that timeout check at all.
  Fixed two ways: (1) `stream.html` now runs an independent `rawStallWatchdog`
  (`setInterval`, 500ms) that actually invokes the stall check on a timer instead of
  relying on a call chain that could stall out; (2) `webRAW-CYD.cpp`'s `loop()`
  proactively resends the ack (`ACK_NUDGE_INTERVAL_MS`, 2000ms) if it hasn't seen any
  new data in a while, on the chance the original ack itself was the thing that got
  lost/delayed - 2000ms is intentionally under the browser's 3000ms so a legitimate
  device-side resend normally resolves the stall before the browser has to force a
  blind retry. Confirmed on real hardware: zero gaps >150ms across a subsequent
  113.85s "wait" mode run (see "Measured performance") vs. four stalls
  (2.1s/8.3s/4.8s/6.0s, ~48% dead time) in a prior 44.2s run before the fix.
- Everything else (stream not starting, mixed-content WebSocket block, upload
  reliability) - see the root README and webJPEG-CYD's README; both apply unchanged
  here.
