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
**Don't average across `source` values as if they were equally precise** -
`readme-reported-approximate` in particular is a single quoted range, not a computed
average.

## Column reference (`performance_results.csv`)

| Column | Meaning |
| --- | --- |
| `run_id` | Unique identifier for the run. |
| `board`, `panel` | Physical hardware. |
| `protocol` | `jpeg` or `raw` - see the relevant example's README for how each pipeline works. |
| `ack_mode` | Flow-control mode active during the run (`immediate`, `wait`, or firmware-specific). |
| `width_px`, `height_px`, `pixels` | Display resolution and its pixel count (`width_px * height_px`). |
| `frames_rendered` | Frames that reached the display. |
| `frames_dropped` | Frames received but not rendered (`Mutex busy - frame dropped` in the serial log - the previous frame was still mid-render). |
| `frames_total`, `drop_rate_pct` | `rendered + dropped`, and dropped's share of that. |
| `sample_duration_s` | Wall-clock span the row's stats were computed over. |
| `achieved_fps` | `(frames_rendered - 1) / sample_duration_s` - throughput of frames that actually reached the screen, not a target rate. |
| `recv_avg_ms` | Average time from the WebSocket message's first byte arriving to the device starting to process it - network transfer + reassembly + queueing, **not** render work. |
| `decode_decompress_avg_ms` | JPEG decode, or RAW deflate decompression. |
| `prep_avg_ms` | The stage between decode/decompress and the final push: JPEG's MCU-block blit into the sprite, or RAW-on-AMOLED's explicit byte-swap. Blank for `webRAW-CYD` rows - that firmware has no separate stage here (`TFT_eSPI`'s `pushImage()` swaps internally as part of the push itself). |
| `push_avg_ms` | Time to write finished pixels to the display bus. |
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

## Suggested charts

- `ns_per_pixel_compute` by `run_id`, grouped/colored by `protocol` - the headline
  "raw is faster than JPEG, and why" comparison, resolution-independent.
- Stacked bar of `decode_decompress_avg_ms` / `prep_avg_ms` / `push_avg_ms` per row -
  shows *where* the compute time actually goes per pipeline.
- `achieved_fps` and `gaps_over_500ms_count` for the three `webRAW-CYD_*` rows in
  chronological order - the stall-fix before/after story.
- `drop_rate_pct` for `webRAW_amoled_immediate` vs `webRAW_amoled_wait` - the flow-control
  story on the AMOLED board.
- `heap_over_time.csv`: `free_heap_bytes` vs `elapsed_s`, one line per `run_id` - flat
  lines across all three runs, i.e. no leak, over the sampled windows.
