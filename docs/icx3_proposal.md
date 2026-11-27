# Proposal: ICX3 data format, channel map and run metadata (step 2c.4)

Status: **draft for review**, nothing implemented. Once agreed, the final text moves into `docs/data_format.md` and `docs/config.md`, and this file is deleted.

Goals:

1. A run of several boards is written as one time-ordered stream that an abstrax `DAQReader` turns into `raw_records` without extra bookkeeping.
2. Every chunk follows the strax chunk rules, so chunks can be processed independently (in parallel, and online while the run is still going).
3. Channel numbers are set by a configurable channel map, as in straxen.
4. The run can be found with `st.select_runs` (run mode, tags, comments) without a run database.

## 1. What strax and abstrax require

From strax (`DataDirectory`, `run_selection.py`, `raw_record_dtype`) and abstrax tutorials 1 and 5:

| Requirement | Consequence for the DAQ |
| --- | --- |
| Data is a stream of intervals, sorted by `time` (int64, ns since the Unix epoch). | Every waveform carries the Unix time of its **first sample** in ns. Waveforms in a file are sorted by time (ties: by channel). |
| A chunk covers `[start, end)`; chunks of a run have no gaps and no overlaps; every interval of a chunk lies inside it. | Same rules as today's `ChunkTracker`, now over all boards: a chunk boundary must lie between waveforms of **all** channels. |
| `raw_records` hold one channel per row (`channel`, `time`, `dt`, `length`, `pulse_length`, `record_i`, `baseline`, `data`). | A waveform in ICX3 is **one channel**, not one event of 16 channels. The reader only cuts it into fixed-length records. |
| `channel` is one integer per sensor; plugins read index ranges from `detector.yaml` (`top_pmt_indices` ...). | Global channel numbers come from a channel map in the config (section 4). |
| Run metadata file `<run_id>-metadata.json`, run ID = text before the first `-`, and a numeric run name (strax makes `number = int(name)`). | Runs get a **numeric run ID** (6 digits). |
| `select_runs` uses `mode`, `tags` (list of strings, or of `{"name": ...}`), `comments`, `start`, `end` (datetimes, written as `{"$date": <ms>}` by `bson.json_util`). | The DAQ writes a strax run-metadata file (section 6). |
| `start`, `end` of the run must cover all data. | `start` ≤ first chunk start, `end` ≥ last chunk end, rounded outwards to whole ms. |
| An online reader treats `wf_NNNNN.bin` as ready when it exists (tutorial 5, `is_ready`). | A chunk file is written under a temporary name and renamed when it is complete. |

## 2. Run identity and directory layout

```text
<output_directory>/
  run_metadata/
    000042-metadata.json      strax run metadata (section 6), one file per run
  000042/
    config.json               copy of the loaded config
    metadata.json             DAQ metadata (section 5)
    wf_00000.bin              chunk 0
    wf_00001.bin              chunk 1
    wf_00002.bin.tmp          chunk being written (renamed to .bin when complete)
```

* **Run ID:** the next free 6-digit number in `<output_directory>`. The DAQ keeps the last used number in `<output_directory>/last_run_number` and takes a file lock while incrementing it, so two runs started at the same time cannot get the same number. Run IDs are never reused, also not for failed runs. If the counter file is missing but run directories exist, the counter continues after the highest existing run ID instead of starting at 1. Known limit (accepted 2026-11-27): a new, empty output directory starts again at `000001`, so run IDs are unique only within one output directory. A run database would solve this but is too much for now.
* The run directory name is just the run ID. The descriptive part of today's name (`overlap_on_20261127T...`) moves into the run mode and the start time in the metadata.
* `run_metadata/` contains nothing but run-metadata files, so it can be added to a strax context as `strax.DataDirectory(<output_directory>/run_metadata, provide_run_metadata=True, readonly=True)`.

## 3. ICX3 waveform file

One file = one chunk. A file is a sequence of waveforms. Little-endian, no file header. A waveform is one channel:

| Offset | Bytes | Type | Field |
| --- | --- | --- | --- |
| 0 | 4 | ASCII | magic `ICX3` |
| 4 | 2 | `uint16` | header size in bytes (40), so later versions can append fields |
| 6 | 2 | `uint16` | board index (position in the config's `boards` list) |
| 8 | 2 | `int16` | global channel number (channel map, section 4) |
| 10 | 2 | `uint16` | channel on the board (0–15) |
| 12 | 4 | `uint32` | event counter of the board's first event in this waveform |
| 16 | 8 | `int64` | `time`: Unix time of the first sample, ns |
| 24 | 8 | `uint64` | `trigger_time_ticks` of that first event (8 ns ticks since the acquisition start, unwrapped) |
| 32 | 4 | `uint32` | sample count `Ns` |
| 36 | 4 | `uint32` | `n_events`: number of board events in this waveform (1, or more when overlapping events were joined) |
| 40 | `2 * Ns` | `int16` | samples, sample 0 first, raw 14-bit ADC code as today |

Overlapping triggers (0x8000 bit 1): the board writes one full event and then, for each overlapping trigger, a shortened event that starts where the previous one ended (2c.1). The DAQ joins these events channel by channel into **one** waveform with **one** header, so in the file there is no shortened event and no second header. `event_counter` is that of the first event, `n_events` says how many were joined. It is not needed for processing; it keeps every trigger countable: the next waveform of that board starts with event counter `event_counter + n_events`, so a lost event shows up as a jump (check in 2c.4, dead time in step 3).

Rules:

* Waveforms are sorted by `time`, then by global channel number.
* Waveform end = `time + 2 ns * Ns`. No waveform crosses a chunk boundary.
* The event counter and ticks are kept for checks (equal counters on all boards, dead time in step 3); the processing needs only `time`, channel and samples.
* Reading in Python: the header has a fixed size and holds `Ns`, so a reader walks the file waveform by waveform (or once builds an index of offsets with numba, then uses numpy views). The abstrax reader maps `time`, channel, `Ns` and the samples to `raw_records` (`dt` = 2, `pulse_length` = `Ns`, cut into records of `samples_per_record`).

Why one waveform per channel instead of one event per board: strax works per channel anyway; switched-off channels then simply have no waveforms (section 4.3); concatenation and later self-triggered channels (different lengths and times per channel) fit without changing the format.

Size: 40 bytes of header per channel and waveform. For a 5 µs window (2500 samples, 5000 bytes) that is 0.8 %.

## 4. Channel map

### 4.1 Config

Two parts, both required keys (rule in CLAUDE.md):

```json
"channel_map": {
  "tpc":      [0, 99],
  "external": [100, 199],
  "aqmon":    [200, 299]
},
"boards": [
  {"name": "B1", ...,
   "channels": [
     {"index": 0, "global_channel": 0, "enabled": true, ...},
     {"index": 1, "global_channel": 1, "enabled": true, ...},
     ...
   ]}
]
```

* `channel_map` (top level): detector name → inclusive range `[first, last]` of global channel numbers, like `straxen`'s channel map (`"tpc": (0, 493)`, `"aqmon": (790, 807)`, ...). Ranges must not overlap. The names are free; `tpc`, `external` and `aqmon` are the ones we need now.
* `global_channel` (every channel of every board, also disabled ones): the channel number in the data. It says where that input is cabled, independent of whether it is recorded in this run. Checks when the config is loaded:
  * unique across all boards,
  * inside one range of `channel_map` (that range gives the channel's detector),
  * integer `0..32767` (strax stores `channel` as `int16`).
* Why one number per channel and not one offset per board: the cabling of the 64 SiPMs and 4 PMTs to the board inputs does not have to follow the sensor numbering, and a board can carry channels of several detectors (e.g. B4 with PMTs and the external detector). An explicit number per input handles both.

Example for our setup (to be filled with the real cabling): SiPMs 0–63 and PMTs 64–67 in `tpc`, external detector 100–101 in `external`, B5 channels in `aqmon` 200–215. `detector.yaml` of abstrax then uses `top_pmt_indices: [0, 64]` and `bottom_pmt_indices: [64, 68]`.

### 4.2 In the metadata

`metadata.json` lists **every** channel of every board (section 5), with board, board channel, global channel, detector, `enabled` and the per-channel settings and baseline results. abstrax can build its own maps from this list (for example which TPC channels were recorded).

### 4.3 Switched-off channels

* A channel with `"enabled": false` is not read out by the board (its bit in the channel mask is 0). It has **no waveforms** in the files: no empty or zero-filled entries, and it costs no disk space. This answers the earlier question: recording one channel does not write 63 empty waveforms.
* It still appears in the metadata channel list with `"enabled": false` and its global channel number, so the processing can tell "channel not recorded" apart from "channel recorded but no signal".
* Broken SiPMs are switched off this way; they keep their global number, so the numbering of the others does not change.
* Later, with self-triggered channels, an enabled channel can also have no waveform in a time range (no signal above threshold). Only the metadata decides between "off" and "quiet".

## 5. DAQ metadata (`<run>/metadata.json`)

Same role as today, extended for several boards. Main fields (types as in today's `docs/data_format.md`):

| Field | Content |
| --- | --- |
| `run_id`, `run_mode` | `"000042"`, mode from the config (section 6) |
| `waveform_format` | `"ICX3"`, `waveform_header_bytes` 40, `sample_period_ns` 2 |
| `started_utc`, `started_utc_unix_time_ns`, `ended_utc`, `ended_utc_unix_time_ns`, `acquisition_start_utc`, `acquisition_start_unix_time_ns` | as today, plus the string for the acquisition start |
| `stop_reason`, `stop_conditions`, `events_read_after_stop` | as today, per run |
| `boards` | one entry per board: name, serial, firmware, clock source, record length, pre/post-trigger, registers written, `board_health` (start, end, checks, problems), events read, events per board |
| `channel_map` | copy of the config's map |
| `channels` | every channel of every board: `board`, `board_channel`, `global_channel`, `detector`, `enabled`, polarity, dynamic range, DC offset, threshold, baseline mean and RMS |
| `chunks` | per chunk: `chunk_i`, `file`, `start_utc`, `start_unix_time_ns`, `end_utc`, `end_unix_time_ns`, `n_waveforms`, `first_waveform_start_unix_time_ns`, `last_waveform_end_unix_time_ns`, and per board the first and last event counter |
| `is_finished` | `false` while the run is going, `true` after the last chunk has been renamed and the metadata written; an online reader uses it for `source_finished` |
| `summary` | waveforms, concatenated waveforms, events per board, event counter differences between boards (2c.4 check) |

## 6. strax run metadata (`run_metadata/<run>-metadata.json`)

Written at the start of the run (with `end` = `null`) and rewritten at the end. Fields:

```json
{
  "name": "000042",
  "number": 42,
  "start": {"$date": 1795779707385},
  "end": {"$date": 1795779717409},
  "start_utc": "2026-11-27T11:41:47.385123456Z",
  "end_utc": "2026-11-27T11:41:57.408462786Z",
  "start_unix_time_ns": 1795779707385123456,
  "end_unix_time_ns": 1795779717408462786,
  "mode": "tpc_external_trigger",
  "tags": ["bench", "overlap_test"],
  "comments": ["AFG bursts of 2 pulses, 2 us apart"],
  "daq": {
    "run_directory": "000042",
    "stop_reason": "duration",
    "n_boards": 4,
    "channels_enabled": [0, 1, 2],
    "waveform_format": "ICX3"
  }
}
```

* `mode`: new config key `run_mode` (string). It replaces today's `name`, which only labels the run directory. The config file *is* the run mode, as `reader.ini.name` in straxen, so it belongs in the config.
* `tags`: new config key `tags` (list of strings, may be `[]`), copied as is. A tag must not contain a comma: `select_runs` joins the tags of a run with commas before matching.
* `comments`: new config key `comments` (list of strings, may be `[]`), copied as is.
* Run mode, tags and comments all come from the config file (review 2026-11-27), so the `config.json` copy in the run directory documents them too. Tags and comments added after the run (e.g. "bad", or a shift note) are edits of the run-metadata file on the analysis side; the DAQ does not touch it after the run.
* `start` / `end`: run start and end in ms, rounded down / up, so they cover all chunks. strax needs them in the `{"$date": <ms>}` form; the exact run start and end are also written as readable UTC strings (`start_utc`, `end_utc`, to the ns) and in Unix ns (`start_unix_time_ns`, `end_unix_time_ns`).

Rule for both metadata files (review 2026-11-27): every start or end time is written twice, as a UTC string (ISO 8601 with `Z`; to the ns where the value has ns precision, e.g. `"2026-11-27T11:41:47.385123456Z"`) and as an integer `*_unix_time_ns`. Today's `started_utc` (whole seconds) becomes a full-precision string as well.
* `daq`: a small summary for `select_runs` and for finding the run directory. The full detail stays in `<run>/metadata.json`.

Then in abstrax:

```python
st.storage.append(strax.DataDirectory("<output_directory>/run_metadata",
                                      provide_run_metadata=True, readonly=True))
st.select_runs(run_mode="tpc_*", include_tags="bench")
```

## 7. What changes in the config

| Key | Change |
| --- | --- |
| `name` | replaced by `run_mode` |
| `tags` | new, top level, list of strings |
| `comments` | new, top level, list of strings |
| `channel_map` | new, top level |
| `boards[].channels[].global_channel` | new, every channel |
| `waveforms_per_file` | replaced by a chunk size rule, see question 3 |


## 8. Open questions for the review

1. ~~Run ID~~ Agreed (2026-11-27): 6-digit counter, counter file in the output directory next to the data (section 2).
2. ~~Two metadata files~~ Agreed (2026-11-27): two files. `st.run_metadata(run_id)` returns the strax file (mode, tags, comments, start, end, `daq` summary with the run directory). Reading the full DAQ metadata from abstrax needs a small helper there (e.g. find `<output_directory>/<daq.run_directory>/metadata.json`); to be decided later together with the abstrax reader.
3. **Chunk size** — agreed (2026-11-27): by time. Details: strax rechunks to ~200 MB when saving, so the DAQ chunk size is mainly about online processing and file handling. Proposal: new key `chunk_duration_s` (e.g. 5 s): a chunk is closed at the first gap between waveforms after that time; a chunk with more than `chunk_max_mb` (e.g. 500) is closed early at the next gap. This replaces `waveforms_per_file`, which no longer fits when one event writes a different number of waveforms depending on the enabled channels. Fine, or keep a count?
4. ~~Concatenation~~ Not a real question (withdrawn 2026-11-27): overlapping events of a board are joined channel by channel, as CLAUDE.md requires.
5. ~~Baseline in `raw_records`~~ Agreed (2026-11-27): the baseline is measured once per run and kept only in the metadata (`channels[].baseline_mean`, `baseline_rms`); ICX3 has no baseline field.
6. ~~Names of the detector ranges~~ Agreed (2026-11-27): `tpc` 0–99, `external` 100–199, `aqmon` 200–299 (inclusive ranges as in straxen).
7. ~~Tags as plain strings~~ Decided (2026-11-27): tags and comments are plain lists of strings, in the config and in the run-metadata file. `select_runs` reads plain strings; strax superruns (`st.define_run`) need the dictionary form and are given up.
