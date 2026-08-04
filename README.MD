<h1 align = "center">EmbeddedScreenSharing</h1>
<p align="center"><i>Use a cheap ESP32 display as an external monitor for prototyping</i></p>

[![PlatformIO CI](https://github.com/lemio/EmbeddedScreenSharing/actions/workflows/platformio.yml/badge.svg)](https://github.com/lemio/EmbeddedScreenSharing/actions/workflows/platformio.yml)

## What this is

Point your browser at anything - a tab, a Figma mirror, a screen recording, your
webcam - and it shows up live on a physical display over WiFi. No reflashing: change
what's on screen and the display just updates.

Started as a fork of LilyGO's
[LilyGo-AMOLED-Series](https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series)
board-support library, trimmed to two streaming examples (webJPEG, webH264) for that
AMOLED lineup, plus a browser-based flasher for them (no PlatformIO install required
just to try it). Since then it's grown two more examples
([webJPEG-CYD](examples/webJPEG-CYD), [webRAW-CYD](examples/webRAW-CYD)) for a
completely different, much cheaper, non-AMOLED board outside that lineup - see "Other
boards" below - so despite the repo's LilyGo-AMOLED-rooted history, "AMOLED" isn't a
hard requirement to use this project, just what the original two examples target.

## Which example do I want?

https://github.com/user-attachments/assets/b82c2d7a-fd30-4112-9a69-beb496bad723

Video showing the streaming of a browser showing a [countdown timer](https://m.youtube.com/watch?v=uo9dAIQR3g8) via JPEG frames and H264.

**Start with webH264** unless you have a specific reason not to. H.264 is the more
modern, more widely understood way to send video over a low-bandwidth link, and it
measured faster than webJPEG even on this software-only setup (see table below).

|            | [webJPEG](examples/webJPEG)                    | [webH264](examples/webH264)                                                  |
| ---------- | ---------------------------------------------- | ---------------------------------------------------------------------------- |
| Codec      | MJPEG (one JPEG per frame)                     | H.264 (WebCodecs `VideoEncoder`)                                             |
| Boards     | 1.47" Lite, 1.91", 2.41"/T4-S3 (auto-detected) | 1.47" Lite, 1.91", 2.41"/T4-S3 (auto-detected)                               |
| Frame rate | ~5.9fps measured on 1.91" (536x240)            | ~8.2fps measured on 1.91" (536x240)                                          |
| Bandwidth  | Higher for the same visual quality             | Much lower - H.264 compresses far better                                     |
| Best for   | Simplicity - no codec tuning, works everywhere | Lower bandwidth and, on this hardware, a higher frame rate too               |

Both frame-rate numbers are real measurements on the same board, not estimates - see
[webJPEG's](examples/webJPEG/README.md#measured-performance) and
[webH264's](examples/webH264/README.md#measured-performance) READMEs for the full data.
webJPEG previously had an open heap-fragmentation crash under sustained streaming;
it's fixed (see webJPEG's README "Stability" note) but not yet re-verified against a
real long-running reproduction.

**Why H.264 tends to win here, structurally, not just in this one measurement:**
webJPEG decodes every frame independently from scratch - a detailed, complex frame
costs roughly the same to decode whether it's frame 1 or frame 10,000, and nothing
carries over between frames. H.264 can reference the previous frame: once a complex
scene has been sent once (as a keyframe), unchanged regions in every frame after that
decode as cheap "skip" macroblocks - the cost tracks how much actually *changed*, not
how detailed the picture is. For a highly detailed but mostly-static picture, webJPEG
pays full decode cost on every single frame; H.264 pays it once. This is on top of
H.264's usual bandwidth advantage, and it's why the gap should widen further in
webH264's favor for realistic, detailed content, not just the flatter UI/screen
content this repo has been measured against so far. See webH264's README for the
bitrate/keyframe tuning that makes the most of this.

webJPEG is still the simpler, more predictable choice if you'd rather every frame cost
the same regardless of content and don't want to think about codec tuning.

**On chips with hardware H.264 decode (e.g. ESP32-P4):** this tradeoff would tilt even
further toward webH264, since the software-decode cost this repo's numbers are all
bound by wouldn't apply. This repo doesn't support P4 yet - both examples target
ESP32-S3 boards, which have no hardware H.264 decode block.

**A third option: [webRAW](examples/webRAW)** sends raw RGB565 pixels, deflate-
compressed in the browser - no JPEG artifacts, no H.264 blocking on detailed content,
genuinely lossless. Its on-device compute is real and measured: about 3x cheaper per
pixel than webJPEG's decode+render+push (see
[webRAW's README](examples/webRAW/README.md#measured-performance) and
[`benchmarks/`](benchmarks) for the numbers behind that). That compute advantage
*doesn't* translate into a similarly large real-world speed win, though - measured
sustained throughput on the same 1.91" board is ~7.75fps with flow control on, close
to webH264's ~8.2fps above, because browser-side compression and network time end up
dominating the total pipeline more than the fast on-device decompress does. The real
tradeoff is bandwidth: a raw payload is bigger than either JPEG or H.264 unless the
content compresses very well with deflate (flat UI/screen-share content usually does;
a busy photograph or video won't). Worth trying if visual losslessness matters more
than bandwidth for your content, or if webJPEG's JPEG artifacts/webH264's codec tuning
aren't a good fit - not a strict upgrade over either. Experimental, not on the browser
flasher yet.

All three examples share the same streaming page ([stream.html](examples/stream.html))
and the same WiFi setup flow. The page auto-detects which example is running on the
board you point it at.

## Flashing through your browser


https://github.com/user-attachments/assets/d4254f9d-f0c3-4d79-989b-088d2321c415


No PlatformIO install needed. Every push to `main` builds and publishes ready-to-flash
firmware at the [browser flasher](https://lemio.github.io/EmbeddedScreenSharing/wizard.html):

1. Open that link in Chrome or Edge (Web Serial support is required).
2. Pick WebJPEG or WebH264 for the AMOLED lineup (see the comparison table above), or
   WebJPEG-CYD/WebRAW-CYD for the "Cheap Yellow Display" board (see "Other boards"
   below) - the CYD entries are new and not yet confirmed working through this
   specific browser tool on real hardware, see their READMEs' "Flashing" sections.
3. Enter your WiFi SSID, password, and optionally an mDNS hostname - these get
   written into the firmware at flash time.
4. Plug the board in over USB and follow the on-screen steps.
5. Once it connects to WiFi, the display shows its IP address and mDNS hostname.
   Visit that address in a browser to start streaming.

### Flashing with PlatformIO

See [PlatformIO Quick Start](#platformio-quick-start) below, then:

```bash
pio run -e webJPEG --target upload   # or -e webH264
```

Before compiling, either use the browser flasher above to set your WiFi credentials, or
edit the placeholder values directly in that example's `.cpp` file:

```cpp
const char ssid[100] = "|*S*|";          // your WiFi SSID
const char password[100] = "|*P*|";      // your WiFi password
const char mdnsName[100] = "esp-screen"; // reachable at http://esp-screen.local
```

## Setup flow

Both examples show the same sequence on the display after flashing. Every screen also
shows a small firmware name + build date line (e.g. "WebJPEG  Aug 4 2026") - since
every example now shares the same default hostname (see "Flashing with PlatformIO"
above), this is how a board's physical display alone answers "what firmware is this,
and is it stale enough to reflash?" without needing serial/computer access:

1. Connecting to WiFi: your SSID, a spinner, and a progress bar (30 attempts, about
   15 seconds).
2. On success: WiFi Connected, your SSID, the board's IP address, its mDNS hostname,
   then Waiting for stream.
3. On failure: a QR code linking back to this repo for setup help.

Visit the address from step 2 in a browser. The board redirects you to the shared
[stream.html](examples/stream.html) page (see "Why the redirect" below), which
detects which example is running and shows the matching controls.

### Why the redirect

Screen/camera capture only works in a secure context - HTTPS, or `localhost`. A LAN
address or `.local` name served over plain `http://`, which is all the board can do,
doesn't qualify. Serving the streaming page from the board would load but silently
have no working Start button. Redirecting to the same page hosted over HTTPS on GitHub
Pages fixes that, with two consequences:

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

## Supported boards (LilyGo AMOLED lineup)

webJPEG/webH264/webRAW's auto-detected boards - see "Other boards" below for the
non-AMOLED board the CYD examples target instead.

| Product                    | SOC        | Flash | PSRAM     | Resolution | Size  |
| -------------------------- | ---------- | ----- | --------- | ---------- | ----- |
| [T-Display-AMOLED-Lite][1] | ESP32-S3R8 | 16MB  | 8MB (OPI) | 194x368    | 1.47" |
| [T-Display-S3 AMOLED][2]   | ESP32-S3R8 | 16MB  | 8MB (OPI) | 240x536    | 1.91" |
| [T4-S3][3]                 | ESP32-S3R8 | 16MB  | 8MB (OPI) | 450x600    | 2.41" |

## Other boards

[webJPEG-CYD](examples/webJPEG-CYD) and [webRAW-CYD](examples/webRAW-CYD) port this
project to the "Cheap Yellow Display" (ESP32-2432S028R) - a much cheaper, widely
available board completely outside the LilyGo AMOLED lineup above, with no AMOLED
panel at all: plain ESP32 (not S3), no PSRAM, 2.8" 240x320 ILI9341 LCD over plain SPI.
**webH264 cannot run on it at all** - Espressif's `esp_h264` component has no build for
plain ESP32, only S3/S3-variant/P4 (see that example's README). Confirmed working
end-to-end on real hardware for both examples - see their READMEs for real
measurements and board-specific quirks found along the way (upload speed sensitivity,
a rotation bug, a stack-size crash fix, an ack-loss recovery fix). Both are now also on
the [browser flasher](https://lemio.github.io/EmbeddedScreenSharing/wizard.html), not
just PlatformIO - see "Flashing through your browser" above and each README's
"Flashing" section for that path's caveats.

## Examples

```txt
examples/
├── stream.html   # Shared browser streaming page - auto-detects which example is running
├── webJPEG       # MJPEG over WebSocket - LilyGo AMOLED lineup
├── webH264       # H.264 over WebSocket - sharper image, lower bandwidth - LilyGo AMOLED lineup
├── webRAW        # Raw RGB565, deflate-compressed in the browser, no JPEG - LilyGo AMOLED lineup
├── webJPEG-CYD   # MJPEG, ported for the Cheap Yellow Display - see "Other boards" above
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
5. In the PlatformIO sidebar, pick the `webJPEG` or `webH264` environment, then Build,
   then Upload.
6. Connect the board over USB. Click Upload again if it didn't auto-detect the port.
7. Click the plug icon to open the Serial Monitor and watch it boot.
8. If it won't write, or the USB device keeps flashing, see the FAQ below.

## FAQ

**Can't find the upload port, or USB is unavailable.** The board uses USB as its
JTAG/upload port, which needs `USB CDC On Boot` enabled to also print serial output.
If the port disappears, enter upload mode manually:

1. Connect the board via USB.
2. Hold BOOT, then press and release RST while still holding BOOT.
3. Release BOOT.
4. Upload.

**1.47" AMOLED (T-Display-AMOLED-Lite)** does not support hardware screen rotation.

**Running on battery or external power instead of USB-C:** turn off `USB CDC On Boot`,
otherwise the board waits for USB access at startup. In `platformio.ini`:

```ini
build_flags =
    -UARDUINO_USB_CDC_ON_BOOT
```

This also turns off Serial output over USB-C; output moves to GPIO43/GPIO44 instead.

## Resources

| Product                    | Schematic                                        | Dimensions                                        | PCB 3D                                                  | Shell                                  |
| -------------------------- | ------------------------------------------------ | ------------------------------------------------- | ------------------------------------------------------- | -------------------------------------- |
| [T-Display-AMOLED-Lite][1] | [PDF](./schematic/T-Display_AMOLED-Lite.pdf)     | [DWG](./dimensions/T-Display-AMOLED-Lite.DWG)     | [STP](./dimensions/T-Display-AMOLED-Lite-PCB-3D.7z)     | -                                      |
| [T-Display-S3 AMOLED][2]   | [PDF](./schematic/T-Display-S3-AMOLED-Touch.pdf) | [DWG](./dimensions/T-Display-S3-AMOLED-Touch.DWG) | [STP](./dimensions/T-Display-S3-AMOLED-Touch-PCB-3D.7z) | [ZIP](./shell/T-Display-S3-AMOLED.zip) |
| [T4-S3][3]                 | [PDF](./schematic/T4-S3-240719.pdf)              | [DWG](./dimensions/T4-S3-AMOLED.DWG)              | [STP](./dimensions/T4-S3-AMOLED-PCB-3D.7z)              | [ZIP](./shell/T4-S3-AMOLED.zip)        |

Datasheets: [ESP32S3-R8](https://www.espressif.com.cn/en/support/documents/technical-documents?keys=&field_type_tid%5B%5D=842),
[SH8501B0 (1.47" driver)](./datasheet/SH8501B0%20DataSheet.pdf),
[RM67162 (1.91" driver)](./datasheet/RM67162%20DataSheet.pdf),
[RM690B0 (T4-S3 driver)](./datasheet/RM690B0%20DataSheet_V0.2.pdf),
[CHSC5816 (touch)](https://github.com/lewisxhe/SensorLib/blob/master/datasheet/CHSC5816%E8%A7%A6%E6%8E%A7%E8%8A%AF%E7%89%87%E4%BD%BF%E7%94%A8%E8%AF%B4%E6%98%8EV1-20221114.pdf),
[AXP2101 (PMU)](https://github.com/lewisxhe/XPowersLib/blob/master/datasheet/AXP2101_Datasheet_V1.0_en.pdf),
[CM32181 (light sensor)](https://github.com/lewisxhe/SensorLib/blob/master/datasheet/CM32181A3OP.pdf).

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
