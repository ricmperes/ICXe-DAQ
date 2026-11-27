# Waveform file format

Cheat sheet for the directory `icxe-daq run` writes. The layout is `encode_waveform_event` in `src/writer.cpp`; `read_waveform_file` in the same file reads `ICX1` and `ICX2` back. The `ICX1` example was checked against `data/v1730sb_20261026T143011Z/wf_00000.bin`.

## Run directory

```text
<output_directory>/
  last_run_number             counter of the run IDs
  run_metadata/
    000042-metadata.json      strax run metadata of run 000042
  000042/
    config.json
    metadata.json
    wf_00000.bin
    wf_00001.bin
```

**Run ID.** Every run gets the next run number of its output directory, printed with at least 6 digits (`000042`), and its folder is `<output_directory>/<run_id>`. The last used number is in `<output_directory>/last_run_number`; it is read and written under a file lock, so two programs started at the same time get different numbers. The next number is above the counter and above every numeric run folder in the directory, so a deleted counter file does not reuse a number. Numbers are never reused, also not for failed runs. Known limit: a new, empty output directory starts again at `000001`, so a run ID is unique only within its output directory. Runs written before 2026-11-27 have folders named `<name>_<UTC>`, e.g. `data/overlap_on_20261127T093731Z/`.

**strax run metadata.** `run_metadata/<run_id>-metadata.json` is the file strax's `DataDirectory(..., provide_run_metadata=True)` reads, so `st.run_metadata` and `st.select_runs` work on that folder without a run database (checked with strax on 2026-11-27). It is written when the run starts (with `end` `null`) and again when it ends:

| Field | Example | Role |
| --- | --- | --- |
| `name`, `number` | `"000042"`, `42` | Run ID and run number. |
| `start`, `end` | `{"$date": 1795779707385}` | Run start rounded down and run end rounded up to the ms, in the form strax reads as a datetime. They cover every chunk. `end` is `null` while the run is going. |
| `start_utc`, `end_utc` | `"2026-11-27T11:41:47.385123456Z"` | Exact run start and end as UTC text, to the ns. |
| `start_unix_time_ns`, `end_unix_time_ns` | `1795779707385123456` | The same instants in Unix ns. |
| `mode`, `tags`, `comments` | `"overlap_on"`, `["bench"]`, `[]` | Config keys `run_mode`, `tags`, `comments` (plain strings). |
| `daq` | object | `run_directory` (folder name next to `run_metadata/`), `is_finished`, `stop_reason` (`null` while running), `boards` (names), `channels_enabled` (global channel numbers), `waveform_format`. |

The DAQ does not touch this file after the run; tags or comments added later (e.g. "bad") are edits on the analysis side.

| File | Contents |
| --- | --- |
| `config.json` | Copy of the JSON loaded for the run. |
| `metadata.json` | Mask, channel list, record length, time-tag tick, run and chunk times, and the file list. Rewritten as events are stored. |
| `wf_NNNNN.bin` | Waveform events. The index is zero-based and printed in five digits, so the first file is `wf_00000.bin`. |

`chunk_duration_s` and `chunk_max_mb` are in both JSON files. A new `wf_*.bin` is started with the first event whose waveform starts `chunk_duration_s` or later after the first waveform of the current file, or whose bytes would make the file larger than `chunk_max_mb` (10^6 bytes). Files therefore end at event boundaries. `wf_*.bin` is created on the first event of that file. Runs written before 2026-11-27 used `waveforms_per_file` (a fixed number of events per file) instead.

## Event bytes

Little-endian. The file is a sequence of events and begins with the first event's magic. Files written now use `ICX2`; older runs use `ICX1`, which has the same layout without the `trigger_time_ticks` field.

| Offset | Bytes | Type | Field |
| --- | --- | --- | --- |
| 0 | 4 | ASCII | magic `ICX2` |
| 4 | 4 | `uint32` | event counter |
| 8 | 4 | `uint32` | trigger time tag, raw |
| 12 | 4 | `uint32` | channel mask |
| 16 | 4 | `uint32` | sample count `Ns` |
| 20 | 8 | `uint64` | `trigger_time_ticks`: unwrapped trigger time tag (see below) |
| 28 | `2 * Nch * Ns` | `int16` | samples |

The header is 28 bytes (`kWaveHeaderBytes`; 20 for `ICX1`, `kWaveHeaderBytesV1`). `Nch` is the number of set bits in the channel mask. One event is `28 + 2 * Nch * Ns` bytes. `waveform_format` in `metadata.json` names the magic used by the run.

Samples are enabled channels only, ascending channel number (bit `1 << ch` for channel `ch`). Each channel is `Ns` little-endian `int16` samples, sample 0 first, then the next enabled channel. The 14-bit ADC code is stored in that 16-bit word: the writer casts the digitizer `uint16` sample to `int16` and writes those bits.

### ICX1 example

`wf_00000.bin` from `data/v1730sb_20261026T143011Z/` (written before `ICX2`) is 61400 bytes. The first header is:

```text
49 43 58 31  00 00 00 00  2f 2b 00 00  07 00 00 00  fc 03 00 00
ICX1          counter 0   tag 11055    mask 0x7     Ns 1020
```

The payload starts `9a 09 9b 09 99 09` (2458, 2459, 2457), channel 0. Channel 1 starts at byte `20 + 1020 * 2` = 2060 and begins 13924, 13927, 13924. Mask `0x7` is channels 0, 1, 2, so `Nch` is 3, one event is 6140 bytes, and the file holds 10 events. The same event in `ICX2` is 6148 bytes, with the samples starting at byte 28.

## Trigger time tag

`trigger_time_tag` is the digitizer `TriggerTimeTag`, copied raw as a `uint32`. It counts 8 ns ticks (`trigger_time_tag_tick_ns`, 125 MHz) from the acquisition start and wraps about every 17 s, so on its own it cannot order events more than one wrap apart.

`trigger_time_ticks` is the same tag unwrapped into a 64-bit count that does not wrap: 8 ns ticks since the run's acquisition start. The unwrapping happens during the run:

* Only the low 31 bits of the raw tag are used, and the wrap period is `trigger_time_tag_wrap_ticks` = `2^31` (17.18 s). This gives the same result whether bit 31 is the top counter bit or a roll-over flag.
* The number of wraps comes from the PC clock. Each event is read at a known PC time, and since the previous event the board ticks must have advanced by about the PC time that passed. Of the possible values, which are 17.18 s apart, the one nearest that expectation is taken. This works for any gap between triggers, including slow triggers with gaps of minutes, as long as readout lag plus clock drift between two consecutive events stays well below half a wrap (8.6 s).

The absolute time of an event is

```text
trigger_unix_ns = acquisition_start_unix_time_ns + trigger_time_ticks * 8
waveform_end    = trigger_unix_ns + post_trigger_samples_actual * 2
waveform_start  = waveform_end - sample_count * 2
```

`sample_count` is the event's own sample count from its header. It equals `record_length` unless the board accepted an overlapping trigger (0x8000 bit[1] = 1), which shortens events. For a full event `waveform_start` is `trigger_unix_ns - pre_trigger_samples_actual * 2`. A shortened event is cut at the front: it starts exactly where the previous event of the board ends, so the two concatenate without gap or duplicate samples (overlap bench test, [overlap_test.md](overlap_test.md)).

Differences of `trigger_time_ticks` are exact board time (8 ns, up to the board oscillator accuracy). The absolute offset is only as good as the PC clock, about a millisecond (see Run timestamps).

`event_counter` is the digitizer header counter. `event_counter_note` in metadata says baseline software triggers can advance it before the run. The first counter in the example file is 0.

## Metadata fields that decode the file

The event header already carries the mask and the sample count. These metadata fields match that layout:

| Field | Example | Role |
| --- | --- | --- |
| `channel_mask` | `"0x7"` | Hex string of the mask written into each event. |
| `enabled_channels` | `[0, 1, 2]` | Set bits of that mask, ascending. This is the channel axis. |
| `record_length` | `1020` | Samples per channel read back from the board. Equals the header sample count of every full-length event. |
| `trigger_time_tag_tick_ns` | `8` | Nanoseconds per trigger time tag tick. |
| `trigger_time_tag_wrap_ticks` | `2147483648` | Wrap period (`2^31` ticks) used to unwrap the tag. |
| `waveform_format` | `"ICX2"` | Event magic and header layout of this run. |
| `sample_period_ns` | `2` | Sample period, from `kSamplePeriodNs` (500 MS/s). |
| `chunk_duration_s` | `5` | Time span of one `wf_*.bin` (config key, see above). |
| `chunk_max_mb` | `500` | Size limit of one `wf_*.bin` in MB (config key, see above). |
| `run_mode` | `"overlap_on"` | Config key `run_mode`. |
| `tags` | `["bench"]` | Config key `tags`. |
| `comments` | `[]` | Config key `comments`. |
| `channel_map` | `{"tpc": [0, 99], ...}` | Config key `channel_map`: detector name → inclusive range of global channel numbers. |
| `channels` | list | Every channel of the board, enabled or not: `index` (board input), `global_channel`, `detector` (from `channel_map`), `enabled`, `polarity`, `dynamic_range`, `target_percent`, `threshold_counts`, `dac`, and the measured `baseline_mean`, `baseline_rms`, `baseline_within_tolerance` (`null` when not measured). |
| `files` | `["wf_00000.bin"]` | Names written in this run. |
| `chunks` | see below | One entry per file, with its time span. |
| `events_written` | `10` | Events across those files. |
| `events_read_after_stop` | `3` | Events read from the board memory after the acquisition stopped, included in `events_written`. `null` when the memory was not read (`max_events`, `error`, or while the run is in progress). |
| `sample_counts` | see below | Smallest and largest header sample count of the run, and how many events were shorter or longer than `record_length`. |

```json
"sample_counts": {
  "minimum": 2500,
  "maximum": 2500,
  "events_shorter_than_record": 0,
  "events_longer_than_record": 0
}
```

`minimum` and `maximum` are `null` while no event has been written. With overlap acceptance off, all events have `record_length` samples.

`channel_mask` in JSON is a string from `hex32` (`"0x7"`). The header stores the same mask as a binary `uint32`. `record_length_requested` is the config request; the length on disk is `record_length` and the header sample count. `pre_trigger_samples_actual` and `post_trigger_samples_actual` (204 and 816 in the example) locate the trigger inside the full record. Per-channel DAC and baseline fields describe the setup and are separate from the sample packing.

## Run timestamps

| Field | Example | Role |
| --- | --- | --- |
| `run_id`, `run_number` | `"000042"`, `42` | Run ID (folder name) and run number. |
| `started_utc` | `"2026-11-02T13:21:08.123456789Z"` | Run start, UTC, to the ns. |
| `started_utc_unix_time_ns` | `1793625668123456789` | Same instant as `started_utc`, Unix time in nanoseconds (integer). |
| `acquisition_start_utc` | `"2026-11-02T13:21:08.131000000Z"` | `acquisition_start_unix_time_ns` as UTC text. |
| `acquisition_start_unix_time_ns` | `1793625668131000000` | PC time just before `CAEN_DGTZ_SWStartAcquisition`. Origin of `trigger_time_ticks`. `null` until acquisition starts. |
| `ended_utc` | `"2026-11-02T13:25:41.987654321Z"` | Run end, UTC, to the ns. `null` while the run is in progress. |
| `ended_utc_unix_time_ns` | `1793625941987654321` | Same instant as `ended_utc`, Unix time in nanoseconds. `null` while the run is in progress. |

Each string and its `_unix_time_ns` partner describe the same instant, to the ns (ISO 8601, readable by `numpy.datetime64` and pandas). Runs written before 2026-11-27 have strings in whole seconds. The start is taken when the run directory is created, a few milliseconds before acquisition starts. The end is taken after acquisition stops and the last waveform file is flushed. If the last chunk ends later than that (board clock running fast against the PC clock), the run end is moved to the end of the last chunk, so `ended_utc_unix_time_ns` is never earlier than any chunk.

The clock is the PC wall clock, so its accuracy is that of the host time sync (NTP, typically about a millisecond), not nanoseconds. The board resets its trigger time tag when acquisition actually starts, which is shortly after `acquisition_start_unix_time_ns`, so absolute event times can be early by that delay (milliseconds at most).

## Stop reason and board health

| Field | Example | Role |
| --- | --- | --- |
| `stop_reason` | `"max_events"` | Why the run ended: `duration`, `max_events`, `sigint`, `sigterm`, `board_status` (a status check found a problem, see `board_health.problems`; the program then exits with an error), or `error` (an exception; files are closed as far as possible). |
| `stop_conditions.duration_s` | `10.0` | Time limit of the run in seconds, counted from the start of acquisition. `null` when there was none. |
| `stop_conditions.duration_s_source` | `"command line --duration"` | Where the time limit came from: `"command line --duration"` (it replaced `duration_s` of the config) or `"config duration_s"`. |
| `stop_conditions.max_events` | `null` | `--max-events N` of the run, `null` when not given. |
| `board_health.start` | object | Board state just before the acquisition starts: `acquisition_status_0x8104` (raw value and decoded flags: running, event ready, memory full, external clock, PLL lock kept, board ready, channels shut down, overheated channels), `board_failure_status_0x8178` (raw value and PLL lock loss, temperature failure, ADC power down), and `adc_temperatures_celsius` (16 values, ADC chip temperature of each channel, register 0x1nA8). |
| `board_health.end` | object or `null` | Same, read after the acquisition stopped. `null` while the run is in progress, or when the board could not be read at the end. |
| `board_health.status_checks_during_run` | `61` | How many times 0x8104 and 0x8178 were checked during the run. The checks are due at 1 s, 2 s, ... after the start of acquisition, so a 10 s run has 9 or 10. |
| `board_health.problems` | `[]` | Problems found by the last check (0x8104 and 0x8178), one sentence each. Empty for a run that ended normally. |

The board protects itself above 85 °C (V1730S): it turns off the channels of the hot 4-channel group and stops its acquisition (UM5118, 0x8104 bits 23:20 and 19). The status checks stop the run with `stop_reason` `board_status` when that happens, when the clock source differs from `boards[].clock_source`, or when the PLL loses lock.

## Chunks

Each `wf_NNNNN.bin` is one chunk. `chunks` lists them in order:

```json
"chunks": [
  {
    "index": 0,
    "file": "wf_00000.bin",
    "n_events": 1000,
    "first_event_counter": 0,
    "last_event_counter": 999,
    "start_utc": "2026-11-02T13:21:08.131000000Z",
    "start_unix_time_ns": 1793625668131000000,
    "end_utc": "2026-11-02T13:21:10.512345678Z",
    "end_unix_time_ns": 1793625670512345678,
    "first_waveform_start_unix_time_ns": 1793625668131200000,
    "last_waveform_end_unix_time_ns": 1793625670511000000
  }
]
```

Waveform times use the formula in "Trigger time tag". The chunk times obey:

* `started_utc_unix_time_ns <= chunks[0].start_unix_time_ns` and `chunks[-1].end_unix_time_ns <= ended_utc_unix_time_ns`.
* No gaps: `chunks[i].end_unix_time_ns == chunks[i+1].start_unix_time_ns`.
* `start_unix_time_ns <= first_waveform_start_unix_time_ns` and `last_waveform_end_unix_time_ns <= end_unix_time_ns`.

How the edges are chosen:

* The first chunk starts at `acquisition_start_unix_time_ns`, or at its first waveform start if that is earlier (a trigger within the pre-trigger window of the start). It never starts before the run start.
* The boundary between two chunks is the start of the later chunk's first waveform.
* The last chunk ends at the PC time when acquisition stopped, or at its last waveform end if that is later. This needs every event before the stop: after `SWStopAcquisition` the program reads the events still in the board memory and writes them (`events_read_after_stop`). This happens for the stop reasons `duration`, `sigint`, `sigterm` and `board_status`.
* When later events were not written, the last chunk ends at its last waveform end instead, so it never claims time without data. This applies to `max_events` (events after the N-th are dropped on purpose; the board memory is not read) and `error`.

Exceptions to the containment rule:

* **Overlapping waveforms.** If the last waveform of one chunk and the first waveform of the next overlap in time, which can happen when a trigger arrives within one pre-trigger window of the previous record's end, the boundary is the end of the earlier waveform. The later chunk's `first_waveform_start_unix_time_ns` is then slightly before its `start_unix_time_ns`.
* **First waveform before the run start.** The run-start rule wins if a first waveform would start before the run start, which needs a pre-trigger window longer than the few ms between run start and acquisition start.

The open chunk has `end_unix_time_ns` `null` while the run is in progress. A run with no events has an empty `chunks` list.

## Read a file

`tools/read_waveforms.py` has moved to the analysis package. The description below is for `ICX1` files; it needs the 8-byte `trigger_time_ticks` field added to read `ICX2` (see Numpy).

```bash
python3 tools/read_waveforms.py data/v1730sb_20261026T143011Z/wf_00000.bin \
    --metadata data/v1730sb_20261026T143011Z/metadata.json
```

On that file the tool prints:

```text
events=10 channels=3 samples=1020 mask=0x7
channel_axis=[0, 1, 2]
channel_axis_note=enabled channels in ascending order
metadata_enabled_channels=[0, 1, 2]
metadata_channel_mask=0x7
```

`load_waveforms(path)` memmaps the file. It returns `waveforms`, an `int16` array of shape `(n_events, n_enabled_channels, n_samples)`, and header columns `event_counter`, `trigger_time_tag`, `channel_mask`, and `sample_count`. The channel axis is the enabled channels in ascending order. The example shape is `(10, 3, 1020)`.

`--metadata` prints `enabled_channels` and `channel_mask`. The array is built from the file. `--head N` also prints N samples from event 0, channel axis 0.

`load_waveforms` takes `Nch` and `Ns` from the first event and requires that mask and sample count on every event, with the file length an integer number of those events. `read_waveform_file` in `src/writer.cpp` uses each event's own mask and sample count. The acquisition loop writes one `channel_mask` for the whole run. The sample count is the record length for every event unless overlapping triggers are accepted; then events in one file can have different lengths, and only `read_waveform_file` (or a reader that walks the headers) handles them, not the fixed-size `load_waveforms` or the numpy dtype below.

`load_waveforms` counts set bits in mask bits 0..15. `encode_waveform_event` sizes the payload with `popcount_u32` of the full 32-bit mask. Channel indexes here are 0..15, and mask `0x7` is 3 channels either way.

## Numpy

`ICX2` dtype (`align=False`). For an `ICX1` file, drop the `trigger_time_ticks` field and read a 20-byte prefix instead of 28:

```python
import json
import numpy as np

run = "data/<run_mode>_<UTC>"
path = f"{run}/wf_00000.bin"
with open(path, "rb") as handle:
    prefix = handle.read(28)
assert prefix[:4] == b"ICX2"
mask = int(np.frombuffer(prefix[12:16], dtype="<u4")[0])
n_samples = int(np.frombuffer(prefix[16:20], dtype="<u4")[0])
n_channels = sum(1 for i in range(16) if mask & (1 << i))
dtype = np.dtype(
    [
        ("magic", "S4"),
        ("event_counter", "<u4"),
        ("trigger_time_tag", "<u4"),
        ("channel_mask", "<u4"),
        ("sample_count", "<u4"),
        ("trigger_time_ticks", "<u8"),
        ("samples", "<i2", (n_channels, n_samples)),
    ],
    align=False,
)
events = np.memmap(path, dtype=dtype, mode="r")
waveforms = events["samples"]
# waveforms.shape == (n_events, n_enabled_channels, n_samples), dtype int16

meta = json.load(open(f"{run}/metadata.json"))
trigger_unix_ns = meta["acquisition_start_unix_time_ns"] + events["trigger_time_ticks"].astype(np.int64) * 8
```
