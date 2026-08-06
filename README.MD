<h1 align = "center">EmbeddedScreenSharing</h1>
<p align="center"><i>Use an ESP32 (or ESP32-S3) display as an external monitor for prototyping</i></p>

<p align="center">
  <a href="https://lemio.github.io/EmbeddedScreenSharing/wizard.html"><strong>Flash it from your browser →</strong></a>
  <br><sub>No PlatformIO install needed - just Chrome or Edge and a USB cable</sub>
</p>

[![PlatformIO CI](https://github.com/lemio/EmbeddedScreenSharing/actions/workflows/platformio.yml/badge.svg)](https://github.com/lemio/EmbeddedScreenSharing/actions/workflows/platformio.yml)

## What is this?

Point your browser at anything - a tab, a Figma mirror, a screen recording - and it
shows up live on a physical display over WiFi. No reflashing: change what's on screen
and the display just updates.

## What do I need?

- One of the [supported boards](#supported-boards) - an ESP32-S3 AMOLED board from
  LilyGo, or the much cheaper "Cheap Yellow Display" (CYD).
- A computer running Chrome or Edge (both the browser flasher and the streaming page
  need Web Serial / screen-capture support only those two currently have).
- A USB cable, and a WiFi network your computer and the board can both reach.

## How do I get it working?


https://github.com/user-attachments/assets/d4254f9d-f0c3-4d79-989b-088d2321c415


1. Open the [browser flasher](https://lemio.github.io/EmbeddedScreenSharing/wizard.html)
   in Chrome or Edge.
2. Pick the firmware for your board (see [below](#which-firmware-do-i-want) if you're
   not sure which).
3. Enter your WiFi SSID and password - these get written into the firmware at flash
   time, no code editing needed.
4. Plug the board in over USB and follow the on-screen steps.
5. Once it connects to WiFi, the display shows its IP address and mDNS hostname. Visit
   that address in a browser to start streaming - pick a tab, window, or your whole
   screen, and it shows up live on the display.

Not sure which firmware to pick? On an AMOLED board, start with **webH264**. On a
CYD, start with **webRAW-CYD** - it's lossless and measured 3-6x faster to render
than webJPEG-CYD on this hardware (see that example's README). See
["Which firmware do I want?"](#which-firmware-do-i-want) below for the full
comparison.

Prefer PlatformIO over the browser flasher? See
[PlatformIO Quick Start](#platformio-quick-start) below.

## Supported boards

| Product                          | SOC        | Flash | PSRAM     | Resolution | Size  |
| --------------------------------- | ---------- | ----- | --------- | ---------- | ----- |
| [T-Display-AMOLED-Lite][1]       | ESP32-S3R8 | 16MB  | 8MB (OPI) | 194x368    | 1.47" |
| [T-Display-S3 AMOLED][2]         | ESP32-S3R8 | 16MB  | 8MB (OPI) | 240x536    | 1.91" |
| [T4-S3][3]                       | ESP32-S3R8 | 16MB  | 8MB (OPI) | 450x600    | 2.41" |
| [CYD (ESP32-2432S028R)][4]       | ESP32      | 4MB   | None      | 240x320    | 2.8"  |

Not every firmware runs on every board - webH264 needs an ESP32-S3 (the top three
rows), since the CYD's plain ESP32 chip has no H.264 decode support. See the table
below for what runs where.

## Which firmware do I want?


https://github.com/user-attachments/assets/b82c2d7a-fd30-4112-9a69-beb496bad723

Video showing the streaming of a browser showing a [countdown timer](https://m.youtube.com/watch?v=uo9dAIQR3g8) via JPEG frames and H264.


|            | [webJPEG](examples/webJPEG)                        | [webH264](examples/webH264)             | [webRAW](examples/webRAW)                              |
| ---------- | ---------------------------------------------------- | ---------------------------------------- | -------------------------------------------------------- |
| Codec      | MJPEG (one JPEG per frame)                            | H.264 (WebCodecs `VideoEncoder`)         | Raw RGB565, zipped in the browser, unzipped on the device  |
| Boards     | AMOLED lineup (auto-detected) + [CYD][4]              | AMOLED lineup only                       | AMOLED lineup (auto-detected) + [CYD][4]                  |
| Frame rate | ~5.9fps measured on 1.91" (536x240)                   | ~8.2fps measured on 1.91" (536x240)      | ~7.75fps measured on 1.91" (536x240) with flow control on |
| Bandwidth  | Higher for the same visual quality                    | Much lower - H.264 compresses far better | Bigger unless content zips well                            |
| Best for   | Simplicity - no codec tuning, works everywhere        | Lower bandwidth and a higher frame rate  | Lossless, flat/low-color content (UI, text) - see below   |

Both the webJPEG and webH264 numbers are real measurements on the same board, not
estimates - see [webJPEG's](examples/webJPEG/README.md#measured-performance) and
[webH264's](examples/webH264/README.md#measured-performance) READMEs for the full
data. This table's numbers are all from an AMOLED board, though - **on the CYD
specifically, webRAW-CYD renders 3-6x faster than webJPEG-CYD**, since it pushes
whole strips of pixels to the display at once instead of the many small blocks a
JPEG decode has to (see webRAW-CYD's README for the measurements) - it's the
recommended default there, not just an alternative worth trying.

**Why H.264 tends to win here, structurally, not just in this one measurement:**
webJPEG decodes every frame independently from scratch - a detailed, complex frame
costs roughly the same to decode whether it's frame 1 or frame 10,000, and nothing
carries over between frames. H.264 can reference the previous frame: once a complex
scene has been sent once (as a keyframe), unchanged regions in every frame after that
decode as cheap "skip" macroblocks - the cost tracks how much actually *changed*, not
how detailed the picture is. For a highly detailed but mostly-static picture, webJPEG
pays full decode cost on every single frame; H.264 pays it once.

webJPEG is still the simpler, more predictable choice if you'd rather every frame cost
the same regardless of content and don't want to think about codec tuning.

**webRAW** sends raw RGB565 pixels, zipped in the browser and unzipped on the device -
no JPEG artifacts, no H.264 blocking on detailed content, genuinely lossless. In
practice it works best on relatively static, low-color content (flat UI, text,
diagrams) - that zips down very well and updates quickly. Busy or fast-moving video
content doesn't zip well, and the ESP32's raw pixel-push throughput becomes the
bottleneck, so it updates noticeably less often - this isn't a general-purpose video
codec replacement, it's a lossless option for the content that suits it. Worth trying if visual
losslessness matters more than bandwidth, or if webJPEG's artifacts/webH264's codec
tuning aren't a good fit.

**Note on color depth:** even though the AMOLED panels support 24-bit color, every
pipeline in this repo (webJPEG, webH264, webRAW alike) only ever uses 16-bit RGB565 -
visually close to indistinguishable for typical UI/screen content, and it keeps the
wire format and on-device buffers simple.

All examples share the same streaming page ([stream.html](examples/stream.html)) and
the same WiFi setup flow. The page auto-detects which example is running on the board
you point it at.

### Flashing with PlatformIO

See [PlatformIO Quick Start](#platformio-quick-start) below, then:

```bash
pio run -e webJPEG --target upload   # or -e webH264, -e webRAW, -e webJPEG-CYD, -e webRAW-CYD
```

Before compiling, either use the browser flasher above to set your WiFi credentials, or
edit the placeholder values directly in that example's `.cpp` file:

```cpp
const char ssid[100] = "|*S*|";          // your WiFi SSID
const char password[100] = "|*P*|";      // your WiFi password
const char mdnsName[100] = "esp-screen"; // reachable at http://esp-screen.local
```

## Setup flow

Every example shows the same sequence on the display after flashing, along with a
small firmware name + build date line (e.g. "WebJPEG  Aug 4 2026") so a board's
physical display alone answers "what firmware is this, and is it stale enough to
reflash?" without needing serial/computer access:

1. Connecting to WiFi: your SSID, a spinner, and a progress bar (30 attempts, about
   15 seconds).
2. On success: WiFi Connected, your SSID, the board's IP address, its mDNS hostname,
   then Waiting for stream.
3. On failure: a QR code linking back to this repo for setup help.

Visit the address from step 2 in a browser. The board redirects you to the shared
[stream.html](examples/stream.html) page (see "Why the redirect" below), which
detects which example is running and shows the matching controls.

### Why the redirect

Screen capture only works in a secure context - HTTPS, or `localhost`. A LAN address
or `.local` name served over plain `http://`, which is all the board can do, doesn't
qualify. Serving the streaming page from the board would load but silently have no
working Start button. Redirecting to the same page hosted over HTTPS on GitHub Pages
fixes that, with two consequences:

- You need internet access to reach GitHub Pages, even though the video stream itself
  stays on your LAN. Without internet, open [stream.html](examples/stream.html) as a
  local file instead (`file://` also counts as secure) and fill in `espAddress`
  yourself.
- The connection back to the board is `ws://`, not `wss://` (the board can't easily
  serve TLS). Browsers block that as mixed content on an HTTPS page by default, so the
  stream fails the first time. In Chrome/Edge: click the padlock icon by the address
  bar, open Site settings, set Insecure content to Allow, then reload. This is a
  one-time exception per browser for the `lemio.github.io` origin - the "insecure"
  traffic only ever reaches your own board on your own LAN.

## Examples

```txt
examples/
├── stream.html   # Shared browser streaming page - auto-detects which example is running
├── webJPEG       # MJPEG over WebSocket - LilyGo AMOLED lineup
├── webH264       # H.264 over WebSocket - sharper image, lower bandwidth - LilyGo AMOLED lineup
├── webRAW        # Raw RGB565, zipped in the browser, no JPEG - LilyGo AMOLED lineup
├── webJPEG-CYD   # MJPEG, ported for the Cheap Yellow Display
└── webRAW-CYD    # webRAW's wire protocol, ported for the Cheap Yellow Display
```

See each example's README ([webJPEG](examples/webJPEG/README.md),
[webH264](examples/webH264/README.md), [webRAW](examples/webRAW/README.md),
[webJPEG-CYD](examples/webJPEG-CYD/README.md), [webRAW-CYD](examples/webRAW-CYD/README.md))
for streaming controls, troubleshooting, and technical notes.

## PlatformIO Quick Start

1. Install [Visual Studio Code](https://code.visualstudio.com/) and [Python](https://www.python.org/).
2. Install the `PlatformIO` extension from VS Code's Extensions panel, then restart
   VS Code.
3. Open this repo's root folder in VS Code (File, Open Folder).
4. Wait for PlatformIO to finish installing dependencies.
5. In the PlatformIO sidebar, pick the environment for your example and board (e.g.
   `webJPEG`, `webH264`, `webJPEG-CYD`), then Build, then Upload.
6. Connect the board over USB. Click Upload again if it didn't auto-detect the port.
7. Click the plug icon to open the Serial Monitor and watch it boot.
8. If it won't write, or the USB device keeps flashing, see the FAQ below.

## FAQ

**Can't find the upload port, or USB is unavailable (AMOLED boards).** The board uses
USB as its JTAG/upload port, which needs `USB CDC On Boot` enabled to also print
serial output. If the port disappears, enter upload mode manually:

1. Connect the board via USB.
2. Hold BOOT, then press and release RST while still holding BOOT.
3. Release BOOT.
4. Upload.

**1.47" AMOLED (T-Display-AMOLED-Lite)** does not support hardware screen rotation.

**Running on battery or external power instead of USB-C (AMOLED boards):** turn off
`USB CDC On Boot`, otherwise the board waits for USB access at startup. In
`platformio.ini`:

```ini
build_flags =
    -UARDUINO_USB_CDC_ON_BOOT
```

This also turns off Serial output over USB-C; output moves to GPIO43/GPIO44 instead.

**CYD colors look wrong, or the display is upside down.** Some CYD variants (notably
the two-USB-port "CYD2USB" clone) wire their panel with inverted colors - use the
browser flasher's Invert Colors option (or `tft.invertDisplay(1)` if building with
PlatformIO). For a board mounted upside down, use the Screen Rotation option - only
Normal/Upside-down are offered, since 90°/270° are a confirmed-broken mode on this
panel, not just unimplemented.

## Resources

| Product                    | Schematic                                        | Dimensions                                        | PCB 3D                                                  | Shell                                  |
| -------------------------- | ------------------------------------------------ | ------------------------------------------------- | ------------------------------------------------------- | --------------------------------------- |
| [T-Display-AMOLED-Lite][1] | [PDF](./schematic/T-Display_AMOLED-Lite.pdf)     | [DWG](./dimensions/T-Display-AMOLED-Lite.DWG)     | [STP](./dimensions/T-Display-AMOLED-Lite-PCB-3D.7z)     | -                                       |
| [T-Display-S3 AMOLED][2]   | [PDF](./schematic/T-Display-S3-AMOLED-Touch.pdf) | [DWG](./dimensions/T-Display-S3-AMOLED-Touch.DWG) | [STP](./dimensions/T-Display-S3-AMOLED-Touch-PCB-3D.7z) | [ZIP](./shell/T-Display-S3-AMOLED.zip)  |
| [T4-S3][3]                 | [PDF](./schematic/T4-S3-240719.pdf)              | [DWG](./dimensions/T4-S3-AMOLED.DWG)              | [STP](./dimensions/T4-S3-AMOLED-PCB-3D.7z)              | [ZIP](./shell/T4-S3-AMOLED.zip)         |

Datasheets: [ESP32S3-R8](https://www.espressif.com.cn/en/support/documents/technical-documents?keys=&field_type_tid%5B%5D=842),
[SH8501B0 (1.47" driver)](./datasheet/SH8501B0%20DataSheet.pdf),
[RM67162 (1.91" driver)](./datasheet/RM67162%20DataSheet.pdf),
[RM690B0 (T4-S3 driver)](./datasheet/RM690B0%20DataSheet_V0.2.pdf),
[CHSC5816 (touch)](https://github.com/lewisxhe/SensorLib/blob/master/datasheet/CHSC5816%E8%A7%A6%E6%8E%A7%E8%8A%AF%E7%89%87%E4%BD%BF%E7%94%A8%E8%AF%B4%E6%98%8EV1-20221114.pdf),
[AXP2101 (PMU)](https://github.com/lewisxhe/XPowersLib/blob/master/datasheet/AXP2101_Datasheet_V1.0_en.pdf),
[CM32181 (light sensor)](https://github.com/lewisxhe/SensorLib/blob/master/datasheet/CM32181A3OP.pdf).

The CYD doesn't have first-party resources here - see its own
[README](examples/webJPEG-CYD/README.md) for pin mapping and board notes.

## Learnings

[learnings.md](./learnings.md) covers non-obvious decisions and bugs found while
building this repo - useful if you're extending it or hit something similar.

## License

MIT - see [LICENSE](./LICENSE). Built on
[LilyGo-AMOLED-Series](https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series) by
LilyGO/Xinyuan-LilyGO.

[1]: https://www.lilygo.cc/products/t-display-amoled-lite
[2]: https://www.lilygo.cc/products/t-display-s3-amoled
[3]: https://www.lilygo.cc/products/t4-s3
[4]: https://nl.aliexpress.com/item/1005004971720824.html
