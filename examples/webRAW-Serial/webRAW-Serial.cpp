/**
 * @file      webRAW-Serial.cpp
 * @license   MIT
 * @note      Isolated transport experiment - NOT wired into the shared stream.html or
 *            the production examples. Identical decode/render pipeline to
 *            webRAW.cpp (same whole-frame deflate/ROM-decompress approach, same
 *            AMOLED board/panel auto-detection, same byte-swap-on-decompress
 *            reasoning) except the WiFi/WebSocket connection is replaced entirely
 *            with USB Serial (native USB CDC on this board - see platformio.ini's
 *            ARDUINO_USB_CDC_ON_BOOT) - see docs/../.claude/plans (the transport
 *            comparison this file exists to test) for the full reasoning. Short
 *            version: a confirmed, unresolved heap-corruption crash was traced into
 *            AsyncTCP/the WiFi driver's own buffer recycling under sustained
 *            WebSocket binary traffic. Serial never touches AsyncTCP, lwIP, or the
 *            WiFi driver at all, so that exact crash class has no code path to occur
 *            through here - this example exists purely to soak-test that on real
 *            hardware, using the exact same decode/render logic as webRAW.cpp so the
 *            only variable being tested is the transport itself.
 *
 *            No WiFi at all in this file - deliberately dropped, not just unused.
 *            This board has to stay physically tethered by USB to the same computer
 *            running the browser tab for the whole streaming session, so there's no
 *            "connect to WiFi" step, no mDNS, no QR-code-on-failure screen - none of
 *            that setup flow applies to a wired connection.
 *
 *            Protocol: browser writes `[4-byte magic FRAME_MAGIC][4-byte LE
 *            compressed length][that many bytes of raw deflate data]` directly over
 *            the serial connection, one frame after another, no other framing - the
 *            magic+length+body layout (once past the magic) is exactly the same byte
 *            layout webRAW.cpp's drawRawFrame() already expects as a whole
 *            reassembled WS message, so reusing that decode logic here needed no
 *            format changes, just a different place to read the bytes from and a
 *            marker in front. The device writes a single 'a' byte back after each
 *            frame is queued (or dropped for mutex-busy) - same ack convention as
 *            every other example in this repo, just over Serial instead of a WS text
 *            message.
 *
 *            The FRAME_MAGIC marker (added after this was originally "no resync
 *            mechanism at all, unlike WiFi a wired link shouldn't need one") exists
 *            because real-world testing found that assumption wrong in practice: if
 *            the device's byte-stream alignment is ever lost for any reason (e.g. a
 *            browser tab closing mid-frame leaves the device expecting more body
 *            bytes than ever arrive, and the next session's fresh header bytes get
 *            read as leftover body instead), a bare length-prefix protocol has no
 *            way back except guessing - re-interpreting arbitrary 4-byte windows as
 *            a new header and hoping one happens to look plausible. That can take an
 *            unpredictable, sometimes very long time (confirmed on real hardware:
 *            over a second of continuous "declared frame size ... exceeds buffer"
 *            noise from a single misaligned session, never recovering within the
 *            observed window). A fixed 4-byte magic value that the parser searches
 *            for byte-by-byte (not 4-byte jumps) finds a genuine resync point
 *            deterministically, with only a ~1-in-4-billion chance of a false match
 *            against real payload data at any given position.
 */

#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>

extern "C" {
#include "esp32/rom/miniz.h"
}

#define FW_VARIANT "WebRAW-Serial"
String buildDateString() {
    String d = __DATE__;
    d.replace("  ", " ");
    return d;
}

const char firmwareBuildDate[32] __attribute__((used)) = "|*FW*|" __DATE__ "|*FW*|";

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft); // Boot screen only
LilyGo_Class amoled;

#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

// True on the 2.41" T4-S3 board (LILYGO_AMOLED_241) specifically - see webRAW.cpp's
// identical flag/comment for the full story (rotation 1 avoids a diagonal-tearing
// MADCTL bit on full-frame video pushes, at the cost of a swapped/portrait native
// buffer). This prototype has no browser to compensate for the video path the way
// webRAW.cpp's does (Web Serial, not stream.html), but the boot screen still needs
// STATUS_WIDTH/STATUS_HEIGHT + pushColorsCompensated() to stay landscape-readable.
bool statusScreenNeedsRotationFix = false;
#define STATUS_WIDTH  (statusScreenNeedsRotationFix ? HEIGHT : WIDTH)
#define STATUS_HEIGHT (statusScreenNeedsRotationFix ? WIDTH : HEIGHT)

volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile uint32_t frameRecvTimestamp = 0;
SemaphoreHandle_t frameMutex;

// Same cap/PSRAM reasoning as webRAW.cpp's MAX_FRAME_SIZE.
static const size_t MAX_FRAME_SIZE = 512 * 1024;

// Serial reassembly buffer - same role wsAssemblyBuffer played in webRAW.cpp, just
// filled by serviceSerialInput() below instead of WS_EVT_DATA.
uint8_t* rxAssemblyBuffer = nullptr;

uint16_t* rawPixelBuffer = nullptr;
size_t rawPixelBufferPixels = 0;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

#define LOGF(fmt, ...) Serial.printf("[%10lu] " fmt, millis(), ##__VA_ARGS__)
#define LOGLN(msg) Serial.printf("[%10lu] " msg "\n", millis())

// Identical to webRAW.cpp's drawRawFrame() - unchanged on purpose, this prototype
// only tests the transport, not the decode/render path.
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

    size_t decompressed = tinfl_decompress_mem_to_mem(
        rawPixelBuffer, expectedBytes,
        data + 4, compressedLen, 0);

    uint32_t t2 = millis();

    if (decompressed != expectedBytes) {
        LOGF("RAW: decompress failed (got %u bytes, wanted %u)\n",
             (unsigned)decompressed, (unsigned)expectedBytes);
        return;
    }

    for (size_t i = 0; i < rawPixelBufferPixels; i++) {
        rawPixelBuffer[i] = __builtin_bswap16(rawPixelBuffer[i]);
    }

    uint32_t t3 = millis();

    amoled.pushColors(0, 0, WIDTH, HEIGHT, rawPixelBuffer);

    uint32_t t4 = millis();

    LOGF("RAW Timing: Recv=%lums | Decompress=%lums | Swap=%lums | Push=%lums | Total=%lums\n",
         t1 - recvStartMs, t2 - t1, t3 - t2, t4 - t3, t4 - recvStartMs);
}

// Non-blocking Serial read state machine - reads whatever's already available each
// call, never waits. Called every loop() iteration. See the file header's "Known
// simplification" note for what this deliberately doesn't handle.
enum SerialRxState { RX_SYNC, RX_LENGTH, RX_BODY };
SerialRxState rxState = RX_SYNC;

// Searched for byte-by-byte (a sliding 4-byte window, not 4-byte jumps) so a
// genuine resync point is always found deterministically no matter where in the
// byte stream the search begins - see the file header comment for the real-
// hardware failure this replaced (a bare length-prefix protocol's only recovery
// from lost alignment was guessing, which could take an unpredictable, sometimes
// very long time). Any fixed 4-byte value works equally well here - what matters
// is both ends agreeing on it, not the specific bytes.
static const uint8_t FRAME_MAGIC[4] = {0xAA, 0x55, 0xAA, 0x55};
uint8_t syncWindow[4] = {0, 0, 0, 0};

uint8_t headerBuf[4];
size_t headerBytesRead = 0;
uint32_t bodyExpectedLen = 0;
size_t bodyBytesRead = 0;
uint32_t frameRecvStartMs = 0;

void serviceSerialInput() {
    while (Serial.available() > 0) {
        if (rxState == RX_SYNC) {
            uint8_t b = (uint8_t)Serial.read();
            syncWindow[0] = syncWindow[1];
            syncWindow[1] = syncWindow[2];
            syncWindow[2] = syncWindow[3];
            syncWindow[3] = b;
            if (memcmp(syncWindow, FRAME_MAGIC, 4) == 0) {
                rxState = RX_LENGTH;
                headerBytesRead = 0;
            }
        } else if (rxState == RX_LENGTH) {
            headerBuf[headerBytesRead++] = (uint8_t)Serial.read();
            if (headerBytesRead == 4) {
                bodyExpectedLen = (uint32_t)headerBuf[0] | ((uint32_t)headerBuf[1] << 8) |
                                  ((uint32_t)headerBuf[2] << 16) | ((uint32_t)headerBuf[3] << 24);
                if (bodyExpectedLen > MAX_FRAME_SIZE - 4) {
                    LOGF("SERIAL: declared frame size %lu exceeds buffer - resuming magic search\n",
                         (unsigned long)bodyExpectedLen);
                    // Fall back to searching for the next magic marker, not the
                    // next raw 4 bytes as a fresh header attempt - blindly guessing
                    // like that was exactly the failure mode FRAME_MAGIC replaces.
                    // A length this large right after a genuine magic match should
                    // be rare (real corruption, not just routine desync), so this
                    // path existing at all is mostly defensive.
                    rxState = RX_SYNC;
                    syncWindow[0] = syncWindow[1] = syncWindow[2] = syncWindow[3] = 0;
                    continue;
                }
                memcpy(rxAssemblyBuffer, headerBuf, 4);
                bodyBytesRead = 0;
                frameRecvStartMs = millis();
                rxState = RX_BODY;
            }
        } else { // RX_BODY
            size_t avail = (size_t)Serial.available();
            size_t remaining = bodyExpectedLen - bodyBytesRead;
            size_t toRead = avail < remaining ? avail : remaining;
            size_t actuallyRead = Serial.readBytes(rxAssemblyBuffer + 4 + bodyBytesRead, toRead);

            // Scan what was just read for a spurious FRAME_MAGIC match, in case this
            // "body" is actually the start of a fresh frame from a new session, not
            // a continuation of this one - the scenario FRAME_MAGIC exists for in the
            // first place (a session ending mid-frame leaves the device expecting
            // body bytes that will never come). Without this check, recovery would
            // only happen once that stuck, potentially large declared length was
            // somehow fully "consumed" by whatever unrelated bytes arrive next -
            // confirmed on real hardware to cost several seconds. A false-positive
            // match within genuine body data is possible but rare (~1-in-4-billion
            // per byte position) and self-limiting even when it happens - the
            // aborted frame just fails decompression cleanly on the next attempt and
            // gets logged/skipped, not a lasting problem - a good trade against
            // potentially hanging for an entire bogus declared length.
            bool foundFreshSync = false;
            for (size_t i = 0; i < actuallyRead && !foundFreshSync; i++) {
                uint8_t b = rxAssemblyBuffer[4 + bodyBytesRead + i];
                syncWindow[0] = syncWindow[1];
                syncWindow[1] = syncWindow[2];
                syncWindow[2] = syncWindow[3];
                syncWindow[3] = b;
                if (memcmp(syncWindow, FRAME_MAGIC, 4) == 0) {
                    foundFreshSync = true;
                }
            }

            if (foundFreshSync) {
                LOGLN("SERIAL: found a fresh sync marker mid-body - abandoning stuck frame and resyncing");
                rxState = RX_LENGTH;
                headerBytesRead = 0;
                syncWindow[0] = syncWindow[1] = syncWindow[2] = syncWindow[3] = 0;
                continue;
            }

            bodyBytesRead += actuallyRead;

            if (bodyBytesRead >= bodyExpectedLen) {
                size_t totalSize = 4 + bodyExpectedLen;

                if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                    memcpy((void*)frameBuffer, rxAssemblyBuffer, totalSize);
                    frameSize = totalSize;
                    newFrameAvailable = true;
                    frameRecvTimestamp = frameRecvStartMs;

                    xSemaphoreGive(frameMutex);
                } else {
                    LOGF("Mutex busy - frame dropped (Recv=%lums)\n", (uint32_t)(millis() - frameRecvStartMs));
                }

                // Same ack convention as every WS-based example's client->text("a") -
                // any byte back means "done with that frame, send another whenever
                // you like."
                Serial.write('a');

                rxState = RX_SYNC;
                syncWindow[0] = syncWindow[1] = syncWindow[2] = syncWindow[3] = 0;
            }
        }
    }
}

void setup()
{
    // Default HWCDC RX queue is only 256 bytes (confirmed by reading
    // HWCDC.cpp - it's a byte-at-a-time FreeRTOS queue, not a real ring buffer) -
    // far too small for multi-KB frame bursts arriving faster than loop() drains
    // them. Must be set before begin().
    //
    // 16384 (the first value tried here) was NOT enough - confirmed on real
    // hardware: solid-fill test frames (~1.5KB compressed) worked fine, but
    // realistic animated-gradient frames (~33KB compressed - far less
    // compressible than a flat fill) corrupted the header framing entirely
    // ("declared frame size 1590744751 exceeds buffer"-style garbage). Root
    // cause: drawRawFrame() blocks loop() synchronously for ~80ms on every
    // single frame, during which serviceSerialInput() never runs at all - a
    // 33KB burst (observed arriving in ~61ms, since native USB CDC doesn't
    // actually rate-limit to the nominal "baud rate" the way a real UART would)
    // can overflow a 16KB queue entirely within that one blocked loop()
    // iteration, silently dropping bytes mid-frame and desyncing every header
    // after it. 65536 gives ~2x margin over the worst compressed size seen so
    // far - not sized to MAX_FRAME_SIZE (512KB) itself, since that's a
    // generous compile-time ceiling, not a realistic target, and this board's
    // internal SRAM (used for this queue - unlike rxAssemblyBuffer/frameBuffer
    // below, which are PSRAM-backed) is a much tighter, shared budget.
    Serial.setRxBufferSize(65536);
    Serial.begin(115200);
    LOGF("Build marker: %s\n", firmwareBuildDate);
    LOGLN("Starting webRAW-Serial (transport prototype) display...");

    delay(3000);

    if (!amoled.begin()) {
        while (1) {
            LOGLN("Display init failed!");
            delay(1000);
        }
    }

    // See webRAW.cpp's identical rotation-1 comment for the full story - only the
    // 2.41" T4-S3 board (LILYGO_AMOLED_241) has the confirmed diagonal-tearing MADCTL
    // issue at rotation 0, so this is scoped to that board only. Unlike webRAW.cpp,
    // this isolated Serial-transport prototype has no browser (stream.html) in the
    // loop to compensate the swapped WIDTH/HEIGHT for actual video content -
    // serial-test.html would need its own compensating rotation to display correctly
    // oriented video on this board; only the boot screen below is fixed up here.
    if (amoled.getBoardID() == LILYGO_AMOLED_241) {
        amoled.setRotation(1);
        statusScreenNeedsRotationFix = true;
    }

    spr.createSprite(STATUS_WIDTH, STATUS_HEIGHT);
    spr.setSwapBytes(true);

    frameMutex = xSemaphoreCreateMutex();

    rawPixelBufferPixels = (size_t)WIDTH * (size_t)HEIGHT;

    LOGF("Free heap before buffer allocation: %u bytes\n", ESP.getFreeHeap());
    rxAssemblyBuffer = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    frameBuffer = (volatile uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    rawPixelBuffer = (uint16_t*)heap_caps_malloc(rawPixelBufferPixels * 2, MALLOC_CAP_SPIRAM);
    LOGF("Free heap after buffer allocation: %u bytes\n", ESP.getFreeHeap());
    if (!rxAssemblyBuffer || !frameBuffer || !rawPixelBuffer) {
        while (1) {
            LOGLN("Frame buffer allocation failed!");
            delay(1000);
        }
    }

    spr.fillSprite(TFT_BLACK);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("webRAW-Serial", STATUS_WIDTH/2, STATUS_HEIGHT/2 - 20, 2);
    spr.drawString("Waiting for USB data...", STATUS_WIDTH/2, STATUS_HEIGHT/2 + 10, 2);
    spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    spr.drawString(buildDateString(), STATUS_WIDTH/2, STATUS_HEIGHT/2 + 30, 1);
    spr.setTextDatum(TL_DATUM);
    amoled.pushColorsCompensated(STATUS_WIDTH, STATUS_HEIGHT, (uint16_t *)spr.getPointer());

    LOGLN("Ready - waiting for frames over USB Serial");
}

void loop()
{
    static unsigned long lastCheck = 0;

    serviceSerialInput();

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

    // No ack-nudge/stall-watchdog here - Serial has no equivalent of AsyncTCP's own
    // send-queue delaying an ack; the ack write above is synchronous to the USB
    // stack, not queued behind anything this firmware doesn't control.
}
