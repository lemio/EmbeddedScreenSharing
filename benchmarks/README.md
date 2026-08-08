# Benchmarks

Real-hardware performance data gathered across this repo's examples, exported as CSV
for charting. Two files:

- **`performance_results.csv`** - one row per test run (one board/protocol/ack-mode
  combination). The main file.
- **`heap_over_time.csv`** - free-heap snapshots over the course of a run, long format
  (one row per snapshot), for the two runs where multiple snapshots exist. Useful for
  a "does memory grow over time" line chart.

## Where the numbers come from

Most rows are computed directly from raw serial-log captures (still available in this
session's scratchpad at the time of writing, not checked into this repo) using a
consistent parser - every per-frame `RAW Timing:`/`Timing:` and `Mutex busy` line in
the window was parsed and averaged, not eyeballed. Two rows (`webJPEG_amoled_191`,
`webJPEG-CYD_320x240`) instead come from numbers already written into
`examples/webJPEG/README.md` and `examples/webRAW-CYD/README.md` in an earlier
session whose raw logs no longer exist - see each row's `source` and `notes` columns.

The two `webRAW-Serial_amoled_t4_*` rows are different again: `webRAW-Serial` isn't a
WebSocket example at all - it's the isolated Web Serial transport prototype (see
`examples/webRAW-Serial/webRAW-Serial.cpp`, not wired into `stream.html` or the
production examples) built to test whether a confirmed, unresolved AsyncTCP/WiFi-
driver heap-corruption crash class is specific to WebSocket. These two rows are that
prototype's actual real-hardware results, driven by a Python script standing in for a
browser (`navigator.serial` from an actual browser tab hasn't been tested yet - see
each row's `notes`). `protocol` is `raw-serial` for both, distinct from the WS-based
`raw` rows above, since it's not just a different ack mode - the whole transport is
different.

`webRAW_amoled_t4_gradient` and `webH264_amoled_t4_gradient` are a matched pair: the
same 80-frame animated diagonal-gradient content, same T4-S3 board, same WebSocket
transport, run back-to-back specifically to compare `raw` vs `h264` head-to-head with
everything else held constant. Both are driven by Python clients standing in for the
browser (raw pixels via `zlib`, H.264 via `ffmpeg`/`libx264` producing an Annex-B
stream parsed into per-frame access units) rather than a real `CompressionStream`/
`VideoEncoder` capture - see each row's `notes` for the exact encode settings used
(matched to `stream.html`'s actual defaults where applicable, e.g. H.264's 700kbps/
5fps/2s-keyframe-interval).
The five `webRAW-CYD_gradient_*`/`webJPEG-CYD_gradient_*` rows are a follow-up to the
AMOLED gradient rows above, run on the CYD (plain ESP32, no PSRAM) instead - same
Python-WS-client methodology, same 80-frame diagonal-gradient content, but each test
run **10 times back-to-back** specifically to confirm the results were stable before
trusting them (they were - see each row's `notes` for the exact run-to-run range).
`frames_rendered`/`frames_dropped`/`frames_total` are summed across all 10 runs
(so up to 800, not 80); `total_min_ms`/`total_max_ms` are the true min/max across
every individual frame in that combined set, not an average of 10 per-run ranges.
These rows exist to document two real bugs found and fixed this way: both CYD
firmwares had the same ack-on-receipt design as the AMOLED's original `webRAW.cpp`
(fixed there, but never ported back to the CYD until these rows' own testing found
it) - `_before_ackfix`/`_before_fix` rows are that bug live (~72-90% drop rates
despite "wait" ack mode), `_after_ackfix` rows are the same fix applied here too
(ack moved from on-receipt to after-render). `webJPEG-CYD_gradient_rowbuffer_only`
is a separate, intermediate row for a different change (batching JPEG MCU-block
pushes into one push per MCU row instead of one per block, aimed at reducing visible
block-by-block tearing) tested in isolation before the ack fix was added on top -
see "Things worth knowing" below for why its drop rate alone doesn't move.

**Don't average across `source` values as if they were equally precise** -
`readme-reported-approximate` in particular is a single quoted range, not a computed
average.

## Column reference (`performance_results.csv`)

| Column | Meaning |
| --- | --- |
| `run_id` | Unique identifier for the run. |
| `board`, `panel` | Physical hardware. |
| `protocol` | `jpeg`, `raw`, or `h264` (all three WebSocket-based) - see the relevant example's README for how each pipeline works. `raw-serial` is a fourth, distinct transport (USB Serial, no WiFi/WebSocket at all) from the isolated `webRAW-Serial` prototype - same decode/render pipeline as `raw`, different wire. |
| `ack_mode` | Flow-control mode active during the run (`immediate`, `wait`, or firmware-specific). |
| `width_px`, `height_px`, `pixels` | Display resolution and its pixel count (`width_px * height_px`). |
| `frames_rendered` | Frames that reached the display. |
| `frames_dropped` | Frames received but not rendered (`Mutex busy - frame dropped` in the serial log - the previous frame was still mid-render). |
| `frames_total`, `drop_rate_pct` | `rendered + dropped`, and dropped's share of that. |
| `sample_duration_s` | Wall-clock span the row's stats were computed over. |
| `achieved_fps` | `(frames_rendered - 1) / sample_duration_s` - throughput of frames that actually reached the screen, not a target rate. |
| `recv_avg_ms` | Average time from the WebSocket message's first byte arriving to the device starting to process it - network transfer + reassembly + queueing, **not** render work. |
| `decode_decompress_avg_ms` | JPEG decode, or RAW deflate decompression. |
| `prep_avg_ms` | The stage between decode/decompress and the final push: JPEG's MCU-block blit into the sprite, or RAW-on-AMOLED's explicit byte-swap. Blank for `webRAW-CYD` rows - that firmware has no separate stage here (`TFT_eSPI`'s `pushImage()` swaps internally as part of the push itself). For the `webJPEG-CYD_gradient_*` rows specifically, holds that firmware's own `Setup` field (per-frame offset calculation, not a blit - there's no sprite on this board, see `push_avg_ms` below). |
| `push_avg_ms` | Time to write finished pixels to the display bus. For the `webJPEG-CYD_gradient_*` rows, holds that firmware's own `Render` field - unlike every other JPEG row, this board has no PSRAM for an intermediate sprite, so decoded MCU blocks go straight to the display one push at a time (or one push per MCU *row* after the `_rowbuffer_only`/`_after_ackfix` optimization - see "Things worth knowing"); `Render` **is** the push work here, just named differently in that firmware's own log line. |
| `compute_total_avg_ms` | `decode_decompress + prep + push` - **on-device compute only, excludes `recv_avg_ms`/network time.** This is the fairest number for comparing raw rendering-pipeline speed across boards/resolutions, independent of WiFi conditions. |
| `total_avg_ms`, `total_min_ms`, `total_max_ms` | The firmware's own logged `Total` (= `recv + compute`, i.e. **includes** network time) - real end-to-end latency from first byte to pixels-on-glass. |
| `gaps_over_500ms_count`, `max_gap_ms` | How many inter-frame gaps exceeded 500ms, and the largest one - a proxy for stalls (as opposed to ordinary per-frame jitter). Not every gap is a bug: see each row's `notes` (e.g. a real client disconnect/reconnect looks identical to a stall in this metric alone). |
| `ns_per_pixel_compute`, `ns_per_pixel_total` | `compute_total_avg_ms` / `total_avg_ms` converted to nanoseconds and divided by `pixels` - resolution-independent cost per pixel, the two normalized side of `compute_total_avg_ms`/`total_avg_ms`. Lower is faster. |
| `megapixels_per_sec_compute` | Inverse framing of `ns_per_pixel_compute` as a throughput number - `pixels / (compute_total_avg_ms / 1000) / 1e6`. Higher is faster. |
| `decompressed_throughput_MBps_compute` | `megapixels_per_sec_compute * 2 bytes/pixel` (RGB565) - how fast the pipeline moves decoded pixel data end to end, in MB/s. |
| `source` | `measured-raw-log` (computed from this session's captures) vs. `readme-reported`/`readme-reported-approximate` (transcribed from an earlier session's README text - see "Where the numbers come from"). |
| `notes` | Caveats specific to that row - read before charting it next to others. |

## Things worth knowing before charting

- **`compute_total_avg_ms` vs `total_avg_ms` measure different things on purpose.**
  `compute_total_avg_ms` isolates the device's own rendering speed (fair
  cross-board/cross-resolution comparison, immune to that day's WiFi conditions).
  `total_avg_ms` is what a viewer actually experienced (network included) - useful for
  comparing ack modes against each other (e.g. `webRAW_amoled_immediate` vs
  `webRAW_amoled_wait`), less useful for comparing across different WiFi networks/rooms.
- **Immediate-mode drop rates are expected, not a defect** - `webRAW_amoled_immediate`
  (69.75%) and any other `immediate` row exist specifically to show what happens with
  no flow control: the browser sends faster than the device can render, and excess
  frames are correctly dropped rather than queued/corrupted. `wait`-mode rows are the
  fairer real-world-usage comparison.
- **The CYD rows predate the AMOLED rows chronologically** and were the reason two
  fixes (a stack-size crash fix and an ack-loss recovery fix) exist at all -
  `webRAW-CYD_capture2_before_fix` vs `webRAW-CYD_capture3_after_fix` is that fix's
  before/after, both on identical hardware/content, and is probably the single most
  illustrative pair of rows to chart together (`gaps_over_500ms_count`: 4 -> 0;
  `achieved_fps`: 5.43 -> 12.74, despite near-identical per-frame `compute_total_avg_ms`).
- **`webJPEG-CYD_320x240` is deliberately sparse** - it's the one row not backed by a
  parsed log, only a remembered range. Treat `total_min_ms`/`total_max_ms` (150/250)
  as a rough bracket, not a distribution to compute a mean from.
- **The two `webRAW-Serial_amoled_t4_*` rows are the same board/protocol under very
  different content, not two different setups** - `flatfill` (~362:1 compression) vs
  `gradient` (~16:1, much closer to real video/photo content) on identical hardware.
  `recv_avg_ms` moving from 5.4ms to 108.75ms is mostly payload size (33KB vs 1.5KB
  compressed) taking longer to arrive even over a fast link; `decode_decompress_avg_ms`
  more than tripling (17.7ms to 55.8ms) is a real decompression-cost effect of harder-
  to-compress data. The `gradient` row's `notes` also documents a real firmware bug
  this specific test surfaced (an RX-buffer-overflow that corrupted every frame after
  the first one) and its fix - worth reading before assuming these numbers were clean
  on the first try.
- **`raw` vs `h264` on identical gradient content is a bandwidth/CPU tradeoff, not a
  clear winner.** `webH264_amoled_t4_gradient`'s average payload (15.5KB/frame) is
  under half `webRAW_amoled_t4_gradient`'s (~33KB/frame), but H.264's software decode
  costs 318.6ms/frame on this chip vs raw deflate's 37.3ms - `h264`'s
  `achieved_fps` (2.51) ends up 2.5x *slower* than `raw`'s (6.23) despite sending
  less than half the bytes. `raw`'s bottleneck is render-pipeline throughput under
  ack-on-receipt (see the `frames_dropped`/63.29% finding above); `h264`'s bottleneck
  is pure CPU decode time, and unlike `raw` it drops **zero** frames at maxInFlight=1
  because its ack only fires after decode+push complete - a structural difference
  worth more than the raw fps number alone.
- **Also worth noting when reading the `raw` vs `h264` gradient rows
  side by side: they're not from the same client tooling** - `raw`'s Python client
  used `zlib` deflate (matching the browser's `CompressionStream('deflate-raw')`
  exactly, byte-for-byte), while `h264`'s used `ffmpeg`/`libx264` reading a pre-generated
  raw frame sequence rather than a live `MediaStreamTrackProcessor` capture. Encoder
  settings were matched to `stream.html`'s actual defaults (700kbps, 5fps, 2s keyframe
  interval, Constrained Baseline/annexb) so the wire format and access-unit chunking
  are realistic, but a real browser encoder's rate-control/motion-search decisions on
  this exact content could still differ somewhat from libx264's.
- **The `webRAW-CYD_gradient_*`/`webJPEG-CYD_gradient_*` before/after pairs are the
  single biggest drop-rate story in this whole file** - both CYD firmwares acked a
  frame the instant it was fully received (`WS_EVT_DATA`), before decode/render even
  started, so "wait mode"'s round-trip was purely network-timed, not render-timed. A
  client that waits for ack could still send the next frame well before the previous
  one finished rendering, hit the render mutex non-blocking, and get silently
  dropped: `raw` fell from 72.3% to **0%** dropped (800/800 rendered across 10 runs),
  `jpeg` from 89.5% to **0%** (also 800/800). Both got measurably *faster* too as a
  side effect (`raw`'s `compute_total_avg_ms` 45.2 to 40.8ms; `jpeg`'s 187.5 to
  148.0ms) - no more CPU spent decoding/reassembling frames that were only ever
  going to be dropped. This is the exact same bug already fixed on the AMOLED's
  `webRAW.cpp` (see `webRAW_amoled_t4_gradient`'s notes) - it just hadn't been
  ported to the CYD firmwares until this testing found it there too.
- **`webJPEG-CYD_gradient_rowbuffer_only`'s drop rate barely moves (89.5% to
  90.1%) despite `push_avg_ms` dropping 12% (185.2 to 162.5ms) - that's expected,
  not a failed fix.** This row isolates a *different* change (batching MCU-block
  pushes into one push per MCU row, aimed at reducing visible tearing, not drop
  rate) tested before the ack-on-receipt bug above was also fixed - rendering
  faster doesn't help drop rate on its own when frames are still arriving faster
  than *any* render speed can keep up with under the old ack timing.
  `webJPEG-CYD_gradient_after_ackfix` (both changes together) is where drop rate
  actually goes to zero.

## Suggested charts

- `ns_per_pixel_compute` by `run_id`, grouped/colored by `protocol` - the headline
  "raw is faster than JPEG, and why" comparison, resolution-independent.
- Stacked bar of `decode_decompress_avg_ms` / `prep_avg_ms` / `push_avg_ms` per row -
  shows *where* the compute time actually goes per pipeline.
- `achieved_fps` and `gaps_over_500ms_count` for the three `webRAW-CYD_*` rows in
  chronological order - the stall-fix before/after story.
- `drop_rate_pct` for `webRAW_amoled_immediate` vs `webRAW_amoled_wait` - the flow-control
  story on the AMOLED board.
- Stacked bar of `recv_avg_ms` / `decode_decompress_avg_ms` / `prep_avg_ms` /
  `push_avg_ms` for `webRAW-Serial_amoled_t4_flatfill` vs `_gradient` - shows content
  compressibility driving both transfer time and decompression cost, on identical
  hardware and transport.
- `achieved_fps` vs average payload size (`decompressed_throughput_MBps_compute` as a
  stand-in) for `webRAW_amoled_t4_gradient` vs `webH264_amoled_t4_gradient` - the
  "smaller payload doesn't mean faster" story, same board/content/transport, only the
  codec differs.
- `heap_over_time.csv`: `free_heap_bytes` vs `elapsed_s`, one line per `run_id` - flat
  lines across all three runs, i.e. no leak, over the sampled windows.
- `drop_rate_pct` for `webRAW-CYD_gradient_before_ackfix` vs `_after_ackfix`, and
  `webJPEG-CYD_gradient_before_fix` vs `_after_ackfix` - the CYD ack-timing fix's
  before/after story, mirroring the AMOLED chart suggestion above.
- `push_avg_ms` (i.e. `Render`) across all three `webJPEG-CYD_gradient_*` rows in
  order - shows the row-buffer optimization's speedup (185.2 to 162.5ms) and the
  ack fix's further speedup (162.5 to 148.0ms minus decode/setup) as two distinct,
  additive effects.
