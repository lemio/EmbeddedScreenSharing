/**
 * @file      webRAW.cpp
 * @license   MIT
 * @note      Streams a browser tab, window, or camera to the AMOLED display as raw
 *            RGB565 pixels, deflate-compressed in the browser - no JPEG anywhere in
 *            the pipeline. See webJPEG.cpp for the board setup this reuses unchanged
 *            (WiFi/QR flow, panel auto-detection via amoled.begin(), PSRAM buffer
 *            strategy) and webRAW-CYD.cpp for the wire protocol/decompression
 *            approach this is ported from, first proven on a PSRAM-less board.
 *
 *            Unlike webRAW-CYD (no PSRAM, forced to split every frame into small
 *            strips to keep each allocation within a proven-safe contiguous-
 *            allocation range), this board has PSRAM to spare - a whole decompressed
 *            frame (at most ~527KB across the LilyGo AMOLED lineup, see webJPEG's
 *            README board table) is a trivial allocation here, so this decompresses
 *            and pushes the whole frame in one shot instead of splitting it. Also
 *            unlike the CYD, WIDTH/HEIGHT aren't known until amoled.begin() runs
 *            (this firmware auto-detects the panel like webJPEG.cpp does), which
 *            rules out picking a fixed strip height that evenly divides every
 *            possible board's height anyway - one more reason to skip splitting.
 *
 *            The wire protocol is still byte-compatible with webRAW-CYD's
 *            length-prefixed multi-strip framing, so stream.html's sending code is
 *            shared, unmodified, between both boards: this firmware just reports
 *            stripRows == its own full height via /boardinfo, so the browser's strip
 *            loop runs exactly once per frame instead of ten times.
 *
 *            One real difference from webRAW-CYD: LilyGo_AMOLED::pushColors() writes
 *            pixel bytes straight to the display's SPI bus with no byte-swap of its
 *            own (see LilyGo_AMOLED.cpp). webJPEG.cpp gets correctly-ordered bytes
 *            "for free" because TFT_eSprite::setSwapBytes(true) swaps them as
 *            spr.pushImage() draws into the sprite; this example decompresses
 *            straight into a plain buffer with no sprite involved, so it swaps each
 *            pixel explicitly after decompression instead (see drawRawFrame()'s Swap
 *            stage) - see webJPEG/README.md's "Technical notes" for the same swap
 *            reasoning applied to that example's direct-push case.
 *
 *            Status: not yet verified on real AMOLED hardware (webRAW-CYD was, on
 *            its own separate CYD board - see that example's README for the
 *            real-hardware findings, including a stack-overflow crash fix and an
 *            ack-loss recovery fix, both ported here preemptively since they're
 *            properties of the shared ROM decompressor/ack protocol, not the CYD
 *            specifically. See ACK_NUDGE_INTERVAL_MS and platformio.ini's
 *            ARDUINO_LOOP_STACK_SIZE for [env:webRAW]).
 *
 * Required libraries:
 * - ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 * - AsyncTCP: https://github.com/me-no-dev/AsyncTCP
 * - ESP32 ROM's built-in miniz (tinfl_decompress_mem_to_mem) - no external library
 */

#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include "qrcodegen.h"

extern "C" {
#include "esp32/rom/miniz.h"
}

// WiFi credentials. ssid/password have no sensible default (must be set via the browser
// flasher or before compiling); mdnsName's default value doubles as its own flasher
// placeholder, so a board flashed without ever touching the browser flasher still gets
// a working hostname - see flasher-manifest.yml. This example isn't on the browser
// flasher yet (see README's "Flashing"), so local builds need real credentials some
// other way - wifi_credentials.h is gitignored and, if present, overrides
// WIFI_SSID/WIFI_PASSWORD below; see wifi_credentials.h.example for the format. CI
// (GitHub Actions) never has that file, so it always builds with the "|*S*|"/"|*P*|"
// placeholders intact. Same mechanism as webJPEG-CYD.cpp/webRAW-CYD.cpp - webJPEG.cpp
// itself doesn't need this since it's already on the browser flasher.
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
#define FW_VARIANT "WebRAW"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

// Shown as a QR code so the user can find help/docs if WiFi connection fails
const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws-raw");

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft); // WiFi/QR/boot screens only - see setupWiFi()
LilyGo_Class amoled;

#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

// Tracks the single active viewer so loop() can nudge it directly - see
// ACK_NUDGE_INTERVAL_MS below. Ported from webRAW-CYD.cpp, where real-hardware
// testing found that a lost/delayed ack in "Wait for device ack" mode can otherwise
// stall the browser for seconds with nothing left to recover it - see that example's
// README "Troubleshooting" section for the full real-hardware writeup. The
// mechanism (ESPAsyncWebServer/AsyncTCP's ack send occasionally getting delayed
// under load) isn't CYD-specific, so this is ported here preemptively rather than
// waiting to rediscover the same stall on this board.
AsyncWebSocketClient* activeClient = nullptr;
volatile uint32_t lastDataActivityMs = 0;
static const uint32_t ACK_NUDGE_INTERVAL_MS = 2000;

volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile uint32_t frameRecvTimestamp = 0;
SemaphoreHandle_t frameMutex;

// Caps the *compressed* whole-frame message. PSRAM makes a generous fixed cap cheap
// to allocate outright - see webJPEG.cpp's identical MAX_FRAME_SIZE reasoning (one
// fixed-size pool avoids heap fragmentation from a different-sized allocation every
// frame). 512KB comfortably covers the 600x450 T4-S3 panel's ~527KB raw size even if
// content compresses poorly; smaller panels have proportionally more headroom.
static const size_t MAX_FRAME_SIZE = 512 * 1024;

// Reassembles fragmented WebSocket frames before they're handed off below
uint8_t* wsAssemblyBuffer = nullptr;
size_t wsAssemblySize = 0;
size_t wsExpectedSize = 0;
uint32_t wsRecvStartMs = 0; // millis() at this message's first fragment (info->index==0)

// Decompression target - one whole frame's worth of RGB565 pixels, sized once
// WIDTH/HEIGHT are known from amoled.begin() (they vary across the LilyGo AMOLED
// lineup - see webJPEG.cpp's WIDTH/HEIGHT macros and its README's board table).
// Reused every frame, never resized.
uint16_t* rawPixelBuffer = nullptr;
size_t rawPixelBufferPixels = 0;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

// Per-frame timings look consistently good in isolation - what they hide is the gap
// *between* lines, which is where a stall or WiFi hiccup actually shows up. See
// webJPEG.cpp's identical comment.
#define LOGF(fmt, ...) Serial.printf("[%10lu] " fmt, millis(), ##__VA_ARGS__)
#define LOGLN(msg) Serial.printf("[%10lu] " msg "\n", millis())

// `data` is one reassembled WS message in webRAW-CYD's wire format (N strips, each
// [u32 LE compressed length][compressed bytes]) - N is always 1 here (see the file
// header comment for why), so this reads exactly one length-prefixed blob instead of
// looping.
void drawRawFrame(uint8_t *data, size_t dataSize, uint32_t recvStartMs) {
    uint32_t t1 = millis();

    if (dataSize < 4) {
        LOGLN("RAW: truncated strip header");
        return;
    }
    uint32_t compressedLen = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                              ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    if (4 + (size_t)compressedLen > dataSize) {
        LOGF("RAW: strip length %lu overruns message (only %u bytes left)\n",
             (unsigned long)compressedLen, (unsigned)(dataSize - 4));
        return;
    }

    size_t expectedBytes = rawPixelBufferPixels * 2;

    // flags=0: raw deflate (TINFL_FLAG_PARSE_ZLIB_HEADER not set) - matches the
    // browser's CompressionStream('deflate-raw'), which has no zlib header/trailer
    // to skip. See webRAW-CYD.cpp's identical comment - same ROM function, same
    // flags, confirmed byte-identical between the plain ESP32 and ESP32-S3 SDKs.
    size_t decompressed = tinfl_decompress_mem_to_mem(
        rawPixelBuffer, expectedBytes,
        data + 4, compressedLen, 0);

    uint32_t t2 = millis();

    if (decompressed != expectedBytes) {
        LOGF("RAW: decompress failed (got %u bytes, wanted %u)\n",
             (unsigned)decompressed, (unsigned)expectedBytes);
        return;
    }

    // amoled.pushColors() writes bytes straight to the SPI bus with no swap of its
    // own (see the file header comment) - swap here instead of relying on a
    // TFT_eSprite in between, which this direct-decompress path doesn't have.
    for (size_t i = 0; i < rawPixelBufferPixels; i++) {
        rawPixelBuffer[i] = __builtin_bswap16(rawPixelBuffer[i]);
    }

    uint32_t t3 = millis();

    amoled.pushColors(0, 0, WIDTH, HEIGHT, rawPixelBuffer);

    uint32_t t4 = millis();

    LOGF("RAW Timing: Recv=%lums | Decompress=%lums | Swap=%lums | Push=%lums | Total=%lums\n",
         t1 - recvStartMs, t2 - t1, t3 - t2, t4 - t3, t4 - recvStartMs);
}

// Renders `text` as a QR code into the sprite - identical to webJPEG.cpp's
// drawQRCode(), used only on the WiFi-failure screen.
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

    spr.fillRect(startX, startY, totalSize, totalSize, bgColor);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrOut, x, y)) {
                spr.fillRect(startX + (quietZoneModules + x) * scale,
                             startY + (quietZoneModules + y) * scale,
                             scale, scale, fgColor);
            }
        }
    }
}

// Identical to webJPEG.cpp's setupWiFi() - same spinner/progress-bar boot screen,
// same WiFi connect flow, same QR-code fallback on failure.
void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    const int maxAttempts = 30;
    const char spinnerFrames[] = {'|', '/', '-', '\\'};
    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        spr.fillSprite(TFT_BLACK);
        spr.setTextDatum(TC_DATUM);

        // See webJPEG.cpp's identical line for why this is here.
        spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        spr.drawString(String(FW_VARIANT) + "  " + buildDateString(), WIDTH / 2, 4, 1);

        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString("Connecting to WiFi", WIDTH / 2, HEIGHT * 0.10, 2);

        spr.setTextColor(TFT_CYAN, TFT_BLACK);
        spr.drawString(ssid, WIDTH / 2, HEIGHT * 0.10 + 22, 2);

        char spinner[2] = { spinnerFrames[attempts % 4], '\0' };
        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString(spinner, WIDTH / 2, HEIGHT * 0.52, 4);

        int barWidth = WIDTH * 0.6;
        int barHeight = 8;
        int barX = (WIDTH - barWidth) / 2;
        int barY = HEIGHT - 28;
        spr.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
        int fillWidth = (barWidth - 2) * attempts / maxAttempts;
        spr.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, TFT_CYAN);

        spr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());

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

        spr.fillSprite(TFT_BLACK);
        spr.setTextDatum(TC_DATUM);
        spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        spr.drawString(String(FW_VARIANT) + "  " + buildDateString(), WIDTH / 2, 4, 1);
        spr.setTextColor(TFT_GREEN, TFT_BLACK);
        spr.drawString("WiFi Connected", WIDTH / 2, HEIGHT * 0.08, 2);
        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString(ssid, WIDTH / 2, HEIGHT * 0.08 + 22, 2);
        spr.drawString(WiFi.localIP().toString(), WIDTH / 2, HEIGHT * 0.08 + 44, 2);
        spr.drawString("http://" + String(mdnsName) + ".local", WIDTH / 2, HEIGHT * 0.08 + 66, 2);
        spr.setTextColor(TFT_YELLOW, TFT_BLACK);
        spr.drawString("Waiting for stream...", WIDTH / 2, HEIGHT * 0.08 + 92, 2);
        spr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    } else {
        Serial.println("\nWiFi connection failed!");
        spr.fillSprite(TFT_BLACK);
        spr.setTextDatum(TC_DATUM);

        spr.setTextColor(TFT_RED, TFT_BLACK);
        spr.drawString("Can't connect to WiFi network", WIDTH / 2, HEIGHT * 0.05, 1);
        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString(ssid, WIDTH / 2, HEIGHT * 0.05 + 14, 2);

        int textBottom = HEIGHT * 0.05 + 14 + 18;
        int captionHeight = 16;
        int qrMaxSize = min((int)WIDTH, (int)HEIGHT - textBottom - captionHeight) * 0.9;
        int qrCenterY = textBottom + (HEIGHT - captionHeight - textBottom) / 2;
        drawQRCode(githubRepoUrl, WIDTH / 2, qrCenterY, qrMaxSize, TFT_WHITE, TFT_BLACK);

        spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        spr.drawString("Scan for setup help", WIDTH / 2, HEIGHT - 16, 2);

        spr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    }
}

void setupWebServer() {
    // See webJPEG.cpp's identical comment - every endpoint here is read-only
    // board/status info or the streaming WebSocket, so a wildcard CORS header is fine.
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"variant\":\"raw\",";
        json += "\"name\":\"" + String(amoled.getName()) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        json += "\"boardId\":" + String(amoled.getBoardID()) + ",";
        json += "\"maxFrameSize\":" + String(MAX_FRAME_SIZE) + ",";
        // One strip covering the whole frame - see the file header comment for why -
        // so stream.html's existing per-strip loop (shared with webRAW-CYD) runs
        // exactly once.
        json += "\"stripRows\":" + String(HEIGHT);
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

            // Tiny flow-control ack, unconditional - see webJPEG.cpp's identical
            // comment. Acted on only when stream.html's Ack Mode is "wait".
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
    LOGLN("Starting webRAW display...");

    delay(3000);

    if (!amoled.begin()) {
        while (1) {
            LOGLN("Display init failed!");
            delay(1000);
        }
    }

    spr.createSprite(WIDTH, HEIGHT);
    spr.setSwapBytes(true);

    frameMutex = xSemaphoreCreateMutex();

    rawPixelBufferPixels = (size_t)WIDTH * (size_t)HEIGHT;

    // See MAX_FRAME_SIZE's comment for why these are allocated once here instead of
    // per-frame. Logging free heap before/after helps diagnose a boot that gets this
    // far but then fails the allocation below on a real board.
    LOGF("Free heap before buffer allocation: %u bytes\n", ESP.getFreeHeap());
    wsAssemblyBuffer = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    frameBuffer = (volatile uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    rawPixelBuffer = (uint16_t*)heap_caps_malloc(rawPixelBufferPixels * 2, MALLOC_CAP_SPIRAM);
    LOGF("Free heap after buffer allocation: %u bytes\n", ESP.getFreeHeap());
    if (!wsAssemblyBuffer || !frameBuffer || !rawPixelBuffer) {
        while (1) {
            LOGLN("Frame buffer allocation failed!");
            delay(1000);
        }
    }

    spr.fillSprite(TFT_BLACK);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("webRAW", WIDTH/2, HEIGHT/2 - 20, 2);
    spr.drawString("Starting...", WIDTH/2, HEIGHT/2 + 10, 2);
    spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    spr.drawString(buildDateString(), WIDTH/2, HEIGHT/2 + 30, 1);
    spr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
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

                drawRawFrame(bufPtr, bufSize, recvStartMs);

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

    // See webRAW-CYD.cpp's identical mechanism/comment - resends the ack on our own
    // initiative if nothing's arrived in a while, in case the original ack was the
    // thing that got lost/delayed.
    if (activeClient != nullptr && (now - lastDataActivityMs > ACK_NUDGE_INTERVAL_MS)) {
        activeClient->text("a");
        lastDataActivityMs = now;
        LOGLN("RAW: no data received in a while - resending ack in case the last one was lost/delayed");
    }

    vTaskDelay(1);
}
