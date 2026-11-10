# Waveform file format

Cheat sheet for the directory `icxe-daq run` writes. The layout is `encode_waveform_event` in `src/writer.cpp`, which `tools/read_waveforms.py` reads back. Checked against `data/v1730sb_20261026T143011Z/wf_00000.bin`.

## Run directory

```text
<data>/<name>_<YYYYMMDDTHHMMSSZ>/
  config.json
  metadata.json
  wf_00000.bin
  wf_00001.bin
```

The folder is `<output_directory>/<name>_<UTC>`, UTC as `%Y%m%dT%H%M%SZ`. If that path exists, the name gains `-2`, `-3`, and so on. The example is `data/v1730sb_20261026T143011Z/`.

| File | Contents |
| --- | --- |
| `config.json` | Copy of the JSON loaded for the run. |
| `metadata.json` | Mask, channel list, record length, time-tag tick, and the file list. Rewritten as events are stored. |
| `wf_NNNNN.bin` | Waveform events. The index is zero-based and printed in five digits, so the first file is `wf_00000.bin`. |

`waveforms_per_file` is in both JSON files. One `wf_*.bin` holds at most that many events; the next event opens the next index. The last file holds the remainder. `wf_*.bin` is created on the first event of that file. The example has `waveforms_per_file` 100, `events_written` 10, and one file, `wf_00000.bin`.

## Event bytes

Little-endian. The file is a sequence of events and begins with the first event's magic.

| Offset | Bytes | Type | Field |
| --- | --- | --- | --- |
| 0 | 4 | ASCII | magic `ICX1` |
| 4 | 4 | `uint32` | event counter |
| 8 | 4 | `uint32` | trigger time tag, raw |
| 12 | 4 | `uint32` | channel mask |
| 16 | 4 | `uint32` | sample count `Ns` |
| 20 | `2 * Nch * Ns` | `int16` | samples |

The header is 20 bytes (`kWaveHeaderBytes`). `Nch` is the number of set bits in the channel mask. One event is `20 + 2 * Nch * Ns` bytes.

Samples are enabled channels only, ascending channel number (bit `1 << ch` for channel `ch`). Each channel is `Ns` little-endian `int16` samples, sample 0 first, then the next enabled channel. The 14-bit ADC code is stored in that 16-bit word: the writer casts the digitizer `uint16` sample to `int16` and writes those bits.

`wf_00000.bin` in the example is 61400 bytes. The first header is:

```text
49 43 58 31  00 00 00 00  2f 2b 00 00  07 00 00 00  fc 03 00 00
ICX1          counter 0   tag 11055    mask 0x7     Ns 1020
```

The payload starts `9a 09 9b 09 99 09` (2458, 2459, 2457), channel 0. Channel 1 starts at byte `20 + 1020 * 2` = 2060 and begins 13924, 13927, 13924. Mask `0x7` is channels 0, 1, 2, so `Nch` is 3, one event is 6140 bytes, and the file holds 10 events.

## Trigger time tag

The header field is the digitizer `TriggerTimeTag`, copied with `event.trigger_time_tag = info.TriggerTimeTag`. It is a raw `uint32`. The tick is metadata, not a header field: `trigger_time_tag_tick_ns` is 8, written from `kTriggerTimeTagTickNs` in `src/util.hpp` (125 MHz, the waveform-firmware clock). Nanoseconds are the raw tag times that value. The count is 32 bits, so a difference can wrap at `2^32`.

`event_counter` is the digitizer header counter. `event_counter_note` in metadata says baseline software triggers can advance it before the run. The first counter in the example file is 0.

## Metadata fields that decode the file

The event header already carries the mask and the sample count. These metadata fields match that layout:

| Field | Example | Role |
| --- | --- | --- |
| `channel_mask` | `"0x7"` | Hex string of the mask written into each event. |
| `enabled_channels` | `[0, 1, 2]` | Set bits of that mask, ascending. This is the channel axis. |
| `record_length` | `1020` | Samples per channel read back from the board. Same value as the header sample count. |
| `trigger_time_tag_tick_ns` | `8` | Nanoseconds per raw trigger time tag. |
| `sample_period_ns` | `2` | Sample period, from `kSamplePeriodNs` (500 MS/s). |
| `waveforms_per_file` | `100` | Maximum events in one `wf_*.bin`. |
| `files` | `["wf_00000.bin"]` | Names written in this run. |
| `events_written` | `10` | Events across those files. |

`channel_mask` in JSON is a string from `hex32` (`"0x7"`). The header stores the same mask as a binary `uint32`. `record_length_requested` is the config request; the length on disk is `record_length` and the header sample count. `pre_trigger_samples_actual` and `post_trigger_samples_actual` (204 and 816 in the example) locate the trigger inside the full record. Per-channel DAC and baseline fields describe the setup and are separate from the sample packing.

## Run timestamps

| Field | Example | Role |
| --- | --- | --- |
| `started_utc` | `"2026-11-02T13:21:08Z"` | Run start, UTC, whole seconds. |
| `started_utc_unix_time_ns` | `1793625668123456789` | Same instant as `started_utc`, Unix time in nanoseconds (integer). |
| `ended_utc` | `"2026-11-02T13:25:41Z"` | Run end, UTC, whole seconds. `null` while the run is in progress. |
| `ended_utc_unix_time_ns` | `1793625941987654321` | Same instant as `ended_utc`, Unix time in nanoseconds. `null` while the run is in progress. |

Each string and its `_unix_time_ns` partner come from one `std::chrono::system_clock` reading, so the string is that value truncated to the second. The start is taken when the run directory is created, a few milliseconds before `CAEN_DGTZ_SWStartAcquisition`. The end is taken after acquisition stops and the last waveform file is flushed.

The clock is the PC wall clock, so its accuracy is that of the host time sync (NTP, typically about a millisecond), not nanoseconds. The trigger time tag counts from the board's own acquisition start, which is offset from `started_utc_unix_time_ns` by an unknown few milliseconds. `started_utc_unix_time_ns + trigger_time_tag * 8` is therefore an approximate absolute event time.

## Read a file

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

`load_waveforms` takes `Nch` and `Ns` from the first event and requires that mask and sample count on every event, with the file length an integer number of those events. `read_waveform_file` in `src/writer.cpp` uses each event's own mask and sample count. The acquisition loop writes one `channel_mask` and one record length for the whole run, which is what the example file is.

`load_waveforms` counts set bits in mask bits 0..15. `encode_waveform_event` sizes the payload with `popcount_u32` of the full 32-bit mask. Channel indexes here are 0..15, and mask `0x7` is 3 channels either way.

## Numpy

This is the dtype `load_waveforms` uses (`align=False`):

```python
import numpy as np

path = "data/v1730sb_20261026T143011Z/wf_00000.bin"
with open(path, "rb") as handle:
    prefix = handle.read(20)
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
        ("samples", "<i2", (n_channels, n_samples)),
    ],
    align=False,
)
waveforms = np.memmap(path, dtype=dtype, mode="r")["samples"]
# waveforms.shape == (n_events, n_enabled_channels, n_samples), dtype int16
```
