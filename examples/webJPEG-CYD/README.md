# webJPEG-CYD

Streams a browser tab, window, or camera to a "Cheap Yellow Display" (CYD) board over
WiFi - same protocol as [webJPEG](../webJPEG): the browser captures video, encodes each
frame as a JPEG, and sends it over a plain `WebSocket`. The ESP32 decodes each JPEG and
pushes it to the display.

This is a **separate port**, not a shared codebase with webJPEG - see
`webJPEG-CYD.cpp`'s file header for why this board needed its own version rather than
just another auto-detected entry in the AMOLED lineup.

**Status: confirmed end-to-end on real hardware** (an actual ESP32-2432S028R) - display
init, WiFi connect/fail flow, frame-buffer allocation, and real JPEG frames actually
arriving and rendering are all verified, including finding and fixing a real
channel-order color bug and a rotation quirk along the way (see "Orientation" and
"Troubleshooting" below, and [learnings.md](../../learnings.md) for the full
real-hardware investigation). Not formally benchmarked the way
[webRAW-CYD](../webRAW-CYD) was (no sustained-fps/drop-rate capture exists for this
firmware specifically) - the ~150-250ms per-frame render figure quoted elsewhere in
this repo (e.g. webRAW-CYD's README) is real, measured on this board, just not written
up here as its own table. If you hit something not covered here, please open an issue
with a serial log from boot.

## Which board is this?

The "CYD" nickname covers several hardware variants sold under different listings.
This example targets the classic/most common one:

| | |
| --- | --- |
| Board | ESP32-2432S028R (a.k.a. "CYD") |
| MCU | Plain ESP32 (Xtensa LX6, dual-core), **no PSRAM** |
| Flash | 4MB |
| Display | 2.8", 240x320, ILI9341 driver, plain SPI (not QSPI) |
| Touch | Resistive (XPT2046) or capacitive (CST820/GT911) depending on variant - **not used by this example** |
| USB | Micro-USB (single port on the classic variant) |

If your board has PSRAM, a different touch controller, or a different USB
configuration, this should still mostly work (touch isn't used at all here) - the one
thing that actually matters for this example is the ILI9341 pin mapping below matching
your board's silkscreen/schematic.

## Why not webH264?

Not a choice - it isn't possible on this chip. Espressif's `esp_h264` component (which
[webH264](../webH264) depends on) ships prebuilt decoder binaries only for
`esp32s3`/`esp32s3`-variant/`esp32p4` (see its `idf_component.yml`'s `targets:` list) -
there is no `esp32/` build of it at all. The CYD's plain ESP32 chip isn't and can't be
one of those targets. See `webJPEG-CYD.cpp`'s file header and
[learnings.md](../../learnings.md) for the full story.

## Why this isn't just webJPEG with a different display driver

Two real hardware differences forced a separate implementation rather than folding this
board into webJPEG's auto-detection:

1. **This board isn't part of `LilyGo_AMOLED`'s lineup at all.** That library only
   knows how to detect and drive LilyGo's own AMOLED panels (I2C probing, QSPI
   init sequences). This board is driven with plain `TFT_eSPI` directly instead - no
   shared code with the `src/` library the other two examples use.
2. **No PSRAM.** webJPEG's AMOLED version renders each frame into a full-frame
   `TFT_eSprite` buffer before one bulk push (see webJPEG's README) - deliberately
   fixed to stop a visible line-by-line render artifact and cut render time. That
   sprite is ~150KB for this board's 240x320, and this board's entire usable RAM
   budget (internal SRAM only, no PSRAM) is roughly that same order of magnitude once
   WiFi/BT/FreeRTOS/Arduino overhead is accounted for. So this example goes back to
   pushing each decoded MCU block straight to the display as it decodes - the same
   direct-render approach the AMOLED boards deliberately moved *away* from. That's not
   a mistake carried over; on a board this memory-constrained, skipping the sprite
   buffer is the right trade, not a regression. Expect the same visible top-to-bottom
   render effect the AMOLED boards used to have, for the same underlying reason.

## Wiring / pin mapping

No wiring needed if you're using the actual CYD board - the pins below are already
connected on the PCB. They're supplied via `platformio.ini`'s `[env:webJPEG-CYD]`
`build_flags` (TFT_eSPI's documented way to configure via build system instead of
editing that library's `User_Setup.h` in place):

| Signal | GPIO |
| ------ | ---- |
| MISO | 12 |
| MOSI | 13 |
| SCLK | 14 |
| CS | 15 |
| DC | 2 |
| RST | tied to EN (not a separate GPIO, `-1` in config) |
| Backlight | 21 (active high) |

If your specific board's schematic differs, edit these `-DTFT_*` build flags to match
rather than touching the `.cpp` file.

## Build size and real-hardware memory

Compile-time (`pio run -e webJPEG-CYD`):

| | Used | Budget |
| --- | --- | --- |
| RAM (static .data/.bss) | 49KB | 320KB |
| Flash | 912KB | 1.31MB (this board's default app partition, out of 4MB total) |

Real hardware, from the boot-time free-heap log (see `setup()` in `webJPEG-CYD.cpp`):
**302,844 bytes free** before this example's three fixed frame buffers are allocated,
**179,916 bytes free** after (at `MAX_FRAME_SIZE = 40KB`, i.e. 120KB reserved).
Confirmed the hard way: a `MAX_FRAME_SIZE` of 64KB (192KB reserved) *failed* to
allocate on this same board despite 302KB being nominally free - not a total-memory
problem but heap fragmentation limiting the largest single contiguous block available.
If you change `MAX_FRAME_SIZE`, watch those two log lines after reflashing rather than
assuming a bigger number is safe just because the total looks like it should fit.

## Flashing

Now listed in the [browser flasher](https://lemio.github.io/EmbeddedScreenSharing/wizard.html)
alongside WebJPEG/WebH264 - pick "WebJPEG Stream Display (CYD)". **Not yet confirmed
working through it on real hardware**: the flasher tool itself
([ESP32-S3-Flasher](https://github.com/lemio/ESP32-S3-Flasher)) documents itself as
built for ESP32-S3 devices, and this board is a plain ESP32 on a CH340 USB-serial
adapter, not S3 native USB. It should work (nothing in that tool's config appeared to
actually reject a different connected chip family), but that's inference from reading
its source, not a confirmed test - if it doesn't connect/flash for you through the
browser, fall back to PlatformIO:

```bash
pio run -e webJPEG-CYD --target upload
```

`platformio.ini` sets this env's `upload_speed` to 460800 - confirmed reliably fast on
real hardware (~15-30s). [env]'s default 921600 fails outright on this board's CH340
USB-serial chip, and this board was also seen, on the same hardware, reliably *failing*
partway through the ~900KB firmware write at 115200 - if you hit upload trouble here,
don't reflexively lower this value, since slower measurably made things worse, not
better, in testing.

Set WiFi credentials one of two ways:

- **Quick/local:** copy `wifi_credentials.h.example` to `wifi_credentials.h` (gitignored)
  in this folder and fill in your real SSID/password. `webJPEG-CYD.cpp` picks it up
  automatically if present (`#if __has_include`). CI (GitHub Actions) never has this
  file, so those builds always ship with the `"|*S*|"`/`"|*P*|"` placeholders below.
- **Direct edit** (matches the other examples' convention, but risks committing real
  credentials if you're not careful): edit the placeholders directly in
  `webJPEG-CYD.cpp`:

```cpp
#define WIFI_SSID "|*S*|"
#define WIFI_PASSWORD "|*P*|"
```

## Orientation

Boots into this panel's **native portrait** orientation (240x320, `tft.setRotation(0)`)
- deliberately not landscape, unlike this repo's other examples. Confirmed on real
hardware: `tft.setRotation(1)` (landscape) does not actually rotate this board's
ILI9341 addressable window the way TFT_eSPI expects - pushing a 320x240-shaped frame at
that rotation showed up as portrait content with a corrupted/"noisy" band where the
mismatched row width wrapped into the wrong scanlines. This looks like a real,
board-specific TFT_eSPI/ILI9341 quirk, not a config mistake - rotation 0 is this
panel's power-on-default orientation, so it's the one most likely to just work.

**For landscape use, don't fight the panel's rotation register - use stream.html's
Rotation option (90° or 270°) instead.** That happens entirely in the browser (see the
root README's Rotation section), so it sidesteps this firmware-level issue completely.
`/boardinfo` reports this board as 240x320 (portrait); pick whichever of 90°/270°
matches your physical mounting.

## Using it

Same flow as webJPEG - see that example's README's "Using it" section; the only
difference is this board has no auto-detected `/boardinfo` size mismatch to worry about
(it's always 240x320) and defaults to portrait rather than landscape - see
"Orientation" above.

## Troubleshooting

- **Won't boot / reboots immediately with "Frame buffer allocation failed!" in the
  serial log:** this board's RAM budget is tight - lower `MAX_FRAME_SIZE` in
  `webJPEG-CYD.cpp` (40KB by default, confirmed working - see "Build size and real
  hardware memory" above) and reflash. The serial log prints free heap before/after
  this allocation at boot; that's the actual number to watch, since it's the largest
  contiguous block that matters, not just the total free heap.
- **Upload fails partway through the firmware write** ("chip stopped responding" or
  similar): confirmed on real hardware to be a `upload_speed` sensitivity on this
  board's CH340 chip, not a bad cable/port/baud-too-high problem - see "Flashing"
  above. Don't lower `upload_speed`; if 460800 itself gives trouble, try 230400 before
  going lower.
- **Blank/garbled screen, but serial log looks normal:** almost certainly a pin
  mapping mismatch - double check the `-DTFT_*` build flags in `platformio.ini`
  against your board's actual schematic, especially if you have a CYD variant other
  than the classic ESP32-2432S028R.
- **Colors look wrong (red/blue swapped - e.g. a red test image showing blue, yellow
  showing as light blue):** already fixed, but documented here in case a future change
  reintroduces it. This is a real channel-order mismatch, confirmed and fixed on real
  hardware via `-DTFT_RGB_ORDER=1` in this env's `build_flags` (already set - check
  it's still there if you see this). It has to be the literal integer `1`, not a
  symbolic name like `TFT_BGR`/`TFT_RGB` - TFT_eSPI's `ILI9341_Defines.h` checks
  `#if (TFT_RGB_ORDER == 1)`, and an undefined identifier there silently evaluates to
  `0`, landing back on the same BGR default as not setting the flag at all (which is
  exactly the bug an earlier attempt here hit - see
  [learnings.md](../../learnings.md)). If colors still look wrong with that flag
  correctly set to `1`, then try `-DTFT_INVERSION_ON=1` instead - a different, rarer
  panel-batch quirk, not the one real hardware actually hit here.
- Everything else (stream not starting, mixed-content WebSocket block, etc.) - see the
  root README's "Why the redirect" section and webJPEG's own Troubleshooting section;
  both apply unchanged here.
