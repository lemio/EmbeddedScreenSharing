/**
 * @file      webRAW-CYD.cpp
 * @license   MIT
 * @note      Streams a browser tab, window, or camera to a "Cheap Yellow Display"
 *            (ESP32-2432S028R) over WiFi as raw RGB565 pixels, deflate-compressed in
 *            the browser, instead of JPEG. Same board/setup/ack-protocol conventions
 *            as examples/webJPEG-CYD/webJPEG-CYD.cpp - read that file's header first,
 *            everything there about no-PSRAM/no-webH264/pin-mapping applies unchanged
 *            here. This file only exists to answer one question: is skipping JPEG
 *            (lossy, and - per learnings.md - decode cost dominated by per-MCU-block
 *            call count, not pixel count) worth it if the payload is bigger instead?
 *
 *            Two things make raw pixels viable here despite being ~2-40x bigger than
 *            an equivalent JPEG on the wire:
 *
 *            1. No lossy artifacts - a JPEG's DCT/quantization throws away
 *               information; deflate-compressed raw RGB565 round-trips exactly.
 *               Screen content (flat fills, sharp text edges) is also exactly the
 *               kind of redundant data deflate compresses well, unlike JPEG which
 *               tends to fight sharp edges (ringing artifacts).
 *
 *            2. Fewer, bigger pushes. webJPEG-CYD pushes one 8x8/16x16 MCU block per
 *               `tft.pushImage()` call - hundreds per frame, and learnings.md's
 *               real-hardware numbers show that call count, not pixel count, is what
 *               made render time slow. Decompressed RGB565 has no block-size
 *               constraint, so this pushes whole horizontal strips at once - an
 *               order of magnitude fewer calls for the same frame.
 *
 *            The real cost: a full 320x240 RGB565 frame is 153,600 bytes - far more
 *            than this no-PSRAM board can hold as a single contiguous allocation (see
 *            webJPEG-CYD's learnings.md entries: 64KB already fails). So frames are
 *            split into fixed-size horizontal strips, each compressed independently
 *            by the browser and decompressed+pushed one at a time into a small
 *            reusable buffer - never holding more than one strip's worth of pixels in
 *            RAM. See STRIP_ROWS below for the exact sizing.
 *
 *            **Status: unverified on real hardware at time of writing** - written and
 *            compiled but not yet flashed/tested, unlike webJPEG-CYD which went
 *            through multiple real-hardware iteration rounds. Expect to need some.
 *
 * Required libraries:
 * - ESPAsyncWebServer, AsyncTCP, TFT_eSPI: see webJPEG-CYD.cpp
 * - none for decompression - ESP32's ROM ships a miniz-derived inflate
 *   (`esp32/rom/miniz.h`, `tinfl_decompress_mem_to_mem()`), no external library
 *   needed. This is why this example only works on ESP32 chips whose Arduino core
 *   ships that header - confirmed present for the plain "esp32" target this board
 *   uses.
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include "qrcodegen.h"

extern "C" {
#include "esp32/rom/miniz.h"
}

// See examples/webJPEG-CYD/webJPEG-CYD.cpp for the full explanation of this
// placeholder/override convention - identical here, own wifi_credentials.h (gitignored)
// in this directory, see wifi_credentials.h.example.
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

// Shown on-device (WiFi connect/connected screens) - see webJPEG.cpp's identical
// FW_VARIANT/buildDateString() for the full explanation.
#define FW_VARIANT "WebRAW-CYD"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

const char boardName[] = "CYD ESP32-2432S028R (2.8in ILI9341) - RAW";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws-raw");

// Tracks the single active viewer so loop() can nudge it directly - see
// ACK_NUDGE_INTERVAL_MS below for why.
AsyncWebSocketClient* activeClient = nullptr;
volatile uint32_t lastDataActivityMs = 0;

// If "Wait for device ack" mode's ack (client->text("a") below) is ever lost or
// badly delayed in flight, the browser has nothing else to send until it arrives -
// see stream.html's RAW_ACK_STALL_TIMEOUT_MS (3000ms) recovery path. Rather than
// relying solely on the browser giving up and retrying blind, the device resends its
// ack on its own initiative once it's seen no new data for a while - if the original
// ack simply hasn't arrived yet, this costs nothing (the browser ignores a second
// ack once it's already unblocked); if it was actually lost, this is what unblocks
// the browser at all, since the browser's own stall timer only fires after this
// (2000ms < the browser's 3000ms), so a legitimate resend normally wins that race
// instead of the browser forcing a blind retry.
static const uint32_t ACK_NUDGE_INTERVAL_MS = 2000;

// Same hardcoded-not-tft.width() reasoning as webJPEG-CYD.cpp - see that file's
// comment above its own WIDTH/HEIGHT and learnings.md item 6.
#define WIDTH  320
#define HEIGHT 240

TFT_eSPI tft = TFT_eSPI(WIDTH, HEIGHT);

// Frame is split into fixed-height horizontal strips, each an independent deflate
// stream (no cross-strip back-references) - see the file header comment for why. 24
// divides HEIGHT (240) exactly, so every strip is the same size and there's no
// partial-strip edge case to handle. Each strip is 320*24*2 = 15,360 bytes
// decompressed - small enough to comfortably fit this board's proven-safe
// contiguous-allocation range (see webJPEG-CYD's learnings.md: 40KB confirmed
// working, 64KB confirmed failing).
static const int STRIP_ROWS = 24;
static const int STRIPS_PER_FRAME = HEIGHT / STRIP_ROWS;
static const size_t STRIP_DECOMPRESSED_SIZE = (size_t)WIDTH * STRIP_ROWS * 2;

volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile uint32_t frameRecvTimestamp = 0;
SemaphoreHandle_t frameMutex;

// Caps the *compressed* whole-frame message (all STRIPS_PER_FRAME strips
// concatenated, each prefixed with a 4-byte length - see readStrip() below). Sized
// well above what real screen content should need (deflate handles flat
// fills/repeated pixels - exactly what UI/screen-share content mostly is - very
// well) but far below the 153,600-byte raw size, so a pathologically incompressible
// frame gets dropped (same oversized-frame handling as webJPEG-CYD) rather than
// risking an allocation this board can't make. Not yet measured against real
// compressed sizes on hardware - see the file header's "Status" note.
static const size_t MAX_FRAME_SIZE = 48 * 1024;

uint8_t* wsAssemblyBuffer = nullptr;
size_t wsAssemblySize = 0;
size_t wsExpectedSize = 0;
uint32_t wsRecvStartMs = 0;

// Reused for every strip of every frame - decompression target for
// tinfl_decompress_mem_to_mem(), then pushed to the display and overwritten by the
// next strip. Never holds more than one strip at a time.
uint16_t* stripBuffer = nullptr;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

#define LOGF(fmt, ...) Serial.printf("[%10lu] " fmt, millis(), ##__VA_ARGS__)
#define LOGLN(msg) Serial.printf("[%10lu] " msg "\n", millis())

// Decompresses and pushes each strip in `data` (the reassembled WS message: 10
// strips, each [u32 LE compressed length][compressed bytes]) directly to the
// display. No intermediate full-frame buffer - see the file header comment for why.
void drawRawStrips(uint8_t *data, size_t dataSize, uint32_t recvStartMs) {
    uint32_t t1 = millis();

    size_t offset = 0;
    int stripsDrawn = 0;

    for (int strip = 0; strip < STRIPS_PER_FRAME; strip++) {
        if (offset + 4 > dataSize) {
            LOGF("RAW: truncated strip header at strip %d/%d\n", strip, STRIPS_PER_FRAME);
            break;
        }
        uint32_t compressedLen = (uint32_t)data[offset] | ((uint32_t)data[offset+1] << 8) |
                                  ((uint32_t)data[offset+2] << 16) | ((uint32_t)data[offset+3] << 24);
        offset += 4;

        if (offset + compressedLen > dataSize) {
            LOGF("RAW: strip %d/%d length %lu overruns message (only %u bytes left)\n",
                 strip, STRIPS_PER_FRAME, (unsigned long)compressedLen, (unsigned)(dataSize - offset));
            break;
        }

        // flags=0: raw deflate (TINFL_FLAG_PARSE_ZLIB_HEADER not set - matches the
        // browser's CompressionStream('deflate-raw'), which has no zlib header/
        // trailer to skip).
        size_t decompressed = tinfl_decompress_mem_to_mem(
            stripBuffer, STRIP_DECOMPRESSED_SIZE,
            data + offset, compressedLen, 0);

        if (decompressed != STRIP_DECOMPRESSED_SIZE) {
            LOGF("RAW: strip %d/%d decompress failed (got %u bytes, wanted %u)\n",
                 strip, STRIPS_PER_FRAME, (unsigned)decompressed, (unsigned)STRIP_DECOMPRESSED_SIZE);
            offset += compressedLen;
            continue;
        }

        tft.pushImage(0, strip * STRIP_ROWS, WIDTH, STRIP_ROWS, stripBuffer);
        stripsDrawn++;
        offset += compressedLen;

        // Same reasoning as webJPEG-CYD's MCU loop: yield periodically so this
        // multi-strip loop doesn't monopolize the CPU long enough to starve the
        // WiFi/TCP stack's own task.
        if (strip % 3 == 2) {
            taskYIELD();
        }
    }

    uint32_t t2 = millis();

    LOGF("RAW Timing: Recv=%lums | Strips=%d/%d | Decompress+Push=%lums | Total=%lums\n",
         t1 - recvStartMs, stripsDrawn, STRIPS_PER_FRAME, t2 - t1, t2 - recvStartMs);
}

// Renders `text` as a QR code - identical to webJPEG-CYD.cpp's drawQRCode(), no
// sprite buffer, draws straight to tft. See that file for why.
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

    // Drawn once, not every attempt - see webJPEG-CYD.cpp's identical comment for
    // why (avoids a full-screen flicker with no sprite buffer to hide it behind).
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
        json += "\"variant\":\"raw\",";
        json += "\"name\":\"" + String(boardName) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        json += "\"boardId\":0,";
        json += "\"maxFrameSize\":" + String(MAX_FRAME_SIZE) + ",";
        json += "\"stripRows\":" + String(STRIP_ROWS);
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
            activeClient = client;
            lastDataActivityMs = millis();
        } else if (type == WS_EVT_DISCONNECT) {
            LOGF("WebSocket client #%u disconnected\n", client->id());
            wsAssemblySize = 0;
            wsExpectedSize = 0;
            if (activeClient == client) {
                activeClient = nullptr;
            }
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
            }

            if (info->index + len > wsExpectedSize || info->index + len > MAX_FRAME_SIZE) {
                return;
            }

            memcpy(wsAssemblyBuffer + info->index, data, len);
            wsAssemblySize += len;

            if (!info->final || wsAssemblySize != wsExpectedSize) {
                return;
            }

            lastDataActivityMs = millis();

            if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                memcpy((void*)frameBuffer, wsAssemblyBuffer, wsAssemblySize);
                frameSize = wsAssemblySize;
                newFrameAvailable = true;
                frameRecvTimestamp = wsRecvStartMs;

                xSemaphoreGive(frameMutex);
            } else {
                LOGF("Mutex busy - frame dropped (Recv=%lums)\n", (uint32_t)(millis() - wsRecvStartMs));
            }

            // See webJPEG.cpp's identical comment - tiny flow-control ack,
            // unconditional, acted on only when stream.html's Ack Mode is "wait".
            client->text("a");
        }
    });

    server.addHandler(&ws);

    server.begin();
    LOGLN("Web server started");
}

void setup()
{
    Serial.begin(115200);
    LOGLN("Starting webRAW-CYD display...");

    delay(3000);

    tft.init();
    tft.setRotation(0);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

    frameMutex = xSemaphoreCreateMutex();

    LOGF("Free heap before buffer allocation: %u bytes\n", ESP.getFreeHeap());
    wsAssemblyBuffer = (uint8_t*)malloc(MAX_FRAME_SIZE);
    frameBuffer = (volatile uint8_t*)malloc(MAX_FRAME_SIZE);
    stripBuffer = (uint16_t*)malloc(STRIP_DECOMPRESSED_SIZE);
    LOGF("Free heap after buffer allocation: %u bytes\n", ESP.getFreeHeap());
    if (!wsAssemblyBuffer || !frameBuffer || !stripBuffer) {
        while (1) {
            LOGLN("Buffer allocation failed! (see MAX_FRAME_SIZE/STRIP_ROWS comments - try lowering them)");
            delay(1000);
        }
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("webRAW-CYD", WIDTH/2, HEIGHT/2 - 20, 2);
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

            if (frameBuffer && frameSize > 0) {
                uint8_t* bufPtr = (uint8_t*)frameBuffer;
                size_t bufSize = frameSize;
                uint32_t recvStartMs = frameRecvTimestamp;

                drawRawStrips(bufPtr, bufSize, recvStartMs);

                frameCount++;
                lastFrameTime = millis() - startTime;

                frameSize = 0;
            }

            newFrameAvailable = false;
            xSemaphoreGive(frameMutex);
        }
    }

    unsigned long now = millis();
    if (now - lastCheck > 30000) {
        if (frameCount > 0) {
            LOGF("Frames: %lu | Last: %lums | Free heap: %u bytes\n", frameCount, lastFrameTime, ESP.getFreeHeap());
        }
        lastCheck = now;
    }

    if (activeClient != nullptr && (now - lastDataActivityMs > ACK_NUDGE_INTERVAL_MS)) {
        activeClient->text("a");
        lastDataActivityMs = now;
        LOGLN("RAW: no data received in a while - resending ack in case the last one was lost/delayed");
    }

    vTaskDelay(1);
}
