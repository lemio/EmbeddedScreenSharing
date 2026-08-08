/**
 * @file      webJPEG.cpp
 * @author    Modified for browser-to-display streaming
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2023-06-14
 * @note      Streams a browser tab, window, or camera to the AMOLED display over WiFi.
 *            This is plain WebSocket + MJPEG (canvas.toBlob), not the WebRTC protocol -
 *            no RTCPeerConnection/SDP/ICE. Frames always render into a sprite buffer
 *            and reach the display in one push - see drawJPEG()'s comments for why.
 *
 * Required libraries:
 * - ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 * - AsyncTCP: https://github.com/me-no-dev/AsyncTCP
 * - JPEGDecoder: https://github.com/Bodmer/JPEGDecoder
 */

#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <JPEGDecoder.h>
#include "qrcodegen.h"

// WiFi credentials. ssid/password have no sensible default (must be set via the browser
// flasher or before compiling); mdnsName's default value doubles as its own flasher
// placeholder, so a board flashed without ever touching the browser flasher still gets
// a working hostname - see flasher-manifest.yml. Shared "esp-screen" default across
// every example in this repo (not just this one) rather than a per-firmware name -
// short, and a board's actual running firmware/build date are now shown on its own
// display (see FW_VARIANT/buildDateString() below), so the hostname no longer needs to
// carry that information itself. Set a custom hostname via the browser flasher or
// wifi_credentials.h if running more than one of these boards on the same network.
const char ssid[100] = "|*S*|";
const char password[100] = "|*P*|";
const char mdnsName[100] = "esp-screen";

// Shown as a QR code so the user can find help/docs if WiFi connection fails
const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

// Shown on-device (WiFi connect/connected screens) so a board's physical display
// alone answers "what firmware is this, and is it stale enough to reflash?" without
// needing serial/computer access. __DATE__ is a standard, always-available compiler
// macro (e.g. "Aug  4 2026" - note the double space for single-digit days, normalized
// by buildDateString() below), refreshed automatically on every build with no manual
// version bump to remember.
#define FW_VARIANT "WebJPEG"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

// Read-only for the browser flasher (see flasher-manifest.yml) - it searches the raw
// compiled firmware.bin for this exact marker and shows everything before it as the
// build date in the firmware list, then strips the marker before displaying. Unlike
// the |*S*|/|*P*| variables above, this is never patched/overwritten - always
// reflects the actual compiled __DATE__, independent of buildDateString()'s own
// on-device-display formatting above. __attribute__((used)) because nothing in this
// program ever reads it (unlike ssid/password below, which survive dead-code
// elimination naturally by being passed to WiFi.begin()) - without it, the linker
// strips this as an unused symbol and it silently never makes it into firmware.bin
// at all (confirmed: it was missing from the compiled binary until this was added).
const char firmwareBuildDate[32] __attribute__((used)) = "|*FW*|" __DATE__ "|*FW*|";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");          // color images
AsyncWebSocket wsMono("/ws-mono"); // monochrome images

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft); // decoded JPEG video frames only - see WIDTH/HEIGHT
// Boot/WiFi/QR status screens only - kept separate from spr because, unlike webRAW.cpp,
// spr here also holds actual decoded video content (see drawJPEG()) which must stay
// sized at the panel's actual current WIDTH/HEIGHT (possibly swapped, see
// statusScreenNeedsRotationFix below) - a status sprite drawn "as landscape" needs its
// own, differently-shaped buffer. See STATUS_WIDTH/STATUS_HEIGHT and
// pushColorsCompensated() in LilyGo_AMOLED.h.
TFT_eSprite statusSpr = TFT_eSprite(&tft);
LilyGo_Class amoled;

#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

// True on the 2.41" T4-S3 board (LILYGO_AMOLED_241) specifically - see webRAW.cpp's
// identical flag/comment for the full story (rotation 1 avoids a diagonal-tearing
// MADCTL bit on full-frame video pushes, at the cost of a swapped/portrait native
// buffer that the browser is responsible for compensating). Set once in setup().
bool statusScreenNeedsRotationFix = false;
#define STATUS_WIDTH  (statusScreenNeedsRotationFix ? HEIGHT : WIDTH)
#define STATUS_HEIGHT (statusScreenNeedsRotationFix ? WIDTH : HEIGHT)

volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile bool isMonochrome = false;
// millis() when the first byte of the frame currently in frameBuffer arrived (set at
// WS_EVT_DATA's info->index==0) - handed off under frameMutex alongside frameBuffer so
// it reflects the frame loop() is about to render, not whatever message the WebSocket
// handler has started assembling since. Lets drawJPEG()/drawMonoJPEG() report a "Recv"
// duration covering network + reassembly time, not just their own decode/render work -
// see learnings.md for why that gap needed a name.
volatile uint32_t frameRecvTimestamp = 0;
SemaphoreHandle_t frameMutex;

// frameBuffer, wsAssemblyBuffer and wsMonoAssemblyBuffer are all allocated once from
// PSRAM in setup() and reused for the life of the program - never malloc()/free()'d
// per frame. No two frames compress to the same JPEG byte count, so allocating a
// fresh buffer for every one of them makes the heap a moving target; over a long
// enough streaming session that fragments badly enough for an allocation to fail,
// which previously took the whole board down (a wedged async_tcp task, then a
// watchdog reset). A fixed-size pool sidesteps that: oversized frames are dropped
// instead of allocated for, and everything else copies into buffers whose addresses
// never change. Mirrors webH264.cpp's wsAssemblyBuf for the same reason.
static const size_t MAX_FRAME_SIZE = 512 * 1024;

// Reassembles fragmented WebSocket frames before they're handed off above
uint8_t* wsAssemblyBuffer = nullptr;
size_t wsAssemblySize = 0;
size_t wsExpectedSize = 0;
uint32_t wsRecvStartMs = 0; // millis() at this message's first fragment (info->index==0)

uint8_t* wsMonoAssemblyBuffer = nullptr;
size_t wsMonoAssemblySize = 0;
size_t wsMonoExpectedSize = 0;
uint32_t wsMonoRecvStartMs = 0;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

// Per-frame timings look consistently good in isolation - what they hide is the gap
// *between* lines, which is where a stall, a WiFi hiccup, or the runup to a crash
// actually shows up. Prefixing every diagnostic line with device uptime makes those
// gaps visible directly in the log instead of having to guess from context.
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

    // True when the JPEG already matches the display size exactly - still worth
    // knowing (a mismatch means the image is being centered/letterboxed), but no
    // longer changes how rendering happens: every frame renders into the sprite and
    // reaches the display in a single push (see the Push step below), rather than the
    // old per-MCU-block direct-to-display path, which was both the dominant cost
    // (many small display-bus transactions) and produced a visible top-to-bottom
    // "unrolling" effect as blocks landed on screen one at a time.
    bool directRender = (w == WIDTH && h == HEIGHT);

    LOGF("JPEG: %dx%d | Display: %dx%d | Direct: %s | MCU: %dx%d\n",
         w, h, WIDTH, HEIGHT, directRender ? "YES" : "NO",
         JpegDec.MCUWidth, JpegDec.MCUHeight);

    // Skippable when directRender: every display pixel gets overwritten by the MCU
    // loop below, so there's nothing left for stale sprite content to show through.
    if (!directRender) {
        spr.fillSprite(TFT_BLACK);
    }

    int16_t offsetX = (WIDTH - w) >> 1;
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;

    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;

    uint32_t t3 = millis();

    // This loop runs ~500 iterations back-to-back with no yield, which was suspected
    // (see learnings.md) of starving the WiFi/TCP stack's own task long enough to
    // delay ACK generation and trigger sender-side retransmission backoff - stalls of
    // hundreds of ms to over a second, invisible in this function's own timing because
    // they happen *before* a frame gets here, not during it. taskYIELD() every 64
    // blocks gives that task a chance to run without the fixed-delay cost of delay().
    uint16_t mcuCount = 0;
    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;

        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        if (mcu_x >= w || mcu_y >= h) continue;

        // Clip this MCU block to the image bounds
        uint16_t valid_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t valid_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        if (valid_w == 0 || valid_h == 0) continue;

        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        if (destX >= WIDTH || destY >= HEIGHT) continue;

        // Clip again to the display bounds
        uint16_t render_w = valid_w;
        uint16_t render_h = valid_h;
        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }
        if (render_w == 0 || render_h == 0) continue;

        // Edge blocks need only their valid portion copied out of the MCU buffer -
        // spr.pushImage() byte-swaps internally, so no manual swap needed here.
        if (render_w != mcu_w || render_h != mcu_h) {
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    tempBuffer[row * render_w + col] = pImg[row * mcu_w + col];
                }
            }
            spr.pushImage(destX, destY, render_w, render_h, tempBuffer);
        } else {
            spr.pushImage(destX, destY, render_w, render_h, pImg);
        }

        if (++mcuCount % 64 == 0) {
            taskYIELD();
        }
    }

    uint32_t t4 = millis();

    amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());

    uint32_t t5 = millis();

    LOGF("Timing: Recv=%lums | Decode=%lums | Setup=%lums | Render=%lums | Push=%lums | Total=%lums\n",
         t1 - recvStartMs, t2-t1, t3-t2, t4-t3, t5-t4, t5-recvStartMs);
}

void drawMonoJPEG(uint8_t *jpegData, size_t jpegSize, uint32_t recvStartMs) {
    uint32_t t1 = millis();

    // Decode JPEG (grayscale)
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
        spr.fillSprite(TFT_BLACK);
    }

    int16_t offsetX = (WIDTH - w) >> 1;
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;

    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;

    uint32_t t3 = millis();

    // See the comment on drawJPEG()'s identical loop for why this yields periodically.
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

        // spr.pushImage() byte-swaps internally, so no manual swap needed here.
        if (render_w != mcu_w || render_h != mcu_h) {
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    tempBuffer[row * render_w + col] = pImg[row * mcu_w + col];
                }
            }
            spr.pushImage(destX, destY, render_w, render_h, tempBuffer);
        } else {
            spr.pushImage(destX, destY, render_w, render_h, pImg);
        }

        if (++mcuCount % 64 == 0) {
            taskYIELD();
        }
    }

    uint32_t t4 = millis();

    amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());

    uint32_t t5 = millis();

    LOGF("Mono Timing: Recv=%lums | Decode=%lums | Setup=%lums | Render=%lums | Push=%lums | Total=%lums\n",
         t1 - recvStartMs, t2-t1, t3-t2, t4-t3, t5-t4, t5-recvStartMs);
}

// Renders `text` as a QR code into the sprite, centered at (centerX, centerY),
// scaled as large as possible within a maxSize x maxSize box.
#define QR_MAX_VERSION 10
void drawQRCode(const char *text, int16_t centerX, int16_t centerY, int16_t maxSize, uint16_t fgColor, uint16_t bgColor) {
    uint8_t qrTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    uint8_t qrOut[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

    bool ok = qrcodegen_encodeText(text, qrTemp, qrOut, qrcodegen_Ecc_MEDIUM,
                                    qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                    qrcodegen_Mask_AUTO, true);
    if (!ok) {
        Serial.println("QR code generation failed!");
        return;
    }

    int size = qrcodegen_getSize(qrOut);
    int quietZoneModules = 2;
    int scale = maxSize / (size + quietZoneModules * 2);
    if (scale < 1) scale = 1;

    int totalSize = (size + quietZoneModules * 2) * scale;
    int startX = centerX - totalSize / 2;
    int startY = centerY - totalSize / 2;

    // Only ever called from the status/WiFi-failure screen below, so this draws into
    // statusSpr, not the video-frame spr - see statusSpr's declaration comment.
    statusSpr.fillRect(startX, startY, totalSize, totalSize, bgColor);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrOut, x, y)) {
                statusSpr.fillRect(startX + (quietZoneModules + x) * scale,
                                    startY + (quietZoneModules + y) * scale,
                                    scale, scale, fgColor);
            }
        }
    }
}

void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    const int maxAttempts = 30;
    const char spinnerFrames[] = {'|', '/', '-', '\\'};
    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        statusSpr.fillSprite(TFT_BLACK);
        statusSpr.setTextDatum(TC_DATUM);

        // What firmware is this, and how stale? Shown on every status screen (not
        // just once at boot) so it's visible the whole time someone might be looking
        // at the physical display, including while troubleshooting a WiFi connect
        // failure - exactly when "is this even the firmware I think it is" matters.
        statusSpr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        statusSpr.drawString(String(FW_VARIANT) + "  " + buildDateString(), STATUS_WIDTH / 2, 4, 1);

        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString("Connecting to WiFi", STATUS_WIDTH / 2, STATUS_HEIGHT * 0.10, 2);

        statusSpr.setTextColor(TFT_CYAN, TFT_BLACK);
        statusSpr.drawString(ssid, STATUS_WIDTH / 2, STATUS_HEIGHT * 0.10 + 22, 2);

        // Spinner: rotates once per attempt to show the connection is progressing
        char spinner[2] = { spinnerFrames[attempts % 4], '\0' };
        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString(spinner, STATUS_WIDTH / 2, STATUS_HEIGHT * 0.52, 4);

        // Progress bar: fills based on attempts remaining before timeout
        int barWidth = STATUS_WIDTH * 0.6;
        int barHeight = 8;
        int barX = (STATUS_WIDTH - barWidth) / 2;
        int barY = STATUS_HEIGHT - 28;
        statusSpr.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
        int fillWidth = (barWidth - 2) * attempts / maxAttempts;
        statusSpr.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, TFT_CYAN);

        statusSpr.setTextDatum(TL_DATUM);
        amoled.pushColorsCompensated(STATUS_WIDTH, STATUS_HEIGHT, (uint16_t *)statusSpr.getPointer());

        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        if (MDNS.begin(mdnsName)) {
            Serial.print("mDNS responder started: ");
            Serial.print(mdnsName);
            Serial.println(".local");
        } else {
            Serial.println("Error setting up mDNS");
        }

        statusSpr.fillSprite(TFT_BLACK);
        statusSpr.setTextDatum(TC_DATUM);
        statusSpr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        statusSpr.drawString(String(FW_VARIANT) + "  " + buildDateString(), STATUS_WIDTH / 2, 4, 1);
        statusSpr.setTextColor(TFT_GREEN, TFT_BLACK);
        statusSpr.drawString("WiFi Connected", STATUS_WIDTH / 2, STATUS_HEIGHT * 0.08, 2);
        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString(ssid, STATUS_WIDTH / 2, STATUS_HEIGHT * 0.08 + 22, 2);
        statusSpr.drawString(WiFi.localIP().toString(), STATUS_WIDTH / 2, STATUS_HEIGHT * 0.08 + 44, 2);
        statusSpr.drawString("http://" + String(mdnsName) + ".local", STATUS_WIDTH / 2, STATUS_HEIGHT * 0.08 + 66, 2);
        statusSpr.setTextColor(TFT_YELLOW, TFT_BLACK);
        statusSpr.drawString("Waiting for stream...", STATUS_WIDTH / 2, STATUS_HEIGHT * 0.08 + 92, 2);
        statusSpr.setTextDatum(TL_DATUM);
        amoled.pushColorsCompensated(STATUS_WIDTH, STATUS_HEIGHT, (uint16_t *)statusSpr.getPointer());
    } else {
        Serial.println("\nWiFi connection failed!");
        statusSpr.fillSprite(TFT_BLACK);
        statusSpr.setTextDatum(TC_DATUM);

        // No variant/build-date line on this screen, unlike the connecting/connected
        // screens above - this one is already tuned tight for the smallest panel
        // (font 1 "to keep this long line from overflowing narrower displays", per the
        // comment below); adding another top line here risks colliding with it on the
        // 1.47" Lite (194px tall) without real hardware to verify the spacing.
        statusSpr.setTextColor(TFT_RED, TFT_BLACK);
        statusSpr.drawString("Can't connect to WiFi network", STATUS_WIDTH / 2, STATUS_HEIGHT * 0.05, 1);
        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString(ssid, STATUS_WIDTH / 2, STATUS_HEIGHT * 0.05 + 14, 2);

        // QR code links to the project repo so the user can look up setup help
        int textBottom = STATUS_HEIGHT * 0.05 + 14 + 18;
        int captionHeight = 16;
        int qrMaxSize = min((int)STATUS_WIDTH, (int)STATUS_HEIGHT - textBottom - captionHeight) * 0.9;
        int qrCenterY = textBottom + (STATUS_HEIGHT - captionHeight - textBottom) / 2;
        drawQRCode(githubRepoUrl, STATUS_WIDTH / 2, qrCenterY, qrMaxSize, TFT_WHITE, TFT_BLACK);

        statusSpr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        statusSpr.drawString("Scan for setup help", STATUS_WIDTH / 2, STATUS_HEIGHT - 16, 2);

        statusSpr.setTextDatum(TL_DATUM);
        amoled.pushColorsCompensated(STATUS_WIDTH, STATUS_HEIGHT, (uint16_t *)statusSpr.getPointer());
    }
}

void setupWebServer() {
    // The streaming page is hosted on GitHub Pages (see the "/" redirect below), so its
    // fetch("/boardinfo") calls back to this device are cross-origin. Every endpoint
    // here is read-only board/status info or the streaming WebSocket - nothing
    // sensitive or mutating - so a wildcard is fine.
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    // Serve board info as JSON - "variant" lets the shared stream.html page tell this
    // firmware apart from webH264's and adjust its controls/streaming protocol
    // accordingly (see examples/webH264/webH264.cpp's matching /boardinfo).
    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"variant\":\"jpeg\",";
        json += "\"name\":\"" + String(amoled.getName()) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        json += "\"boardId\":" + String(amoled.getBoardID()) + ",";
        // Lets stream.html cap JPEG quality/size client-side instead of encoding and
        // sending a frame this device can only ever drop - see MAX_FRAME_SIZE's comment.
        json += "\"maxFrameSize\":" + String(MAX_FRAME_SIZE);
        json += "}";
        request->send(200, "application/json", json);
    });

    // Redirect to the HTTPS-hosted copy of the streaming page on GitHub Pages.
    // getDisplayMedia()/getUserMedia() require a "secure context" - plain http:// on a
    // LAN address or mDNS hostname doesn't qualify (only literal localhost/127.0.0.1
    // do), so serving the streaming UI directly from here would silently fail to offer
    // screen share. See examples/webJPEG/README.md for the tradeoffs this introduces
    // (needs internet access to reach GitHub Pages, and a one-time browser permission
    // for the WebSocket connection back to this device, since it's ws:// not wss://).
    // This same page also serves webH264 - see examples/stream.html.
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
        } else if (type == WS_EVT_DATA) {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;

            // Only binary frames (JPEG images) carry a video frame
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
            }

            if (info->index + len > wsExpectedSize || info->index + len > MAX_FRAME_SIZE) {
                return;
            }

            memcpy(wsAssemblyBuffer + info->index, data, len);
            wsAssemblySize += len;

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
            } else {
                LOGF("Mutex busy - frame dropped (Recv=%lums)\n", (uint32_t)(millis() - wsRecvStartMs));
            }

            // Tiny flow-control signal, same convention as webH264.cpp: any WS message
            // back to the browser means "done with that frame, send another whenever
            // you like" - sent unconditionally (whether the frame rendered or got
            // dropped for the mutex) so the browser can pace itself to what the device
            // can actually keep up with instead of firing blind at a fixed interval.
            // Harmless to clients not using it - stream.html only acts on this when
            // its Ack Mode option is set to wait for it (see that file).
            client->text("a");
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
            }

            if (info->index + len > wsMonoExpectedSize || info->index + len > MAX_FRAME_SIZE) {
                return;
            }

            memcpy(wsMonoAssemblyBuffer + info->index, data, len);
            wsMonoAssemblySize += len;

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
            } else {
                LOGF("Mono mutex busy - dropped (Recv=%lums)\n", (uint32_t)(millis() - wsMonoRecvStartMs));
            }

            // See the color handler's identical comment above.
            client->text("a");
        }
    });

    server.addHandler(&wsMono);


    server.begin();
    LOGLN("Web server started");
}

void setup()
{
    Serial.begin(115200);
    // firmwareBuildDate is otherwise never read anywhere in this program (it exists
    // purely for the browser flasher to find by scanning the raw compiled binary) -
    // __attribute__((used)) alone wasn't enough to survive linking: PlatformIO's
    // -ffunction-sections/-fdata-sections + --gc-sections combo still discarded the
    // whole unreferenced section at link time (confirmed missing from firmware.bin
    // until this was added). A real reference like this keeps it alive - and doubles
    // as a genuinely useful boot-log line.
    LOGF("Build marker: %s\n", firmwareBuildDate);
    LOGLN("Starting webJPEG display...");

    delay(3000);

    if (!amoled.begin()) {
        while (1) {
            LOGLN("Display init failed!");
            delay(1000);
        }
    }

    // See webRAW.cpp's identical rotation-1 comment for the full story - only the
    // 2.41" T4-S3 board (LILYGO_AMOLED_241) has the confirmed diagonal-tearing MADCTL
    // issue at rotation 0, so this is scoped to that board only.
    if (amoled.getBoardID() == LILYGO_AMOLED_241) {
        amoled.setRotation(1);
        statusScreenNeedsRotationFix = true;
    }

    spr.createSprite(WIDTH, HEIGHT);
    spr.setSwapBytes(true);

    statusSpr.createSprite(STATUS_WIDTH, STATUS_HEIGHT);
    statusSpr.setSwapBytes(true);

    frameMutex = xSemaphoreCreateMutex();

    // See the comment above MAX_FRAME_SIZE for why these are allocated once here
    // instead of per-frame.
    wsAssemblyBuffer = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    wsMonoAssemblyBuffer = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    frameBuffer = (volatile uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!wsAssemblyBuffer || !wsMonoAssemblyBuffer || !frameBuffer) {
        while (1) {
            LOGLN("Frame buffer allocation failed!");
            delay(1000);
        }
    }

    statusSpr.fillSprite(TFT_BLACK);
    statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
    statusSpr.setTextDatum(TC_DATUM);
    statusSpr.drawString("webJPEG", STATUS_WIDTH/2, STATUS_HEIGHT/2 - 20, 2);
    statusSpr.drawString("Starting...", STATUS_WIDTH/2, STATUS_HEIGHT/2 + 10, 2);
    statusSpr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    statusSpr.drawString(buildDateString(), STATUS_WIDTH/2, STATUS_HEIGHT/2 + 30, 1);
    statusSpr.setTextDatum(TL_DATUM);
    amoled.pushColorsCompensated(STATUS_WIDTH, STATUS_HEIGHT, (uint16_t *)statusSpr.getPointer());
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
        }
    }

    // Status line every 30 seconds
    unsigned long now = millis();
    if (now - lastCheck > 30000) {
        if (frameCount > 0) {
            LOGF("Frames: %lu | Last: %lums\n", frameCount, lastFrameTime);
        }
        lastCheck = now;
    }

    vTaskDelay(1); // yields so the idle task can run; delay() would block longer than needed
}
