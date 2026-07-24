/**
 * @file      webH264.cpp
 * @license   MIT
 * @note      Streams a browser tab, window, or camera to a LilyGo AMOLED display over
 *            WiFi as H.264, using the browser's WebCodecs VideoEncoder and Espressif's
 *            esp_h264 (tinyh264) software decoder on-device.
 *
 *            Unlike webJPEG.cpp, this is a .cpp file, not a .ino: esp_h264 ships as a
 *            prebuilt IDF component wired in via the IDF Component Manager, which the
 *            plain Arduino/SCons build can't consume. This example instead builds
 *            Arduino as an ESP-IDF component (see platformio.ini's [env:webH264]:
 *            framework = arduino, espidf), which needs a real CMake project - hence
 *            examples/CMakeLists.txt/idf_component.yml, a root CMakeLists.txt,
 *            sdkconfig.defaults, and partitions.webH264.csv.
 *
 *            Like webJPEG, this auto-detects the attached panel at boot (amoled.begin()
 *            in setup() below) and sizes the decode buffer/display push to match, so
 *            one firmware build works across the whole T-Display-AMOLED lineup. The
 *            browser side (stream.html) reads the detected resolution back from
 *            /boardinfo and configures its VideoEncoder to match.
 *
 * Required libraries:
 * - ESPAsyncWebServer / AsyncTCP (esp32async forks, same as webJPEG)
 * - TFT_eSPI (already a repo-wide dependency, used here only for its Sprite class -
 *   this display has no font/text API of its own)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>
#include "qrcodegen.h"

extern "C" {
#include "h264_decode.h"
}

// WiFi credentials. ssid/password have no sensible default (must be set via the browser
// flasher or before compiling) - see examples/webJPEG/webJPEG.cpp for the full
// explanation of this placeholder convention. mdnsName's default value is itself both a
// working hostname AND the flasher's search key, matching webJPEG's convention.
const char ssid[100] = "|*S*|";
const char password[100] = "|*P*|";
const char mdnsName[100] = "esp-webh264";

// Shown as a QR code so the user can find help/docs if WiFi connection fails
const char githubRepoUrl[] = "https://github.com/lemio/EmbeddedScreenSharing";

LilyGo_Class amoled;

// Actual attached panel's resolution, known only after amoled.begin() in setup() -
// matches webJPEG.cpp's WIDTH/HEIGHT macro convention.
#define DISPLAY_WIDTH  amoled.width()
#define DISPLAY_HEIGHT amoled.height()
AsyncWebServer server(80);
AsyncWebSocket ws("/video-stream");

h264_decoder_t *decoder = NULL;

// Off-screen framebuffer used only for status text/QR code between/around video
// frames; amoled has no text/font API of its own, so we draw into this
// TFT_eSPI sprite (backed by PSRAM, never touches real TFT_eSPI hardware
// pins) and push its pixels the same way we push decoded video frames.
TFT_eSPI tft;
TFT_eSprite statusSpr = TFT_eSprite(&tft);

// Per-frame timings (Recv/Decode/Push/etc.) look consistently good in isolation - what
// they hide is the gap *between* lines, which is where a stall or a WiFi hiccup
// actually shows up. Prefixing every diagnostic line with device uptime makes those
// gaps visible directly in the log instead of having to guess from context. Mirrors
// webJPEG.cpp's LOGF/LOGLN.
#define LOGF(fmt, ...) Serial.printf("[%10lu] " fmt, millis(), ##__VA_ARGS__)
#define LOGLN(msg) Serial.printf("[%10lu] " msg "\n", millis())

// Renders `text` as a QR code into statusSpr, centered at (centerX, centerY),
// scaled as large as possible within a maxSize x maxSize box. Mirrors
// webJPEG.cpp's drawQRCode() exactly, so a failed WiFi connection looks and
// behaves the same on both examples.
#define QR_MAX_VERSION 10
static void drawQRCode(const char *text, int16_t centerX, int16_t centerY, int16_t maxSize, uint16_t fgColor, uint16_t bgColor) {
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

// Connects to WiFi, showing the same spinner/progress-bar/QR-fallback flow as
// webJPEG.cpp's setupWiFi() - same wording, same layout - so the two examples'
// out-of-box setup experience is identical up to the point streaming starts.
static void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    const int maxAttempts = 30;
    const char spinnerFrames[] = {'|', '/', '-', '\\'};
    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        statusSpr.fillSprite(TFT_BLACK);
        statusSpr.setTextDatum(TC_DATUM);

        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString("Connecting to WiFi", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.10, 2);

        statusSpr.setTextColor(TFT_CYAN, TFT_BLACK);
        statusSpr.drawString(ssid, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.10 + 22, 2);

        // Spinner: rotates once per attempt to show the connection is progressing
        char spinner[2] = { spinnerFrames[attempts % 4], '\0' };
        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString(spinner, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.52, 4);

        // Progress bar: fills based on attempts remaining before timeout
        int barWidth = DISPLAY_WIDTH * 0.6;
        int barHeight = 8;
        int barX = (DISPLAY_WIDTH - barWidth) / 2;
        int barY = DISPLAY_HEIGHT - 28;
        statusSpr.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
        int fillWidth = (barWidth - 2) * attempts / maxAttempts;
        statusSpr.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, TFT_CYAN);

        statusSpr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());

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
        statusSpr.setTextColor(TFT_GREEN, TFT_BLACK);
        statusSpr.drawString("WiFi Connected", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.08, 2);
        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString(ssid, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.08 + 22, 2);
        statusSpr.drawString(WiFi.localIP().toString(), DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.08 + 44, 2);
        statusSpr.drawString("http://" + String(mdnsName) + ".local", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.08 + 66, 2);
        statusSpr.setTextColor(TFT_YELLOW, TFT_BLACK);
        statusSpr.drawString("Waiting for stream...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.08 + 92, 2);
        statusSpr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
    } else {
        Serial.println("\nWiFi connection failed!");
        statusSpr.fillSprite(TFT_BLACK);
        statusSpr.setTextDatum(TC_DATUM);

        // Font 1 keeps this long line from overflowing narrower displays
        statusSpr.setTextColor(TFT_RED, TFT_BLACK);
        statusSpr.drawString("Can't connect to WiFi network", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.05, 1);
        statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
        statusSpr.drawString(ssid, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT * 0.05 + 14, 2);

        // QR code links to the project repo so the user can look up setup help
        int textBottom = DISPLAY_HEIGHT * 0.05 + 14 + 18;
        int captionHeight = 16;
        int qrMaxSize = min((int)DISPLAY_WIDTH, (int)DISPLAY_HEIGHT - textBottom - captionHeight) * 0.9;
        int qrCenterY = textBottom + (DISPLAY_HEIGHT - captionHeight - textBottom) / 2;
        drawQRCode(githubRepoUrl, DISPLAY_WIDTH / 2, qrCenterY, qrMaxSize, TFT_WHITE, TFT_BLACK);

        statusSpr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        statusSpr.drawString("Scan for setup help", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT - 16, 2);

        statusSpr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
    }
}

static void setupWebServer() {
    // The streaming page is hosted on GitHub Pages (see the "/" redirect below), so its
    // fetch("/boardinfo") calls back to this device are cross-origin - see
    // examples/webJPEG/webJPEG.cpp for the full explanation of why a wildcard is fine
    // here (read-only board/status info and the streaming WebSocket, nothing sensitive).
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    // Serve board info as JSON - same shape as webJPEG's /boardinfo, plus "variant" so
    // the shared stream.html page can tell the two firmwares apart and adjust its
    // controls/streaming protocol accordingly.
    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"variant\":\"h264\",";
        json += "\"name\":\"" + String(amoled.getName()) + "\",";
        json += "\"width\":" + String(DISPLAY_WIDTH) + ",";
        json += "\"height\":" + String(DISPLAY_HEIGHT) + ",";
        json += "\"boardId\":" + String(amoled.getBoardID());
        json += "}";
        request->send(200, "application/json", json);
    });

    // Redirect to the HTTPS-hosted copy of the streaming page on GitHub Pages - see
    // examples/webJPEG/webJPEG.cpp for why (getDisplayMedia() needs a secure context).
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String url = "https://lemio.github.io/EmbeddedScreenSharing/stream.html?espAddress=http://" + request->host();
        request->redirect(url);
    });

    server.begin();
    LOGLN("Web server started");
}

// AsyncWebSocket delivers large messages (e.g. a keyframe chunk with
// SPS+PPS+IDR bundled in) across multiple WS_EVT_DATA calls rather than one;
// info->index/len mark this fragment's offset within the full message. This
// buffer reassembles those fragments before handing a complete message to
// the decoder. Allocated once at a fixed size and reused for every message
// (never malloc'd/freed per-frame) so hours of streaming can't fragment the
// PSRAM heap - a classic long-running-crash cause on embedded targets.
static const size_t WS_ASSEMBLY_BUF_SIZE = 128 * 1024;
static uint8_t *wsAssemblyBuf = NULL;
static size_t wsAssemblyLen = 0;

// Set on the first fragment of a new message (info->index == 0) so Receive=
// below covers the full reassembly time even when a frame arrives split
// across several WS_EVT_DATA calls, not just the last one.
static uint32_t frameRecvStartMs = 0;
static uint32_t frameCount = 0;

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                       void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    LOGF("Client connected from %s\n", client->remoteIP().toString().c_str());
    // A fresh SPS/PPS/IDR sequence landing on an already-active decode
    // session (e.g. a browser reconnect) can crash the underlying tinyh264
    // decoder, so start every new session from a clean decoder state.
    h264_decode_reset(decoder);
    frameCount = 0;
    statusSpr.fillSprite(TFT_BLACK);
    statusSpr.setTextDatum(MC_DATUM);
    statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
    statusSpr.drawString("Streaming...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, 4);
    statusSpr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
  } else if (type == WS_EVT_DISCONNECT) {
    LOGLN("Client disconnected");
    statusSpr.fillSprite(TFT_BLACK);
    statusSpr.setTextDatum(TC_DATUM);
    statusSpr.setTextColor(TFT_YELLOW, TFT_BLACK);
    statusSpr.drawString("Waiting for stream...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, 2);
    statusSpr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->opcode != WS_BINARY) {
      return;
    }

    if (info->len > WS_ASSEMBLY_BUF_SIZE) {
      LOGF("Dropping oversized WS message: %llu bytes\n", (unsigned long long)info->len);
      return;
    }
    if (info->index == 0) {
      wsAssemblyLen = 0;
      frameRecvStartMs = millis();
    }
    if (!wsAssemblyBuf || info->index + len > info->len || info->index + len > WS_ASSEMBLY_BUF_SIZE) {
      return;
    }
    memcpy(wsAssemblyBuf + info->index, data, len);
    wsAssemblyLen += len;

    if (!info->final || wsAssemblyLen != info->len) {
      return;
    }

    // t0: full message reassembled (may have taken several WS_EVT_DATA calls
    // since frameRecvStartMs). Everything below is timed off this point, so
    // "Recv" below reflects network + reassembly time, not decode time.
    uint32_t t0 = millis();
    uint32_t tDecoded = t0, tGotFrame = t0, tPushed = t0;
    bool gotFrame = false;
    int width = 0, height = 0;

    // h264_decode_parse() does the actual H.264 decode AND the YUV->RGB565
    // colorspace conversion internally (see i420_to_rgb565() in
    // h264_decode.c) - there's no separate conversion step to time here.
    int parseResult = h264_decode_parse(decoder, wsAssemblyBuf, wsAssemblyLen);
    tDecoded = millis();
    uint16_t topLeftPixel = 0;

    if (parseResult == 0) {
      uint8_t *rgb_buffer = NULL;
      if (h264_decode_get_frame(decoder, &rgb_buffer, &width, &height) == 0) {
        tGotFrame = millis();
        if (width <= DISPLAY_WIDTH && height <= DISPLAY_HEIGHT) {
          // h264_decode.c stores pixels byte-swapped for the QSPI panel, so
          // un-swap this one back before logging it as RGB565 below.
          uint16_t rawTopLeft = ((uint16_t *)rgb_buffer)[0];
          topLeftPixel = (rawTopLeft >> 8) | (rawTopLeft << 8);

          amoled.pushColors(0, 0, width, height, (uint16_t *)rgb_buffer);
          tPushed = millis();
          gotFrame = true;
        }
      } else {
        tGotFrame = millis();
      }
    }

    // Tiny flow-control signal: tells the browser we're ready for another
    // frame. All pacing/skip decisions live client-side (see stream.html)
    // so this device only ever does one thing per message: decode, maybe
    // push, ack. No rate tracking, no timers, no state beyond the decoder.
    client->text("a");
    uint32_t tAcked = millis();

    if (gotFrame) {
      frameCount++;
      // RGB565 -> 8-bit-per-channel, scaled up for readability (5/6-bit
      // fields don't span the full 0-255 range on their own).
      uint8_t r8 = ((topLeftPixel >> 11) & 0x1F) * 255 / 31;
      uint8_t g8 = ((topLeftPixel >> 5) & 0x3F) * 255 / 63;
      uint8_t b8 = (topLeftPixel & 0x1F) * 255 / 31;
      LOGF("Frame #%lu (%dx%d, %uB): Recv=%lums | Decode=%lums | GetFrame=%lums | Push=%lums | Ack=%lums | Total=%lums | TopLeft=0x%04X (R=%u G=%u B=%u)\n",
           (unsigned long)frameCount, width, height, (unsigned)wsAssemblyLen,
           (unsigned long)(t0 - frameRecvStartMs),
           (unsigned long)(tDecoded - t0),
           (unsigned long)(tGotFrame - tDecoded),
           (unsigned long)(tPushed - tGotFrame),
           (unsigned long)(tAcked - tPushed),
           (unsigned long)(tAcked - frameRecvStartMs),
           topLeftPixel, r8, g8, b8);
    } else {
      // No displayable frame this message (e.g. only SPS/PPS arrived, no
      // slice yet) - still worth logging Recv/Decode/Ack to see where a
      // stuck-looking stream is actually spending its time.
      LOGF("No frame (%uB): Recv=%lums | Decode=%lums | Ack=%lums | Total=%lums\n",
           (unsigned)wsAssemblyLen,
           (unsigned long)(t0 - frameRecvStartMs),
           (unsigned long)(tDecoded - t0),
           (unsigned long)(tAcked - tDecoded),
           (unsigned long)(tAcked - frameRecvStartMs));
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Auto-detects the attached panel (same as webJPEG.cpp) - probes I2C addresses
  // at boot to tell the 1.47"/1.91"/2.41" boards apart, no compile-time board
  // selection needed. See beginAMOLED_241()'s comment in
  // src/LilyGo_AMOLED.cpp for why begin() disables SD-card init on the 2.41"
  // T4-S3 specifically (mounting one that isn't there panics on boot).
  if (!amoled.begin()) {
    LOGLN("Failed to detect the LilyGo AMOLED panel!");
    while (1) delay(1000);
  }
  amoled.setRotation(0);
  amoled.setBrightness(175);

  statusSpr.setColorDepth(16);
  statusSpr.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);

  decoder = h264_decode_open(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!decoder) {
    LOGLN("Fatal: failed to open H.264 decoder!");
    statusSpr.fillSprite(TFT_BLACK);
    statusSpr.setTextDatum(MC_DATUM);
    statusSpr.setTextColor(TFT_RED, TFT_BLACK);
    statusSpr.drawString("Fatal error", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 14, 4);
    statusSpr.drawString("H.264 decoder failed to open", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 14, 2);
    statusSpr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
    while (1) delay(1000);
  }

  wsAssemblyBuf = (uint8_t *)heap_caps_malloc(WS_ASSEMBLY_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!wsAssemblyBuf) {
    LOGLN("Fatal: failed to allocate WS assembly buffer!");
    statusSpr.fillSprite(TFT_BLACK);
    statusSpr.setTextDatum(MC_DATUM);
    statusSpr.setTextColor(TFT_RED, TFT_BLACK);
    statusSpr.drawString("Fatal error", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 14, 4);
    statusSpr.drawString("Out of memory", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 14, 2);
    statusSpr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
    while (1) delay(1000);
  }

  setupWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    setupWebServer();
  }
}

void loop() {
  ws.cleanupClients();
  // loopTask runs at a higher priority than the idle task and never blocks
  // otherwise, so without this the idle task on this core never gets
  // scheduled and the task watchdog trips continuously.
  delay(1);
}
