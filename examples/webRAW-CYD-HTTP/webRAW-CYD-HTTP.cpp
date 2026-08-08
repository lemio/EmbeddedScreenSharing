/**
 * @file      webRAW-CYD-HTTP.cpp
 * @license   MIT
 * @note      Isolated transport experiment - NOT wired into the shared stream.html or
 *            the production examples. Identical to webRAW-CYD.cpp (same strip-based
 *            deflate/ROM-decompress pipeline, same board, same rotation/invert
 *            options) except the WebSocket connection is replaced with a plain HTTP
 *            POST per frame - see docs/../.claude/plans (the transport comparison
 *            this file exists to test) for the full reasoning. Short version: a
 *            confirmed, unresolved heap-corruption crash was traced into
 *            AsyncTCP/the WiFi driver's own buffer recycling under sustained
 *            WebSocket binary traffic - the crash's likely (not proven) origin is
 *            WebSocket-specific frame-parsing/masking code inside AsyncTCP, which a
 *            plain HTTP POST body never touches. This example exists purely to
 *            soak-test that hypothesis on real hardware, using the exact same
 *            per-strip decode logic as webRAW-CYD.cpp so the only variable being
 *            tested is the transport itself.
 *
 *            Protocol: browser POSTs the same wire format webRAW-CYD.cpp already
 *            uses (concatenated [4-byte LE length][deflate bytes] strips) as a raw
 *            `application/octet-stream` body to POST /frame. The HTTP response IS
 *            the ack - 200 once the frame is queued for render, 503 if the render
 *            mutex was busy (previous frame still drawing), 413 if oversized. This
 *            replaces webRAW-CYD.cpp's explicit ack-nudge/stall-watchdog machinery
 *            entirely rather than porting it - HTTP's request/response already
 *            gives a "did you get it" signal WebSocket doesn't have.
 *
 *            Content-Type matters: ESPAsyncWebServer's request body parser treats
 *            `application/x-www-form-urlencoded` and (heuristically) `text/plain`
 *            bodies as form-encoded params, byte-parsed, and never calls the raw
 *            body handler below for them. The browser side must POST with
 *            `application/octet-stream` (or any other content-type that doesn't
 *            match those two cases) or frames will silently never reach
 *            handleFrameBody() at all.
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <esp_task_wdt.h>
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
const char mdnsName[100] = "esp-screen";

// Flash-time-patchable rotation/invert options - see webJPEG-CYD.cpp's identical
// rotationStr/invertStr for the full explanation.
const char rotationStr[16] = "|*ROTATION*|";
const char invertStr[16] = "|*INVERT*|";

#define FW_VARIANT "WebRAW-CYD-HTTP"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

const char firmwareBuildDate[32] __attribute__((used)) = "|*FW*|" __DATE__ "|*FW*|";

const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

const char boardName[] = "CYD ESP32-2432S028R (2.8in ILI9341) - RAW over HTTP (prototype)";

AsyncWebServer server(80);

// Same hardcoded-not-tft.width() reasoning as webJPEG-CYD.cpp.
#define WIDTH  320
#define HEIGHT 240

TFT_eSPI tft = TFT_eSPI(WIDTH, HEIGHT);

// Identical strip sizing to webRAW-CYD.cpp - see that file's header comment for why.
static const int STRIP_ROWS = 24;
static const int STRIPS_PER_FRAME = HEIGHT / STRIP_ROWS;
static const size_t STRIP_DECOMPRESSED_SIZE = (size_t)WIDTH * STRIP_ROWS * 2;

volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile uint32_t frameRecvTimestamp = 0;
SemaphoreHandle_t frameMutex;

// Tried and reverted: an ack-after-render fix analogous to webRAW.cpp/
// webRAW-CYD.cpp's (defer the 200 response until drawRawStrips() actually
// completes in loop(), instead of responding immediately on receipt - real-
// hardware testing confirmed this protocol has the exact same ack-on-receipt bug,
// ~50% of frames dropped despite the design intent of "the HTTP response IS the
// ack"). Implemented by stashing the AsyncWebServerRequest* and calling send()
// later from loop(), guarded by request->onDisconnect() for safety. On real
// hardware this made things worse, not better (0/80 responses reached the client,
// though the frames themselves did render - confirmed via this device's own
// frameCount log). Root cause, from reading WebRequest.cpp:
// AsyncClient::onDisconnect() is one callback slot *per TCP connection*, not per
// request - on a keep-alive connection (which every HTTP client capable of
// sustained streaming will use), a second request arriving before the first one's
// deferred response is sent re-registers that same slot for its own
// AsyncWebServerRequest, silently orphaning the first request's callback and,
// seemingly, its underlying response delivery too. Unlike AsyncWebSocket::
// textAll() (which has its own connection-list-aware, thread-safe fan-out used by
// every other example's ack-after-render fix), ESPAsyncWebServer's per-request
// response path doesn't have an equivalent safe deferred-send primitive - fixing
// this properly would need a different protocol design (e.g. actually rendering
// synchronously inside the handler, at the cost of blocking that request's own
// AsyncTCP callback for the render duration), not this pointer-stash approach.

// Same cap as webRAW-CYD.cpp's MAX_FRAME_SIZE, same reasoning.
static const size_t MAX_FRAME_SIZE = 48 * 1024;

// HTTP request-body reassembly buffer - same role wsAssemblyBuffer played in
// webRAW-CYD.cpp, just fed by ESPAsyncWebServer's body handler instead of
// WS_EVT_DATA. index/len/total from that handler map directly onto the same
// high-water-mark pattern used there (see handleFrameBody() below).
uint8_t* bodyAssemblyBuffer = nullptr;
size_t bodyAssemblySize = 0;
size_t bodyExpectedSize = 0;
uint32_t bodyRecvStartMs = 0;
bool bodyOversized = false;

uint16_t* stripBuffer = nullptr;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

#define LOGF(fmt, ...) Serial.printf("[%10lu] " fmt, millis(), ##__VA_ARGS__)
#define LOGLN(msg) Serial.printf("[%10lu] " msg "\n", millis())

// Identical to webRAW-CYD.cpp's drawRawStrips() - unchanged on purpose, this
// prototype only tests the transport, not the decode/render path.
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

        if (strip % 3 == 2) {
            taskYIELD();
        }
    }

    uint32_t t2 = millis();

    LOGF("RAW Timing: Recv=%lums | Strips=%d/%d | Decompress+Push=%lums | Total=%lums\n",
         t1 - recvStartMs, stripsDrawn, STRIPS_PER_FRAME, t2 - t1, t2 - recvStartMs);
}

// Identical to webRAW-CYD.cpp's drawQRCode().
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

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
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
        tft.drawString("Waiting for POST /frame...", WIDTH / 2, HEIGHT * 0.08 + 92, 2);
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

// Body handler: called once per received chunk of the POST body (possibly several
// times per frame, same as WS_EVT_DATA fragments were). index/len/total map
// directly onto webRAW-CYD.cpp's info->index/len/info->len - same high-water-mark
// reasoning applies (a chunk could in principle be redelivered/reordered; ESPAsync
// WebServer's own body parser is more linear than AsyncWebSocket's frame state
// machine, but there's no documented guarantee against it, so this stays defensive
// rather than assuming away the exact desync class that bit webRAW-CYD.cpp).
void handleFrameBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (index == 0) {
        bodyExpectedSize = total;
        bodyAssemblySize = 0;
        bodyRecvStartMs = millis();
        bodyOversized = (total > MAX_FRAME_SIZE);
    }

    if (bodyOversized) {
        return;
    }

    if (index + len > MAX_FRAME_SIZE) {
        LOGF("Dropping oversized HTTP frame body: %u bytes\n", (unsigned)(index + len));
        bodyOversized = true;
        return;
    }

    memcpy(bodyAssemblyBuffer + index, data, len);
    if (index + len > bodyAssemblySize) {
        bodyAssemblySize = index + len;
    }
}

// Request handler: fires once the full body has been delivered to handleFrameBody()
// above (ESPAsyncWebServer only calls this after _parsedLength == _contentLength -
// confirmed by reading WebRequest.cpp - so bodyAssemblySize is always complete by
// the time this runs).
void handleFrameRequest(AsyncWebServerRequest *request) {
    if (bodyOversized) {
        request->send(413, "text/plain", "oversized");
        bodyOversized = false;
        return;
    }

    if (bodyAssemblySize == 0) {
        request->send(400, "text/plain", "empty");
        return;
    }

    // Response is sent immediately on receipt, not after drawRawStrips() actually
    // renders it in loop() - see the reverted-fix comment above frameBuffer's
    // declaration for why this is deliberate: a deferred, ack-after-render version
    // of this handler was tried and made real-hardware results worse (0% of
    // responses reaching the client, vs this version's real-but-imperfect ~50%
    // drop rate), because ESPAsyncWebServer's per-request response path has no
    // safe cross-task deferred-send equivalent to AsyncWebSocket::textAll(). This
    // known-imperfect drop rate is documented in the benchmarks folder.
    if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
        memcpy((void*)frameBuffer, bodyAssemblyBuffer, bodyAssemblySize);
        frameSize = bodyAssemblySize;
        newFrameAvailable = true;
        frameRecvTimestamp = bodyRecvStartMs;

        xSemaphoreGive(frameMutex);
        request->send(200, "text/plain", "ok");
    } else {
        LOGF("Mutex busy - frame dropped (Recv=%lums)\n", (uint32_t)(millis() - bodyRecvStartMs));
        request->send(503, "text/plain", "busy");
    }
}

void setupWebServer() {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"variant\":\"raw-http\",";
        json += "\"name\":\"" + String(boardName) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        json += "\"boardId\":0,";
        json += "\"maxFrameSize\":" + String(MAX_FRAME_SIZE) + ",";
        json += "\"stripRows\":" + String(STRIP_ROWS);
        json += "}";
        request->send(200, "application/json", json);
    });

    // No shared stream.html redirect - this is an isolated prototype, see the file
    // header. Points at its own standalone test page instead once one exists
    // locally; for now just confirms the board is alive.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "webRAW-CYD-HTTP prototype - POST raw strip data to /frame");
    });

    // See handleFrameBody()'s comment about Content-Type: the browser side MUST
    // send application/octet-stream (or anything other than
    // x-www-form-urlencoded/text-plain-that-looks-like-params), or
    // ESPAsyncWebServer's body parser silently never calls handleFrameBody() at all.
    server.on("/frame", HTTP_POST, handleFrameRequest, nullptr, handleFrameBody);

    server.begin();
    LOGLN("Web server started");
}

void setup()
{
    Serial.begin(115200);
    LOGF("Build marker: %s\n", firmwareBuildDate);
    LOGLN("Starting webRAW-CYD-HTTP (transport prototype) display...");

    delay(3000);

    tft.init();
    tft.setRotation(atoi(rotationStr) == 2 ? 2 : 0);
    tft.invertDisplay(atoi(invertStr) != 0);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

    frameMutex = xSemaphoreCreateMutex();

    LOGF("Free heap before buffer allocation: %u bytes\n", ESP.getFreeHeap());
    bodyAssemblyBuffer = (uint8_t*)malloc(MAX_FRAME_SIZE);
    frameBuffer = (volatile uint8_t*)malloc(MAX_FRAME_SIZE);
    stripBuffer = (uint16_t*)malloc(STRIP_DECOMPRESSED_SIZE);
    LOGF("Free heap after buffer allocation: %u bytes\n", ESP.getFreeHeap());
    if (!bodyAssemblyBuffer || !frameBuffer || !stripBuffer) {
        while (1) {
            LOGLN("Buffer allocation failed! (see MAX_FRAME_SIZE/STRIP_ROWS comments - try lowering them)");
            delay(1000);
        }
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("webRAW-CYD-HTTP", WIDTH/2, HEIGHT/2 - 20, 2);
    tft.drawString("Starting...", WIDTH/2, HEIGHT/2 + 10, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(buildDateString(), WIDTH/2, HEIGHT/2 + 30, 1);
    tft.setTextDatum(TL_DATUM);
    delay(1000);

    setupWiFi();

    if (WiFi.status() == WL_CONNECTED) {
        setupWebServer();
    }

    // See webRAW-CYD.cpp's identical comment - kept here even though this prototype
    // exists specifically to test whether avoiding WebSocket avoids the crash this
    // guards against; if the crash turns out to live in AsyncTCP generally rather
    // than WS-specific code, this is still the same worthwhile safety net.
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
        }
    }

    unsigned long now = millis();
    if (now - lastCheck > 30000) {
        if (frameCount > 0) {
            LOGF("Frames: %lu | Last: %lums | Free heap: %u bytes\n", frameCount, lastFrameTime, ESP.getFreeHeap());
        }
        lastCheck = now;
    }

    // No ack-nudge loop here - there's no persistent connection/ack-in-flight
    // concept to nudge in a plain request/response model. Each POST either gets a
    // response (200/503/413) or the browser's own fetch() timeout/retry logic
    // handles it - nothing for the device to proactively resend.

    vTaskDelay(1);
}
