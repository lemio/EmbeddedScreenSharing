# 24-bit color support for the AMOLED boards

**Status:** research only, nothing implemented. Written 2026-08-04 in response to
"the ESP32-S3 AMOLED boards can display 24-bit colours; is that currently the case
for all examples?" - answer at the time: no, everything is 16-bit RGB565 end to end,
for three independent, stacked reasons. This doc records what was actually found in
the code, so a future session doesn't have to re-derive it from scratch.

Scope: the three LilyGo AMOLED boards (T-Display-AMOLED-Lite/SH8501B0,
T-Display-S3 AMOLED/RM67162, T4-S3/RM690B0) and the three examples that target them
(webJPEG, webH264, webRAW). The CYD boards (webJPEG-CYD/webRAW-CYD) are a plain
ILI9341 over standard SPI via TFT_eSPI, a completely different driver stack - not in
scope here unless someone specifically wants CYD 18-bit support investigated too.

## Why it's 16-bit today (confirmed, not assumed)

Three separate layers all currently lock the whole pipeline to RGB565, independently
of each other - fixing one wouldn't be enough on its own:

1. **Panel init sequence** - `src/initSequence.cpp` sets COLMOD (display register
   `0x3A`) to `0x55` (16-bit/pixel) for every panel variant (lines 521, 545, 558).
   The 18-bit (`0x66`) and 24-bit (`0x77`) alternatives are physically present in the
   file as commented-out lines right next to the active one - whoever wrote this
   sequence already knew the option existed and chose 16-bit, but there's no comment
   explaining why (worth checking git blame/history if it exists).

2. **The display library's API is hardcoded to 16-bit.** `LilyGo_Display::pushColors()`
   (`src/LilyGo_Display.h:25-26`) is declared as `virtual void pushColors(uint16_t
   *data, ...)`. The actual implementation in `src/LilyGo_AMOLED.cpp:901-968` hardcodes
   2-bytes-per-pixel arithmetic in the low-level SPI/QSPI transfer sizing itself, not
   just the pointer type:
   - `spiDev->writeBytes((uint8_t *)data, len * sizeof(uint16_t));` (line 907, the
     regular-SPI path)
   - `t.base.length = chunk_size * 16;` (line 937, the QSPI path - 16 *bits* per pixel)
   - The rotation-correction path (`pushColors(x,y,w,h,data)`, used only for the
     1.47" Lite board, the only one with `frameBufferSize` set - see below) copies
     through an intermediate `uint16_t *pBuffer` (`src/LilyGo_AMOLED.h:428`), sized
     via `boards->display.frameBufferSize = SH8501_WIDTH * SH8501_HEIGHT *
     sizeof(uint16_t)` (`src/LilyGo_AMOLED.h:141`).

   None of this is a config flag - it's arithmetic baked into the transfer-sizing
   code. Adding 24-bit support means either a parallel set of 24-bit-aware
   `pushColors()` overloads (safer, more code) or generalizing the existing ones with
   a bytes-per-pixel parameter (less duplication, touches more call sites).

3. **Every example's own rendering pipeline already produces RGB565 before it ever
   reaches pushColors():**
   - **webJPEG**: `JPEGDecoder` (`.pio/libdeps/*/JPEGDecoder`, a wrapper around
     `picojpeg`) allocates its output buffer as `pImage = new uint16_t[...]`
     (`JPEGDecoder.cpp:441`) and every MCU-block decode routine writes directly into
     that `uint16_t*` - the YCbCr→RGB conversion and the packing into 565 happen
     together, fused, inside the library's private decode routines. There is no
     "give me RGB888 instead" option in this library at all; getting 24-bit JPEG
     decode means either forking picojpeg's conversion routines or switching to a
     different decoder entirely (see Open Questions below). Also uses
     `TFT_eSprite spr` as its framebuffer (`webJPEG.cpp:74`), which defaults to
     16-bit color depth (TFT_eSPI sprites default to `setColorDepth(16)`-equivalent
     unless told otherwise) and is cast to `(uint16_t *)` when pushed
     (`webJPEG.cpp:227` etc.) - would also need `spr.setColorDepth(24)` before
     `createSprite()`.
   - **webH264**: fully custom, hand-written conversion in
     `examples/webH264/h264_decode.c` - `yuv_to_rgb565()` (line 58) and
     `i420_to_rgb565()` (line 88) do the YUV→RGB math AND the 565 packing in one
     step, writing into a buffer allocated as `heap_caps_malloc(width * height * 2,
     ...)` (line 156, the `* 2` being bytes-per-pixel at 565). This is the easiest of
     the three to change - it's this repo's own code, not a third-party library, so
     no API-shape constraints. Would need a new `yuv_to_rgb888()`, an
     `i420_to_rgb888()` writing 3 bytes/pixel, and the buffer alloc changed from
     `* 2` to `* 3`.
   - **webRAW**: the browser itself packs RGB565 before ever sending anything
     (`stream.html`'s `(r&0xF8)<<8 | (g&0xFC)<<3 | b>>3`), so the wire format is
     16-bit before it reaches the device at all. 24-bit here means changing the
     browser-side packing to send 3 raw bytes/pixel (or 4, padded) instead, which
     also means the whole strip-size/`MAX_FRAME_SIZE`/bandwidth math this example is
     built around needs re-deriving (see below).

## What it would actually take, by component

None of this is a flag flip - every layer above needs a real code change, and they're
not independent (changing the library API doesn't help until an example actually
sends 24-bit data, and vice versa).

### 1. Confirm the panels can actually do it (do this first)

The commented-out `0x77` COLMOD value being present in the code is evidence someone
*intended* 24-bit to work, not proof the physical panels genuinely resolve 24 distinct
bits per pixel. Some cheap AMOLED/LCD panels accept an 18/24-bit *interface* value in
COLMOD (i.e. they'll take 3 bytes/pixel over the wire) but only have 6-bit-per-channel
analog drive circuitry underneath, dithering the rest - same idea as a "10-bit input,
8-bit+FRC panel" monitor. Before writing any code:

- Check the three datasheets already in this repo (`datasheet/SH8501B0 DataSheet.pdf`,
  `datasheet/RM67162 DataSheet.pdf`, `datasheet/RM690B0 DataSheet_V0.2.pdf`) for their
  actual supported COLMOD values and true panel bit depth (look for "18-bit",
  "24-bit", "RGB666", "RGB888", "interface format" in the pixel format / COLMOD
  register section).
- If any panel is 18-bit-only in practice, decide whether 18-bit (COLMOD `0x66`,
  6-6-6, 262,144 colors) is worth doing even without full 24-bit on that board - still
  a real step up from 16-bit's 5-6-5 (65,536 colors), and touches the same code paths.

### 2. `src/` (LilyGo_AMOLED library fork)

- Change COLMOD from `0x55` to `0x77` (or `0x66`, per board, depending on what step 1
  finds) in `src/initSequence.cpp` for the relevant panel(s).
- Add 24-bit-aware `pushColors()` overloads (or generalize the existing ones with an
  explicit bytes-per-pixel parameter) in `src/LilyGo_AMOLED.cpp`/`.h` - the SPI
  `writeBytes()` length and the QSPI `t.base.length` bit-count both need to stop
  assuming 2 bytes/pixel.
- The 1.47" Lite board's rotation-correction `pBuffer` path also needs a 24-bit
  variant (or a generalized one) - it's currently a `uint16_t*` doing a per-pixel
  copy loop.
- Decide the in-memory layout for 24-bit pixels (packed 3 bytes RGB888, no padding,
  matching what actually goes out over QSPI) and make sure it's consistent between
  whatever produces the framebuffer (TFT_eSprite, or a hand-rolled buffer) and what
  `pushColors()` expects.

### 3. Per-example changes

- **webH264** (easiest - fully custom code, no library constraints): add
  `yuv_to_rgb888()`/`i420_to_rgb888()` next to the existing 565 versions in
  `h264_decode.c`, change the output buffer alloc from `width * height * 2` to
  `width * height * 3`, update `webH264.cpp`'s push call to use the new 24-bit
  `pushColors()`. The status sprite (`statusSpr`, currently explicitly
  `setColorDepth(16)`) can likely stay 16-bit even if the video path goes to 24-bit -
  it's just on-screen text/status, not the streamed content.
- **webJPEG** (hardest - depends on a third-party decoder): JPEGDecoder/picojpeg has
  no RGB888 output mode built in. Either fork its MCU-to-pixel routines to write 3
  bytes/pixel instead of packing into `uint16_t`, or switch decoders entirely - worth
  evaluating `esp_jpeg` (ESP-IDF's own component, may have hardware-assisted decode on
  some chips) or `TJpg_Decoder` (Bodmer's, TFT_eSPI-adjacent) for whether either
  exposes a raw-RGB888 callback mode; neither was checked in this session, this needs
  its own research pass before committing to an approach. Also add
  `spr.setColorDepth(24)` before `spr.createSprite(...)` and update the
  `(uint16_t *)spr.getPointer()` casts.
- **webRAW** (bandwidth-sensitive): change the browser's packing in `stream.html`
  from the 2-byte 565 pack to a 3-byte RGB888 pack (or 4-byte padded, whichever the
  device side ends up wanting - padding trades 33% more bandwidth for simpler/faster
  unpacking, worth benchmarking both). This is a 50% bigger payload before
  compression than 565 - re-check whether `MAX_FRAME_SIZE` (currently 512KB, PSRAM-
  backed, see `webRAW.cpp:151`) and the deflate-compressed frame size still fit
  comfortably within a frame interval at the target fps, since this example's whole
  pitch is being fast *despite* raw pixels by keeping frames small and calls few -
  re-verify against real hardware, not just math, per this repo's own established
  practice (see `learnings.md`).

### 4. Memory headroom (checked - not expected to be a blocker for the AMOLED boards)

All three AMOLED boards have 8MB PSRAM (see README's board table). Worst case
(T4-S3, 600x450 = 270,000 pixels) a full 24-bit framebuffer is 810,000 bytes vs
540,000 at 16-bit - a 270KB increase, trivial against 8MB. PSRAM budget is not
expected to be the constraint here, unlike the memory-starved CYD boards (not in
scope). Actual SPI/QSPI *throughput* (more bytes to clock out per frame) is a more
likely real-world bottleneck than RAM - worth measuring frame push time before/after
on real hardware, not just assuming it scales linearly.

## Suggested order of work

1. Read the three datasheets, confirm real panel bit depth and correct COLMOD value
   per board (§1 above) - cheap to do, and determines whether the rest is even worth
   pursuing on all three boards or just some.
2. Prototype on webH264 only first - it's the one example where the conversion code
   is fully owned by this repo (no third-party library to fight), so it's the fastest
   path to an actual on-screen, real-hardware-verified "yes, 24-bit genuinely looks
   better/different" result before investing in the library-level `pushColors()`
   rework and the harder webJPEG/webRAW changes.
3. Only then touch `src/LilyGo_AMOLED.cpp`'s `pushColors()` for real 24-bit transfer
   support (webH264's prototype can start on 16-bit while this is being built, then
   switch over).
4. webJPEG and webRAW after, in either order - both are more involved than webH264
   for the reasons above (third-party decoder constraint; bandwidth math), so treat
   them as separate follow-up efforts rather than bundling with webH264's.

## Open questions to resolve before/during implementation

- Real per-panel bit depth per datasheet (§1) - unresolved as of this writing, PDFs
  weren't parsed in this research pass.
- Which JPEG decoder (if replacing JPEGDecoder) actually supports RGB888 output on
  ESP32-S3 with acceptable performance - not evaluated yet.
- Whether 18-bit (if that's the real ceiling for one or more panels) is worth doing
  as an intermediate step, or whether it's not different enough from 16-bit to bother.
- Real QSPI throughput impact of 50% more bytes/frame, measured on actual hardware.
