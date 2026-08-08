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
#include <esp_task_wdt.h>
#include "qrcodegen.h"

// Temporary diagnostic: flip to 1, flash, and look at the screen - no WiFi, no
// streaming, just tft.fillRect() with hardcoded RGB565 values straight after
// tft.init()/setSwapBytes(true). Exists to answer a specific question: distinct
// RGB565 values sent from the browser (packed correctly - verified by hand) were
// reported rendering identically on this board. This bypasses the entire
// browser->WebSocket->deflate->tinfl_decompress_mem_to_mem->pushImage() pipeline,
// going straight from a hardcoded uint16_t to the same low-level TFT_eSPI color-write
// path pushImage() itself uses. If the swatches are STILL indistinguishable here,
// the bug is in TFT_eSPI's config or the panel itself, not in this file's pipeline -
// if they're clearly distinct here, the bug is upstream (packing/compression/
// decompression). Flip back to 0 and reflash once done - this is not meant to stay
// enabled.
#define COLOR_TEST 0

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

// Flash-time-patchable rotation/invert options - see webJPEG-CYD.cpp's identical
// rotationStr/invertStr for the full explanation (byte-for-byte patch mechanism,
// why only 0/180deg are ever applied, and the CYD2USB hardware-inversion background).
const char rotationStr[16] = "|*ROTATION*|";
const char invertStr[16] = "|*INVERT*|";

// Shown on-device (WiFi connect/connected screens) - see webJPEG.cpp's identical
// FW_VARIANT/buildDateString() for the full explanation.
#define FW_VARIANT "WebRAW-CYD"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

// See webJPEG.cpp's identical firmwareBuildDate for the full explanation - read (not
// patched) by the browser flasher to show a build date in the firmware list. Kept
// entirely separate from buildDateString() above (which feeds the on-device LCD via
// tft.drawString()) - an earlier version of this file appended the marker directly
// onto buildDateString()'s own return value, which would have made "|*FW*|" show up
// on the physical screen alongside the date instead of staying confined to the raw
// compiled binary the flasher scans. __attribute__((used)): without it the linker
// strips this as an unused symbol and it never makes it into firmware.bin - confirmed
// the hard way.
const char firmwareBuildDate[32] __attribute__((used)) = "|*FW*|" __DATE__ "|*FW*|";

const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

const char boardName[] = "CYD ESP32-2432S028R (2.8in ILI9341) - RAW";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws-raw");

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
//
// This used to cache the connected client as a raw `AsyncWebSocketClient*`
// (`activeClient`), written from the WS event handler (which runs on AsyncTCP's own
// task) and read+dereferenced from loop() (the separate Arduino main task) with no
// synchronization at all - a genuine cross-task data race on the pointer itself, and
// on the AsyncWebSocketClient object's lifetime (freed on disconnect, potentially
// while loop() was mid-dereference on another core/task). Confirmed as a real crash
// on real hardware: "Mutex busy - frame dropped" immediately followed by a
// LoadProhibited panic inside AsyncWebSocketClient::text()'s internal mutex lock,
// with garbage-looking register values consistent with heap corruption rather than a
// clean null-pointer deref. Fixed by never caching a raw client pointer across tasks
// at all - ws.textAll() below asks the AsyncWebSocket object itself (which manages
// its own client list with proper internal locking) to message whichever client(s)
// are actually still connected, so there's no pointer of ours to go stale.
static const uint32_t ACK_NUDGE_INTERVAL_MS = 2000;

// True from the first byte of a WS message until its ack has actually been sent -
// see webRAW.cpp's identical flag/comment for the full story: acking immediately on
// receipt (the original design here) let a waiting client outrun the ~51ms render
// pipeline, since a send+ack round trip only cost the ~6ms Recv stage - confirmed on
// real hardware as a stable ~72% drop rate across ten repeated runs despite
// "waiting" the whole time. Also gates the ACK_NUDGE resend just below, for the
// same reason as webRAW.cpp's identical guard - without it, a nudge firing mid-
// stream can let the client jump ahead of a still-rendering frame, reintroducing
// the same bug conditionally instead of continuously.
volatile bool frameInFlight = false;

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
            lastDataActivityMs = millis();
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
            // High-water mark (highest byte offset written so far), not `+= len` - a
            // running sum silently desyncs from what's actually in wsAssemblyBuffer if
            // any chunk is ever redelivered/out-of-order/overlapping (AsyncTCP doesn't
            // contractually rule this out), which would let a message with real gaps
            // (stale/garbage bytes sitting unwritten in the middle) look "complete" once
            // the sum coincidentally reaches wsExpectedSize. drawRawStrips() would then
            // read those stale bytes as a strip header - exactly the kind of impossible
            // compressed-length values seen crashing this on real hardware (see
            // learnings.md).
            if (info->index + len > wsAssemblySize) {
                wsAssemblySize = info->index + len;
            }

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
                // No ack here anymore - see loop()'s ack-after-render for why (moved
                // there so "wait mode" backpressures against actual render time, not
                // just network time - see frameInFlight's declaration comment).
            } else {
                LOGF("Mutex busy - frame dropped (Recv=%lums)\n", (uint32_t)(millis() - wsRecvStartMs));
                // Still ack a dropped frame - see webRAW.cpp's identical comment on
                // its own mutex-busy branch for why (mainly for Ack Mode "immediate",
                // which ignores acks anyway but shouldn't be left permanently stalled
                // by this code path either).
                //
                // Guarded by status(): confirmed on real hardware that a client can
                // still have a WS_EVT_DATA callback in flight (already queued/
                // dispatching on the async task) right as the same connection's
                // disconnect is processed - calling client->text() on it then walks
                // into AsyncWebSocketClient::_queueMessage()'s mutex lock on a torn-
                // down object and hard-crashes (LoadProhibited deep in
                // _queueMessage's recursive_mutex lock - see learnings.md for the
                // full backtrace). status() is a plain field read, not a queue/mutex
                // operation, so it's safe to check even this late -
                // AsyncWebSocketClient::_onDisconnect() sets WS_DISCONNECTED before
                // tearing anything else down.
                if (client->status() == WS_CONNECTED) {
                    client->text("a");
                }
                frameInFlight = false;
            }
        }
    });

    server.addHandler(&ws);

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
    LOGLN("Starting webRAW-CYD display...");

    delay(3000);

    tft.init();
    // See webJPEG-CYD.cpp's identical line for why only 0/180deg are ever applied
    // here (90deg/270deg are confirmed broken on this panel, not just unimplemented).
    tft.setRotation(atoi(rotationStr) == 2 ? 2 : 0);
    tft.invertDisplay(atoi(invertStr) != 0);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

#if COLOR_TEST
    // The 4 values reported rendering identically, plus pure R/G/B/white/black as a
    // sanity check that those still look distinct from each other. Each swatch is
    // labeled with its hex value in the browser's raw uint16_t log for one-to-one
    // comparison against what's on screen.
    // First version of this test used tft.fillRect() and found a real (if subtle)
    // difference between these swatches - meaning the panel/driver on its own isn't
    // the bottleneck. But fillRect() and pushImage() are different TFT_eSPI code
    // paths (pushImage() is what drawRawStrips() actually calls, typically
    // DMA-backed) - this version pushes each swatch through a small malloc'd buffer
    // via tft.pushImage(0, y, WIDTH, STRIP_ROWS, buffer), matching drawRawStrips()'s
    // exact call shape, to check whether the bug is specific to that path instead.
    struct { uint16_t color; const char *label; } swatches[] = {
        {0xf75a, "f75a"}, {0xf7bb, "f7bb"}, {0xff59, "ff59"}, {0xefbf, "efbf"},
        {0xF800, "RED"},  {0x07E0, "GRN"},  {0x001F, "BLU"},
        {0xFFFF, "WHT"},  {0x0000, "BLK"},
    };
    const int n = sizeof(swatches) / sizeof(swatches[0]);
    uint16_t *band = (uint16_t*)malloc((size_t)WIDTH * STRIP_ROWS * sizeof(uint16_t));
    if (!band) {
        LOGLN("COLOR_TEST: band buffer allocation failed");
        while (1) { delay(1000); }
    }
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int i = 0; i < n; i++) {
        for (int p = 0; p < WIDTH * STRIP_ROWS; p++) band[p] = swatches[i].color;
        tft.pushImage(0, i * STRIP_ROWS, WIDTH, STRIP_ROWS, band);
        tft.drawString(swatches[i].label, 4, i * STRIP_ROWS + 4, 1);
        LOGF("COLOR_TEST band %d: 0x%04x (%s) at y=%d\n", i, swatches[i].color, swatches[i].label, i * STRIP_ROWS);
    }
    free(band);
    LOGLN("COLOR_TEST: done drawing, halting here (no WiFi, no streaming)");
    while (1) { delay(1000); }
#endif

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

    // Confirmed on real hardware: a heap-corruption assert originating inside
    // AsyncTCP/the WiFi driver's own RX buffer recycling (esf_buf_recycle, called
    // while freeing a pbuf in AsyncClient::_recv - not anywhere in this file's own
    // code) can leave the whole chip hung with no further serial output at all,
    // rather than the clean "print backtrace, then reboot" every other crash in this
    // file's history has shown. Most likely explanation: esp_restart()'s own graceful
    // shutdown tries to stop the WiFi driver cleanly before actually resetting, and
    // that gets stuck waiting on the very structure the corruption already damaged -
    // see learnings.md. The system-wide Interrupt WDT (CONFIG_ESP_INT_WDT, NMI-based)
    // is already compiled in and should catch a true full halt regardless of what
    // this file does, but subscribing loopTask to the Task WDT here too covers the
    // case where loopTask itself ends up blocked waiting on something (a corrupted
    // mutex, say) without the CPU being fully halted - a scenario the idle-task-only
    // default monitoring (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) wouldn't catch on
    // its own, since a blocked (not spinning) task doesn't starve the idle task.
    esp_task_wdt_add(NULL);
}

void loop()
{
    static unsigned long lastCheck = 0;
    esp_task_wdt_reset();

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

            // Ack now that the frame has actually been rendered - see
            // WS_EVT_DATA's comment above for why this moved here from the
            // WS event handler. ws.textAll() rather than a cached client
            // pointer, for the same cross-task-safety reason as the
            // ACK_NUDGE mechanism below (see its comment).
            ws.textAll("a");
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

    // Gated on !frameInFlight - see that flag's comment for why: this must only
    // ever fire when nothing is genuinely still being reassembled or awaiting its
    // render-ack, or it reintroduces a premature, render-independent ack exactly
    // like the bug the ack-after-render fix above was meant to eliminate.
    if (ws.count() > 0 && !frameInFlight && (now - lastDataActivityMs > ACK_NUDGE_INTERVAL_MS)) {
        ws.textAll("a");
        lastDataActivityMs = now;
        LOGLN("RAW: no data received in a while - resending ack in case the last one was lost/delayed");
    }

    vTaskDelay(1);
}
