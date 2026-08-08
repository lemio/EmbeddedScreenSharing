/**
 * @file      webJPEG-CYD.cpp
 * @license   MIT
 * @note      Streams a browser tab, window, or camera to a "Cheap Yellow Display"
 *            (ESP32-2432S028R: plain ESP32, 2.8" 240x320 ILI9341, no PSRAM) over WiFi.
 *            Same WebSocket + MJPEG protocol as examples/webJPEG/webJPEG.cpp - this is
 *            a separate port, not a shared codebase, because this board differs from
 *            every other board in this repo in two ways that matter:
 *
 *            1. It's a plain ESP32 (Xtensa LX6), not an ESP32-S3. webH264 is not an
 *               option here at all - Espressif's esp_h264 component only ships
 *               prebuilt decoder binaries for esp32s3/esp32s3-variant/esp32p4 (see
 *               managed_components/espressif__esp_h264/idf_component.yml's `targets:`
 *               list, and the absence of an `esp32/` folder under its `sw/libs/`).
 *               MJPEG has no such restriction - JPEGDecoder is a portable C library.
 *
 *            2. It has no PSRAM - only the ESP32's ~520KB internal SRAM, most of it
 *               already spoken for by WiFi/BT/FreeRTOS/Arduino overhead. Every other
 *               example in this repo leans on PSRAM (LilyGo_AMOLED's framebuffer,
 *               webH264's decode/assembly buffers). Here, unlike
 *               examples/webJPEG/webJPEG.cpp, there is no full-frame sprite: MCU
 *               blocks are pushed straight to the display as they decode. That's the
 *               exact per-MCU-block direct-render approach the AMOLED boards moved
 *               away from (see that file's history and learnings.md) because it was
 *               both slower and visibly rendered top-to-bottom - but on a board this
 *               memory-constrained, skipping a ~150KB sprite buffer is the right
 *               trade, not a regression. See learnings.md for the full reasoning.
 *
 * Required libraries:
 * - ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 * - AsyncTCP: https://github.com/me-no-dev/AsyncTCP
 * - JPEGDecoder: https://github.com/Bodmer/JPEGDecoder
 * - TFT_eSPI: https://github.com/Bodmer/TFT_eSPI (pin mapping supplied entirely via
 *   this env's build_flags in platformio.ini - see that file, no User_Setup.h edit
 *   needed)
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <JPEGDecoder.h>
#include "qrcodegen.h"

// WiFi credentials. ssid/password have no sensible default (must be set via the browser
// flasher or before compiling) - see examples/webJPEG/webJPEG.cpp for the full
// explanation of this placeholder convention. This board's flasher support is not yet
// confirmed working on real hardware (see README's "Flashing"), so local builds still
// need real credentials some other way too - wifi_credentials.h is gitignored and, if
// present, overrides WIFI_SSID/WIFI_PASSWORD below; see wifi_credentials.h.example for
// the format. CI (GitHub Actions) never has that file, so it always builds with the
// "|*S*|"/"|*P*|" placeholders intact.
#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "|*S*|"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "|*P*|"
#endif
const char ssid[100] = WIFI_SSID;
const char password[100] = WIFI_PASSWORD;
// Same shared "esp-screen" default as every other example in this repo now - see
// webJPEG.cpp's identical comment for why.
const char mdnsName[100] = "esp-screen";

// Flash-time-patchable, same convention as ssid/password/mdnsName above (see the
// browser flasher's replaceVariables() in ESP32-S3-Flasher-phase2/wizard.html for the
// exact byte-for-byte patch mechanism) - unpatched (plain PlatformIO build/upload)
// leaves these as non-numeric text, and atoi() on that returns 0, i.e. the same
// rotation-0/not-inverted default this board always used before these existed.
// Needed because CYD boards vary in ways firmware can't detect on its own:
// - Rotation: only 0deg and 180deg are ever applied below, regardless of what value
//   gets patched in - see setup()'s use of this for why (90deg/270deg are confirmed
//   broken on this panel, not just unimplemented).
// - Invert: some CYD variants (notably the two-USB-port "CYD2USB" clone) wire their
//   panel with inverted colors at the hardware level - see
//   https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/master/cyd.md's
//   "My CYD has two USB ports" section, which documents the same fix
//   (`tft.invertDisplay(1)`) this repo has no such board to test against itself.
const char rotationStr[16] = "|*ROTATION*|";
const char invertStr[16] = "|*INVERT*|";

// Shown on-device (WiFi connect/connected screens) - see webJPEG.cpp's identical
// FW_VARIANT/buildDateString() for the full explanation.
#define FW_VARIANT "WebJPEG-CYD"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

// See webJPEG.cpp's identical firmwareBuildDate for the full explanation - read (not
// patched) by the browser flasher to show a build date in the firmware list.
// __attribute__((used)): without it the linker strips this as an unused symbol and
// it never makes it into firmware.bin - confirmed the hard way.
const char firmwareBuildDate[32] __attribute__((used)) = "|*FW*|" __DATE__ "|*FW*|";

// Shown as a QR code so the user can find help/docs if WiFi connection fails
const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

// A plain string, not a library-reported name - this board isn't part of
// LilyGo_AMOLED's auto-detected lineup, there's only ever this one variant. No literal
// '"' here on purpose - /boardinfo below concatenates this directly into a JSON string
// without escaping, so a raw quote would silently corrupt that response.
const char boardName[] = "CYD ESP32-2432S028R (2.8in ILI9341)";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");          // color images
AsyncWebSocket wsMono("/ws-mono"); // monochrome images

// Hardcoded, not tft.width()/height() - confirmed on real hardware that TFT_eSPI's own
// reporting for this panel doesn't track its physical reality (a rotation-diagnostic
// with a labeled pixel grid, tried across all four setRotation() values, showed a hard
// ~240px addressable-width ceiling regardless of which rotation TFT_eSPI claimed was
// "320 wide" - a driver/controller-level quirk on this specific ILI9341 clone, not
// something a different setRotation() value fixes). At rotation(0), this panel is
// physically landscape; TFT_eSPI's own width()/height() for that rotation report
// 240x320 (backwards) regardless. See learnings.md.
#define WIDTH  320
#define HEIGHT 240

TFT_eSPI tft = TFT_eSPI(WIDTH,HEIGHT);



volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile bool isMonochrome = false;

// True from the first byte of a WS message until its ack has actually been sent -
// see webRAW.cpp's identical flag/comment for the full story: acking immediately on
// receipt (the original design here) let a waiting client outrun the ~160-200ms
// render pipeline, since a send+ack round trip only cost the ~8-10ms Recv stage -
// confirmed on real hardware as an ~90% drop rate despite "waiting" the whole time.
// Shared across both /ws and /ws-mono, same as frameBuffer/frameMutex above - only
// one frame is ever in flight across both endpoints at once by design.
volatile bool frameInFlight = false;
// See examples/webJPEG/webJPEG.cpp's identical field for what this is - ported as-is,
// PSRAM-vs-not doesn't change this part of the design.
volatile uint32_t frameRecvTimestamp = 0;
SemaphoreHandle_t frameMutex;

// Unlike examples/webJPEG/webJPEG.cpp's 512KB (sized for 8MB of PSRAM), this board has
// no PSRAM at all - every byte here comes out of the same ~520KB of internal SRAM that
// WiFi, Bluetooth, FreeRTOS and the Arduino core are also drawing from. Same
// fixed-size, allocated-once-in-setup() reasoning as the AMOLED boards' crash fix
// applies equally here - see learnings.md - just at a much smaller size because
// there's no PSRAM to be generous with.
//
// Confirmed on real hardware, not just estimated: at boot, right before this
// allocation, free heap was 302,844 bytes; a 3x64KB request (192KB) failed outright
// even though that's well under the total free - not a total-memory problem but a
// contiguous-block one (heap fragmentation from Arduino core/TFT_eSPI init limits the
// largest single allocation, independent of how much is free in aggregate). 3x40KB
// (120KB) succeeded, leaving 179,916 bytes free afterward for WiFi/BT/AsyncWebServer.
// 40KB is comfortably enough for a 240x320 JPEG regardless (even high-quality frames
// at this resolution rarely exceed 40-50KB) - if yours needs more, the free-heap log
// lines in setup() will show whether there's room to raise it.
static const size_t MAX_FRAME_SIZE = 40 * 1024;

uint8_t* wsAssemblyBuffer = nullptr;
size_t wsAssemblySize = 0;
size_t wsExpectedSize = 0;
uint32_t wsRecvStartMs = 0;

uint8_t* wsMonoAssemblyBuffer = nullptr;
size_t wsMonoAssemblySize = 0;
size_t wsMonoExpectedSize = 0;
uint32_t wsMonoRecvStartMs = 0;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

#define LOGF(fmt, ...) Serial.printf("[%10lu] " fmt, millis(), ##__VA_ARGS__)
#define LOGLN(msg) Serial.printf("[%10lu] " msg "\n", millis())

void drawJPEG(uint8_t *jpegData, size_t jpegSize, uint32_t recvStartMs) {
    uint32_t t1 = millis();

    if (!JpegDec.decodeArray(jpegData, jpegSize)) {
        LOGF("JPEG decode failed! (Recv=%lums)\n", t1 - recvStartMs);
        return;
    }

    uint32_t t2 = millis();

    uint16_t w = JpegDec.width;
    uint16_t h = JpegDec.height;

    if (w == 0 || h == 0) {
        LOGLN("Invalid JPEG dimensions!");
        return;
    }

    // True when the JPEG already matches the display size exactly - stream.html
    // always sizes its capture canvas to match the selected display size, so this is
    // the overwhelmingly common case in practice; a mismatch (directRender == false)
    // only happens with a manually-set wrong size.
    bool directRender = (w == WIDTH && h == HEIGHT);

    LOGF("JPEG: %dx%d | Display: %dx%d | Direct: %s | MCU: %dx%d\n",
         w, h, WIDTH, HEIGHT, directRender ? "YES" : "NO",
         JpegDec.MCUWidth, JpegDec.MCUHeight);

    // No sprite buffer here (see the file header comment for why), so centering a
    // mismatched frame means clearing the real screen directly - this can flicker
    // since there's no offscreen buffer to prepare first. Rare path in practice.
    if (!directRender) {
        tft.fillScreen(TFT_BLACK);
    }

    int16_t offsetX = (WIDTH - w) >> 1;
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;

    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;

    uint32_t t3 = millis();

    // Row-buffer optimization: MCUs decode in strict raster order (left-to-right,
    // then down a row), so for the common directRender case (a "row" of MCUs always
    // spans the full display width, no cropping) we can accumulate one MCU row - at
    // most WIDTH x 16 ×2 bytes = 10KB, since 16 is the largest MCU height baseline
    // JPEG uses (4:2:0 subsampling) - and push it as ONE SPI transaction instead of
    // ~20 individual per-MCU ones. Real-hardware testing found the original
    // one-push-per-MCU-block approach visibly "painted in" block by block over
    // ~185ms - this cuts the push count roughly 20x, which should both speed up
    // rendering (fewer, larger transactions) and make that visible tearing/build-up
    // effect far less pronounced. The rare mismatched-size fallback (directRender ==
    // false) keeps the original per-block push below instead, to avoid the added
    // complexity of a partial-width row buffer on a path that already flickers.
    static uint16_t rowBuffer[WIDTH * 16];
    int16_t rowBufferY = -1;

    // taskYIELD() every 64 blocks gives the WiFi/TCP stack's own task a chance to
    // run during this loop - see webJPEG.cpp's identical comment and learnings.md.
    uint16_t mcuCount = 0;
    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;

        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        if (mcu_x >= w || mcu_y >= h) continue;

        uint16_t valid_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t valid_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        if (valid_w == 0 || valid_h == 0) continue;

        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        if (destX >= WIDTH || destY >= HEIGHT) continue;

        uint16_t render_w = valid_w;
        uint16_t render_h = valid_h;
        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }
        if (render_w == 0 || render_h == 0) continue;

        // tft.pushImage() byte-swaps internally (setSwapBytes(true) in setup()), so no
        // manual swap needed here - same as the sprite path on the AMOLED boards.
        if (directRender) {
            if (destY != rowBufferY) {
                if (rowBufferY >= 0) {
                    tft.pushImage(0, rowBufferY, WIDTH, mcu_h, rowBuffer);
                }
                rowBufferY = destY;
            }
            for (uint16_t row = 0; row < render_h; row++) {
                memcpy(&rowBuffer[row * WIDTH + destX], &pImg[row * mcu_w], render_w * sizeof(uint16_t));
            }
        } else if (render_w != mcu_w || render_h != mcu_h) {
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    tempBuffer[row * render_w + col] = pImg[row * mcu_w + col];
                }
            }
            tft.pushImage(destX, destY, render_w, render_h, tempBuffer);
        } else {
            tft.pushImage(destX, destY, render_w, render_h, pImg);
        }

        if (++mcuCount % 64 == 0) {
            taskYIELD();
        }
    }
    if (directRender && rowBufferY >= 0) {
        tft.pushImage(0, rowBufferY, WIDTH, mcu_h, rowBuffer);
    }

    uint32_t t4 = millis();

    // No separate bulk push step - each block/row above already reached the display
    // directly, so this stage is 0ms by construction. Kept in the log for the same
    // field layout as examples/webJPEG/webJPEG.cpp, so a serial log from this board
    // is directly comparable to one from an AMOLED board.
    uint32_t t5 = t4;

    LOGF("Timing: Recv=%lums | Decode=%lums | Setup=%lums | Render=%lums | Push=%lums | Total=%lums\n",
         t1 - recvStartMs, t2-t1, t3-t2, t4-t3, t5-t4, t5-recvStartMs);
}

void drawMonoJPEG(uint8_t *jpegData, size_t jpegSize, uint32_t recvStartMs) {
    uint32_t t1 = millis();

    if (!JpegDec.decodeArray(jpegData, jpegSize)) {
        LOGF("Mono JPEG decode failed! (Recv=%lums)\n", t1 - recvStartMs);
        return;
    }

    uint32_t t2 = millis();

    uint16_t w = JpegDec.width;
    uint16_t h = JpegDec.height;

    if (w == 0 || h == 0) {
        LOGLN("Invalid JPEG dimensions!");
        return;
    }

    LOGF("Mono JPEG: %dx%d | Display: %dx%d\n", w, h, WIDTH, HEIGHT);

    bool directRender = (w == WIDTH && h == HEIGHT);

    if (!directRender) {
        tft.fillScreen(TFT_BLACK);
    }

    int16_t offsetX = (WIDTH - w) >> 1;
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;

    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;

    uint32_t t3 = millis();

    // See drawJPEG()'s identical row-buffer comment for why this exists - same
    // one-push-per-MCU-row optimization, mirrored here for the mono pipeline.
    static uint16_t rowBufferMono[WIDTH * 16];
    int16_t rowBufferMonoY = -1;

    uint16_t mcuCount = 0;
    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;

        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        if (mcu_x >= w || mcu_y >= h) continue;

        uint16_t render_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t render_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        if (render_w == 0 || render_h == 0) continue;

        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        if (destX >= WIDTH || destY >= HEIGHT) continue;

        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }

        if (directRender) {
            if (destY != rowBufferMonoY) {
                if (rowBufferMonoY >= 0) {
                    tft.pushImage(0, rowBufferMonoY, WIDTH, mcu_h, rowBufferMono);
                }
                rowBufferMonoY = destY;
            }
            for (uint16_t row = 0; row < render_h; row++) {
                memcpy(&rowBufferMono[row * WIDTH + destX], &pImg[row * mcu_w], render_w * sizeof(uint16_t));
            }
        } else if (render_w != mcu_w || render_h != mcu_h) {
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    tempBuffer[row * render_w + col] = pImg[row * mcu_w + col];
                }
            }
            tft.pushImage(destX, destY, render_w, render_h, tempBuffer);
        } else {
            tft.pushImage(destX, destY, render_w, render_h, pImg);
        }

        if (++mcuCount % 64 == 0) {
            taskYIELD();
        }
    }
    if (directRender && rowBufferMonoY >= 0) {
        tft.pushImage(0, rowBufferMonoY, WIDTH, mcu_h, rowBufferMono);
    }

    uint32_t t4 = millis();
    uint32_t t5 = t4;

    LOGF("Mono Timing: Recv=%lums | Decode=%lums | Setup=%lums | Render=%lums | Push=%lums | Total=%lums\n",
         t1 - recvStartMs, t2-t1, t3-t2, t4-t3, t5-t4, t5-recvStartMs);
}

// Renders `text` as a QR code centered at (centerX, centerY), scaled as large as
// possible within a maxSize x maxSize box. Draws straight to tft - see the file
// header comment for why there's no sprite to draw into first on this board.
#define QR_MAX_VERSION 10
void drawQRCode(const char *text, int16_t centerX, int16_t centerY, int16_t maxSize, uint16_t fgColor, uint16_t bgColor) {
    uint8_t qrTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    uint8_t qrOut[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

    bool ok = qrcodegen_encodeText(text, qrTemp, qrOut, qrcodegen_Ecc_MEDIUM,
                                    qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                    qrcodegen_Mask_AUTO, true);
    if (!ok) {
        LOGLN("QR code generation failed!");
        return;
    }

    int size = qrcodegen_getSize(qrOut);
    int quietZoneModules = 2;
    int scale = maxSize / (size + quietZoneModules * 2);
    if (scale < 1) scale = 1;

    int totalSize = (size + quietZoneModules * 2) * scale;
    int startX = centerX - totalSize / 2;
    int startY = centerY - totalSize / 2;

    tft.fillRect(startX, startY, totalSize, totalSize, bgColor);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrOut, x, y)) {
                tft.fillRect(startX + (quietZoneModules + x) * scale,
                             startY + (quietZoneModules + y) * scale,
                             scale, scale, fgColor);
            }
        }
    }
}

void setupWiFi() {
    LOGLN("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    const int maxAttempts = 30;
    const char spinnerFrames[] = {'|', '/', '-', '\\'};
    int attempts = 0;

    // Drawn once, not every attempt - only the spinner character and the progress bar
    // fill (both inside the loop below) actually change per attempt. Re-clearing and
    // redrawing the whole screen every 500ms (the old code called fillScreen(BLACK)
    // on every iteration) produced a visible black flash each tick, since there's no
    // sprite buffer on this board to prepare a frame in before it's visible (see the
    // file header comment for why). Redrawing just the small changing regions with
    // opaque text/fill (setTextColor's background param, fillRect) avoids that without
    // needing one.
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    // See webJPEG.cpp's identical line for why this is here - static like the rest of
    // this block, so it's drawn once here rather than inside the per-attempt loop.
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(String(FW_VARIANT) + "  " + buildDateString(), WIDTH / 2, 4, 1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Connecting to WiFi", WIDTH / 2, HEIGHT * 0.10, 2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(ssid, WIDTH / 2, HEIGHT * 0.10 + 22, 2);

    int barWidth = WIDTH * 0.6;
    int barHeight = 8;
    int barX = (WIDTH - barWidth) / 2;
    int barY = HEIGHT - 28;
    tft.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
    tft.setTextDatum(TL_DATUM);

    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        char spinner[2] = { spinnerFrames[attempts % 4], '\0' };
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(spinner, WIDTH / 2, HEIGHT * 0.52, 4);
        tft.setTextDatum(TL_DATUM);

        int fillWidth = (barWidth - 2) * attempts / maxAttempts;
        tft.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, TFT_CYAN);

        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        LOGLN("WiFi connected!");
        LOGF("IP address: %s\n", WiFi.localIP().toString().c_str());

        if (MDNS.begin(mdnsName)) {
            LOGF("mDNS responder started: %s.local\n", mdnsName);
        } else {
            LOGLN("Error setting up mDNS");
        }

        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString(String(FW_VARIANT) + "  " + buildDateString(), WIDTH / 2, 4, 1);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString("WiFi Connected", WIDTH / 2, HEIGHT * 0.08, 2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(ssid, WIDTH / 2, HEIGHT * 0.08 + 22, 2);
        tft.drawString(WiFi.localIP().toString(), WIDTH / 2, HEIGHT * 0.08 + 44, 2);
        tft.drawString("http://" + String(mdnsName) + ".local", WIDTH / 2, HEIGHT * 0.08 + 66, 2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("Waiting for stream...", WIDTH / 2, HEIGHT * 0.08 + 92, 2);
        tft.setTextDatum(TL_DATUM);
    } else {
        LOGLN("WiFi connection failed!");
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TC_DATUM);

        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Can't connect to WiFi network", WIDTH / 2, HEIGHT * 0.05, 1);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(ssid, WIDTH / 2, HEIGHT * 0.05 + 14, 2);

        int textBottom = HEIGHT * 0.05 + 14 + 18;
        int captionHeight = 16;
        int qrMaxSize = min((int)WIDTH, (int)HEIGHT - textBottom - captionHeight) * 0.9;
        int qrCenterY = textBottom + (HEIGHT - captionHeight - textBottom) / 2;
        drawQRCode(githubRepoUrl, WIDTH / 2, qrCenterY, qrMaxSize, TFT_WHITE, TFT_BLACK);

        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("Scan for setup help", WIDTH / 2, HEIGHT - 16, 2);

        tft.setTextDatum(TL_DATUM);
    }
}

void setupWebServer() {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"variant\":\"jpeg\",";
        json += "\"name\":\"" + String(boardName) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        // No LilyGo_AMOLED board-ID enum applies here - this firmware only ever runs
        // on this one board, so there's nothing to distinguish. stream.html doesn't
        // use this field for anything; kept only for /boardinfo shape compatibility.
        json += "\"boardId\":0,";
        // Lets stream.html cap JPEG quality/size client-side instead of encoding and
        // sending a frame this device can only ever drop - see MAX_FRAME_SIZE's comment.
        // Real-world importance confirmed on this board: a 320x240 color JPEG hit
        // 42,158 bytes (over the 40KB default) at default quality, getting silently
        // dropped by the device every time until stream.html started checking this.
        json += "\"maxFrameSize\":" + String(MAX_FRAME_SIZE);
        json += "}";
        request->send(200, "application/json", json);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String url = "https://lemio.github.io/EmbeddedScreenSharing/stream.html?espAddress=http://" + request->host();
        request->redirect(url);
    });

    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            LOGF("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        } else if (type == WS_EVT_DISCONNECT) {
            LOGF("WebSocket client #%u disconnected\n", client->id());
            wsAssemblySize = 0;
            wsExpectedSize = 0;
            // Safety reset - see frameInFlight's declaration comment. Mirrors
            // webRAW.cpp's identical reset for a message caught mid-reassembly when
            // the client disconnects.
            frameInFlight = false;
        } else if (type == WS_EVT_DATA) {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;

            if (info->opcode != WS_BINARY && info->opcode != WS_CONTINUATION) {
                return;
            }

            if (info->len > MAX_FRAME_SIZE) {
                LOGF("Dropping oversized WS message: %llu bytes\n", (unsigned long long)info->len);
                return;
            }

            if (info->index == 0) {
                wsExpectedSize = info->len;
                wsAssemblySize = 0;
                wsRecvStartMs = millis();
                frameInFlight = true;
            }

            if (info->index + len > wsExpectedSize || info->index + len > MAX_FRAME_SIZE) {
                return;
            }

            memcpy(wsAssemblyBuffer + info->index, data, len);
            // High-water mark, not `+= len` - see webRAW-CYD.cpp's identical fix for why
            // a running sum can silently desync from what's actually in the buffer.
            if (info->index + len > wsAssemblySize) {
                wsAssemblySize = info->index + len;
            }

            if (!info->final || wsAssemblySize != wsExpectedSize) {
                return;
            }

            if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                memcpy((void*)frameBuffer, wsAssemblyBuffer, wsAssemblySize);
                frameSize = wsAssemblySize;
                newFrameAvailable = true;
                isMonochrome = false;
                frameRecvTimestamp = wsRecvStartMs;

                xSemaphoreGive(frameMutex);
                // No ack here anymore - see loop()'s ack-after-render for why (moved
                // there so "wait mode" backpressures against actual render time, not
                // just network time - see frameInFlight's declaration comment).
            } else {
                LOGF("Mutex busy - frame dropped (Recv=%lums)\n", (uint32_t)(millis() - wsRecvStartMs));
                // Still ack a dropped frame - see webRAW.cpp's identical comment on
                // its own mutex-busy branch for why (mainly for Ack Mode "immediate",
                // which ignores acks anyway but shouldn't be left permanently stalled
                // by this code path either). Guarded by status() - see
                // webRAW-CYD.cpp's identical guard for the real hardware crash
                // (LoadProhibited inside _queueMessage's mutex lock on a torn-down
                // client) this closes.
                if (client->status() == WS_CONNECTED) {
                    client->text("a");
                }
                frameInFlight = false;
            }
        }
    });

    server.addHandler(&ws);

    wsMono.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            LOGF("Mono WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        } else if (type == WS_EVT_DISCONNECT) {
            LOGF("Mono WebSocket client #%u disconnected\n", client->id());
            wsMonoAssemblySize = 0;
            wsMonoExpectedSize = 0;
            frameInFlight = false;
        } else if (type == WS_EVT_DATA) {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;

            if (info->opcode != WS_BINARY && info->opcode != WS_CONTINUATION) {
                return;
            }

            if (info->len > MAX_FRAME_SIZE) {
                LOGF("Dropping oversized mono WS message: %llu bytes\n", (unsigned long long)info->len);
                return;
            }

            if (info->index == 0) {
                wsMonoExpectedSize = info->len;
                wsMonoAssemblySize = 0;
                wsMonoRecvStartMs = millis();
                frameInFlight = true;
            }

            if (info->index + len > wsMonoExpectedSize || info->index + len > MAX_FRAME_SIZE) {
                return;
            }

            memcpy(wsMonoAssemblyBuffer + info->index, data, len);
            // High-water mark, not `+= len` - see webRAW-CYD.cpp's identical fix for why
            // a running sum can silently desync from what's actually in the buffer.
            if (info->index + len > wsMonoAssemblySize) {
                wsMonoAssemblySize = info->index + len;
            }

            if (!info->final || wsMonoAssemblySize != wsMonoExpectedSize) {
                return;
            }

            if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                memcpy((void*)frameBuffer, wsMonoAssemblyBuffer, wsMonoAssemblySize);
                frameSize = wsMonoAssemblySize;
                newFrameAvailable = true;
                isMonochrome = true;
                frameRecvTimestamp = wsMonoRecvStartMs;

                xSemaphoreGive(frameMutex);
                // See the color handler's identical comment above.
            } else {
                LOGF("Mono mutex busy - dropped (Recv=%lums)\n", (uint32_t)(millis() - wsMonoRecvStartMs));
                if (client->status() == WS_CONNECTED) {
                    client->text("a");
                }
                frameInFlight = false;
            }
        }
    });

    server.addHandler(&wsMono);

    server.begin();
    LOGLN("Web server started");
}

void setup()
{
    Serial.begin(115200);
    // See webJPEG.cpp's identical line for why this print exists at all - it's not
    // just a log line, it's what keeps firmwareBuildDate from being discarded by the
    // linker's --gc-sections as an unreferenced symbol.
    LOGF("Build marker: %s\n", firmwareBuildDate);
    LOGLN("Starting webJPEG-CYD display...");

    delay(3000);

    tft.init();
    // Native orientation (portrait, 240x320) by default - deliberately never
    // setRotation(1)/(3) for landscape, flash-time-configurable or not. Confirmed on
    // real hardware: setRotation(1) on this board's ILI9341 doesn't actually rotate
    // the panel's addressable window the way TFT_eSPI expects (a real, board-specific
    // quirk, not a config mistake) - pushing a 320x240-shaped frame at rotation 1
    // showed up as portrait content with a corrupted/"noisy" band where the mismatched
    // row width wrapped into the wrong scanlines. 0 and 2 (180deg) share the same
    // underlying addressing mode as each other (only row/column mirroring differs,
    // unlike 1/3 which also swap width/height), so 2 is offered as a "mounted upside
    // down" option - clamped to just these two rather than passing rotationStr's value
    // through directly, since any other value would hit the same broken mode. Use
    // stream.html's Rotation option (90deg/270deg) to get landscape-shaped content
    // onto this board instead of fighting the panel's rotation register - that happens
    // entirely in the browser, so it sidesteps this firmware-level issue completely.
    // See learnings.md.
    tft.setRotation(atoi(rotationStr) == 2 ? 2 : 0);
    tft.invertDisplay(atoi(invertStr) != 0);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

    frameMutex = xSemaphoreCreateMutex();

    // No PSRAM on this board - see the file header comment and MAX_FRAME_SIZE's
    // comment above. Logging free heap before/after helps diagnose a boot that gets
    // this far but then fails the allocation below on a real board, since the actual
    // free-heap budget after WiFi/BT init isn't something that can be verified without
    // hardware in hand.
    LOGF("Free heap before frame buffer allocation: %u bytes\n", ESP.getFreeHeap());
    wsAssemblyBuffer = (uint8_t*)malloc(MAX_FRAME_SIZE);
    wsMonoAssemblyBuffer = (uint8_t*)malloc(MAX_FRAME_SIZE);
    frameBuffer = (volatile uint8_t*)malloc(MAX_FRAME_SIZE);
    LOGF("Free heap after frame buffer allocation: %u bytes\n", ESP.getFreeHeap());
    if (!wsAssemblyBuffer || !wsMonoAssemblyBuffer || !frameBuffer) {
        while (1) {
            LOGLN("Frame buffer allocation failed! (see MAX_FRAME_SIZE's comment - try lowering it)");
            delay(1000);
        }
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("webJPEG-CYD", WIDTH/2, HEIGHT/2 - 20, 2);
    tft.drawString("Starting...", WIDTH/2, HEIGHT/2 + 10, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(buildDateString(), WIDTH/2, HEIGHT/2 + 30, 1);
    tft.setTextDatum(TL_DATUM);
    delay(1000);

    setupWiFi();

    if (WiFi.status() == WL_CONNECTED) {
        setupWebServer();
    }
}

void loop()
{
    static unsigned long lastCheck = 0;

    if (newFrameAvailable) {
        uint32_t startTime = millis();

        if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Captured before isMonochrome gets reset below, so the ack after
            // xSemaphoreGive() still knows which endpoint's client(s) to notify
            // regardless of which branch ran.
            bool wasMono = isMonochrome;

            if (frameBuffer && frameSize > 0) {
                uint8_t* bufPtr = (uint8_t*)frameBuffer;
                size_t bufSize = frameSize;
                bool isMono = isMonochrome;
                uint32_t recvStartMs = frameRecvTimestamp;

                if (isMono) {
                    drawMonoJPEG(bufPtr, bufSize, recvStartMs);
                } else {
                    drawJPEG(bufPtr, bufSize, recvStartMs);
                }

                frameCount++;
                lastFrameTime = millis() - startTime;

                frameSize = 0;
                isMonochrome = false;
            }

            newFrameAvailable = false;
            xSemaphoreGive(frameMutex);

            // Ack now that the frame has actually been rendered - see WS_EVT_DATA's
            // comment above for why this moved here from the WS event handlers.
            // textAll() rather than a cached client pointer, for the same cross-
            // task-safety reason as webRAW.cpp's ACK_NUDGE mechanism.
            if (wasMono) {
                wsMono.textAll("a");
            } else {
                ws.textAll("a");
            }
            frameInFlight = false;
        }
    }

    unsigned long now = millis();
    if (now - lastCheck > 30000) {
        if (frameCount > 0) {
            LOGF("Frames: %lu | Last: %lums | Free heap: %u bytes\n", frameCount, lastFrameTime, ESP.getFreeHeap());
        }
        lastCheck = now;
    }

    vTaskDelay(1);
}
