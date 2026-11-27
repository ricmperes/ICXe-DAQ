# ICXe-DAQ

Single-board waveform readout for a CAEN V1730SB on the direct A5818 optical link. One C++17 binary, `icxe-daq`, programs the digitizer from JSON and writes compact waveform files. The GUI (separate) edits the same JSON and starts this binary. Headless runs do not need Qt.

Config cheat sheet: [docs/config.md](docs/config.md).
Waveform file format: [docs/data_format.md](docs/data_format.md).

The crate only supplies power. The data path is the front-panel optical port, node 0. This program targets the standard waveform firmware. If `CAEN_DGTZ_GetDPPFirmwareType` reports DPP firmware, it stops.

Bring-up of the A5818 kernel module is separate. See [scripts/check_link.sh](scripts/check_link.sh) and [docs/drivers.md](docs/drivers.md).

## Build

CAENDigitizer, CAENComm, and CAENVME are expected in `/usr/lib`, with headers in `/usr/include`. JSON is the vendored single header `third_party/nlohmann/json.hpp`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/icxe-daq --self-test --config config/default.json
```

The self-test checks buffer-organization choice, JSON loading, and a waveform-file round trip. It does not open the digitizer.

## Commands

```bash
./build/icxe-daq info  --config config/four_boards.json --board B1
./build/icxe-daq run   --config config/overlap_on.json
./build/icxe-daq run   --config config/overlap_on.json --max-events 100
./build/icxe-daq run   --config config/overlap_on.json --duration 10
./build/icxe-daq scope --config config/four_boards.json --board B2 --channel 0
./build/icxe-daq reg   --config config/four_boards.json --board B3 --address 0x1070 [--value 0x7D]
```

A config lists one or more boards ([docs/config.md](docs/config.md)). `--board NAME` picks one; it can be left out when the config has only one board. Runs on several boards at once come with step 2c.4 (PLAN.md).

`reg` reads one register, or writes it and reads it back, without resetting or programming the board. It is meant for bench tests such as the MON/Σ majority test in [docs/mon_test.md](docs/mon_test.md), which also uses the optional `trigger_requests`, `analog_monitor` and `register_writes` config keys ([docs/config.md](docs/config.md)).

`info` prints board identity (model, serial, channel count, ADC bits, ROC/AMC firmware, DPP vs standard waveform, and the link used to open) and exits. If the optical link is down the open fails with `CAEN_DGTZ_DigitizerNotFound` (or a communication error) and points at the link-check script.

On 2026-10-26 `info` opened the V1730SB (serial 30176, 16 channels, standard waveform firmware, ROC `04.29 - Build 8716`, AMC `00.05 - Build 9130`). Board info `0x8140` was `0x10080b` (memory size field 8; this board's SRAM is 5.12 MS/channel). A following `run` programmed a 1020-sample record, then `CAEN_DGTZ_SWStartAcquisition` returned `CAEN_DGTZ_CommError` because acquisition status `0x8104` stayed 0. No waveform file was written. See [docs/smoke.md](docs/smoke.md).

`run` writes a run directory and does not plot. `scope` does not write run files. It streams one channel on stdout for an oscilloscope. Both stop on `duration_s`, or on `--duration S` when it is given (it replaces `duration_s` for that run), and `run` also stops on `--max-events`. The limits a run used are in its metadata (`stop_conditions`). SIGINT and SIGTERM close the run cleanly (`stop=sigint` or `stop=sigterm`).

Only the main thread calls the digitizer. A rate trigger is `CAEN_DGTZ_SendSWtrigger` on that same thread, paced with `clock_nanosleep`. The optical readout adds jitter. There is no internal periodic-trigger register. If the loop falls behind, missed periods are skipped rather than fired in a burst.

### Run status (stdout)

`run` writes only these lines to stdout, so a GUI can show a status line:

```text
directory=data/v1730sb_20261026T121314Z
events=0 file=0
events=1000 file=1
events=1500 file=1 stop=max_events
```

`file` is the index of `wf_XXXXX.bin` (0 if no event has been written yet). Programming details and baseline progress go to stderr.

## JSON

`config/four_boards.json` is a complete example: B1 to B4 through the V4718, channels 0 to 2 enabled on each, external NIM trigger, 2500-sample records with 750 pre-trigger samples, MON/Σ majority. Record lengths must be a multiple of 10; 1024 is rejected.

Every config key is required (no hidden defaults) and is described in [docs/config.md](docs/config.md). `config/four_boards.json` is a complete example.

There is no `CAEN_DGTZ_SetChannelGain` in the installed header. Dynamic range is register `0x1n28` (`0x1028` for channel 0, then +`0x100` per channel): `0` = 2 Vpp, `1` = 0.5 Vpp.

### Pre-trigger

Standard firmware has no sample-accurate pre-trigger call. The program sets

```text
post% = 100 * (record_length - pre_trigger_samples) / record_length
```

with integer division, calls `CAEN_DGTZ_SetPostTriggerSize`, reads the percent back, and stores the actual sample counts in `metadata.json`. A 1020-sample record with 200 pre-trigger samples asks for 80% post-trigger (`100 * 820 / 1020`), which is 816 post samples and 204 pre samples. The coarse step is written down rather than hidden.

### Buffer organization

Each channel has a fixed SRAM of 5.12 MS, sliced by register `0x800C` into `Nb = 2^k` equal buffers, `k = 0..10`. Custom record length (`CAEN_DGTZ_SetRecordLength`, register `0x8020`) makes the stored event shorter. It does not create more buffers. More buffers means more events can pile up while the PC drains the optical link, so a short waveform uses the largest `Nb` whose buffer still holds the record. The choice is printed on stderr (`k`, `Nb`, samples per buffer).

The sizes below are the manual table (the largest record that fits is that size minus 10):

| k | Nb | samples in one buffer |
| --- | --- | --- |
| 0 | 1 | 5120000 − 10 |
| 1 | 2 | 2560000 − 10 |
| 2 | 4 | 1280000 − 10 |
| 3 | 8 | 640000 − 10 |
| 4 | 16 | 320000 − 10 |
| 5 | 32 | 160000 − 10 |
| 6 | 64 | 80000 − 10 |
| 7 | 128 | 40000 − 10 |
| 8 | 256 | 20000 − 10 |
| 9 | 512 | 10000 − 10 |
| 10 | 1024 | 5120 − 10 |

A 1020-sample record therefore uses 1024 buffers of 5110 samples. The PC readout buffer from `CAEN_DGTZ_MallocReadoutBuffer` is separate.

### Triggers

* **external** — `CAEN_DGTZ_SetExtTriggerInputMode(ACQ_ONLY)`, self-triggers off, front panel NIM or TTL.
* **rate** — same software-controlled acquisition, but the readout loop calls `CAEN_DGTZ_SendSWtrigger` on a `clock_nanosleep` period.
* **coincidence** — self-trigger. The FPGA does not AND an arbitrary channel list. Adjacent pairs (0-1, 2-3, … 14-15) each emit one request. Register `0x1n84` (`n` = even channel: `0x1084`, `0x1284`, …) sets that couple to AND (`00`), only even (`01`), only odd (`10`), or OR (`11`). Register `0x810C` then applies the majority: bits `[7:0]` choose which pair-requests participate, bits `[23:20]` are the coincidence window in 8 ns steps (`window_ns` must be a multiple of 8, from 0 to 120), and bits `[26:24]` are the majority level. The board triggers when the number of pair requests inside the window is **greater than** that level.

Impossible coincidence settings are rejected: a channel in two pairs, a pair that is not one adjacent couple, logic that does not match the listed channels, or a majority greater than or equal to the number of enabled pair requests. To require both of two pairs, set majority to 1 (count > 1), not 2.

```json
"coincidence": {
  "majority": 1,
  "window_ns": 48,
  "pairs": [
    {"channels": [0, 1], "logic": "and"},
    {"channels": [2, 3], "logic": "or"}
  ]
}
```

`logic` is `and`, `or`, `even`, or `odd`. `even` lists only the even channel; `odd` lists only the odd channel. Thresholds are baseline ± the per-channel count, programmed after the DC-offset loop.

### Baseline

On start, if `apply_on_start` is true:

1. `CAEN_DGTZ_Calibrate` (ADC trim; keep the inputs quiet for the few seconds it runs).
2. Two DC-offset points, then a short loop: `CAEN_DGTZ_SetChannelDCOffset`, a handful of software triggers, mean and RMS, step the 16-bit DAC, stop at `tolerance_lsb` or `max_iterations`.
3. DAC code, mean, and RMS are written to `metadata.json`.

The nominal DAC for a target percent, used as the starting map, is `round((100 - target_percent) / 100 * 65535)` because the offset DAC is inverting. The measured slope replaces that guess. If a channel is still outside tolerance at `max_iterations`, the run continues and the metadata flag `baseline_within_tolerance` is false.

## Files

```text
data/<name>_<UTC>/
  config.json
  metadata.json
  wf_00000.bin
  wf_00001.bin
```

`config.json` is a copy of the JSON that was loaded. `metadata.json` holds board info, firmware, link, `Nb`, the real record length, requested and actual pre/post samples, per-channel DAC, baseline mean, RMS, and polarity, run start/end times, and a `chunks` list with the time span of each `wf_*.bin` (see [docs/data_format.md](docs/data_format.md)). The trigger time tag is stored raw and also unwrapped into a 64-bit tick count. `trigger_time_tag_tick_ns` is 8 (125 MHz). The sample period is 2 ns (500 MS/s).

Events are concatenated with no extra file header. Each event is little-endian:

| Bytes | Field |
| --- | --- |
| 4 | magic `ICX2` |
| 4 | `uint32` event counter (digitizer header) |
| 4 | `uint32` trigger time tag, raw |
| 4 | `uint32` channel mask |
| 4 | `uint32` sample count |
| 8 | `uint64` trigger time ticks, unwrapped (8 ns since acquisition start) |
| `2 * Nch * Ns` | `int16` ADC codes |

That is a 28-byte header, then samples. Runs written before this format use `ICX1`, the same header without the 8-byte tick field (20 bytes). Only enabled channels are stored, in ascending channel number. 14-bit codes sit in 16-bit words. Disabled channels are omitted. A 1020-sample, 16-channel event is about 32 KB plus the header.

```bash
python3 tools/read_waveforms.py data/v1730sb_<UTC>/wf_00000.bin \
    --metadata data/v1730sb_<UTC>/metadata.json
```

`load_waveforms(path)` memmaps the file and returns an array shaped `(n_events, n_channels, n_samples)` plus the per-event header columns. The channel axis is the enabled channels in ascending order. `metadata.json` has `channel_mask` and `enabled_channels` for that mapping.

## Scope frames

`scope` writes only binary frames to stdout, about 10 Hz (the newest waveform, not a backlog). Each frame is flushed:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `SCP1` |
| 4 | 4 | `uint32` `n_samples`, little-endian |
| 8 | `2 * n_samples` | `int16` samples, little-endian |

No other bytes are written to stdout in scope mode. Human status goes to stderr:

```text
events=<seen> emitted=<frames> channel=<ch> samples=<n>
```

`events` counts digitizer events drained. `emitted` counts frames actually written. The selected `--channel` must be enabled in the JSON.

## License

MIT. See `LICENSE`.
