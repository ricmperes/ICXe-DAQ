# Config cheat sheet

JSON passed to `icxe-daq --config`. The loader is `load_config` in `src/config.cpp`. A key that is not in this sheet is ignored. Enum strings ignore case.

`channels[].dynamic_range` is accepted and is not in `config/default.json`. Every other key below is in that file.

## Settings that fail a run

- `record_length` must be a positive multiple of 10 and must fit in one SRAM buffer. 1024 is rejected. With `n_buffers` null, the longest record is 5119990 samples; a shorter record uses the largest `Nb` that still holds it.
- `connection.use_vme_base_address` stays `false` on this direct optical link. The rotary address 3214 is `0x32140000`. While the flag is false that value is parsed and stored, and the open base passed to the digitizer is 0.
- `trigger.type` is `external`, `coincidence`, or `rate`.
- Coincidence pairs are adjacent channels only (0-1, 2-3, …, 14-15). `logic` is `and`, `or`, `even`, or `odd`: `and` / `or` list both channels of the couple, `even` lists only the even channel, and `odd` lists only the odd channel. `majority` must be smaller than the number of pair requests (the board fires when the request count is greater than that level). `window_ns` is in nanoseconds, on a hardware step of 8 ns (0, 8, …, 120).
- `channels[].polarity` is `positive` or `negative`. `channels[].target_percent` is 0 to 100.
- `dynamic_range` is `2Vpp` or `0.5Vpp`, including a per-channel override.

`eth_v4718` and `usb_v4718` load, then fail when the digitizer is opened. Only `optical` runs.

## Connection

`connection` is a required object.

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `connection.link_type` | string | `optical` | `optical`, `eth_v4718`, `usb_v4718` | Selects the link. This board opens `CAEN_DGTZ_OpticalLink`. |
| `connection.link_num` | integer | `0` | `>= 0` | A5818 link index. This board is link 0. |
| `connection.conet_node` | integer | `0` | `>= 0` | Daisy-chain node. This board is node 0. |
| `connection.vme_base_address` | string | `0x32140000` | whole string parses as a 32-bit integer (`0x` hex or decimal) | Rotary address. 3214 is `0x32140000`. |
| `connection.use_vme_base_address` | boolean | `false` | `true` or `false` | `false` opens with base 0, so the rotary address is not passed. |

## Channels

`channels` is a required non-empty array of objects. Indexes are unique and at least one channel is enabled. Channels you leave out of the array stay disabled.

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `channels[].index` | integer | required (`0`, `1`, `2` in default.json) | `0..15`, unique | Digitizer channel this object programs. |
| `channels[].enabled` | boolean | `true` | `true` or `false` | `true` puts the channel in the enable mask and in the waveform file. |
| `channels[].polarity` | string | required (`positive` on 0, `negative` on 1 and 2) | `positive`, `negative` | Sets pulse polarity and the self-trigger edge (rising for positive, falling for negative). |
| `channels[].target_percent` | number | required (`15`, `85`, `85`) | `0..100` | Baseline as a percent of the 14-bit range; a positive pulse uses a low percent and a negative pulse uses a high one. |
| `channels[].threshold` | integer | `100` in default.json; omit uses `0` | `>= 0`, and baseline ± this count must stay inside ADC `0..16383` | Counts above a positive baseline or below a negative baseline, written as the coincidence threshold after the baseline measurement. |

## Record length and pre-trigger

Both keys are required.

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `record_length` | integer | `1020` | positive multiple of 10 that fits in one buffer | Samples stored per channel. |
| `pre_trigger_samples` | integer | `200` | `0..record_length` | Samples requested before the trigger; the board stores a post-trigger percent, so 1020 and 200 ask for 80% post and come back as 204 pre and 816 post. |

There is no post-trigger key. Post percent is `100 * (record_length - pre_trigger_samples) / record_length` with integer division.

## Buffer

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `n_buffers` | null or integer | `null` | `null`, or `Nb = 2^k` for `k = 0..10`, and the record must fit | `null` picks the largest buffer count whose slice still holds `record_length`; an integer forces that count. |
| `max_events_blt` | integer | `32` | `1..1024` | Events requested per `ReadData` call. |

SRAM is 5.12 MS/channel, split into `Nb` equal buffers. The longest record in a buffer is the slice size minus 10. A 1020-sample record uses `Nb = 1024` (5110 samples in the buffer).

| k | Nb | max `record_length` |
| --- | --- | --- |
| 0 | 1 | 5119990 |
| 1 | 2 | 2559990 |
| 2 | 4 | 1279990 |
| 3 | 8 | 639990 |
| 4 | 16 | 319990 |
| 5 | 32 | 159990 |
| 6 | 64 | 79990 |
| 7 | 128 | 39990 |
| 8 | 256 | 19990 |
| 9 | 512 | 9990 |
| 10 | 1024 | 5110 |

## Dynamic range

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `dynamic_range` | string | `2Vpp` | `2Vpp`, `0.5Vpp` | Input span for every enabled channel that does not set its own. |
| `channels[].dynamic_range` | string | omit uses the top-level value; not in default.json | `2Vpp`, `0.5Vpp` | Input span for that channel. |

## Trigger

`trigger` is a required object.

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `trigger.type` | string | `external` | `external`, `coincidence`, `rate` | `external` arms the front panel, `rate` sends a software trigger from the readout thread, and `coincidence` arms adjacent-pair self-triggers. |
| `trigger.io_level` | string | `NIM` | `NIM`, `TTL`; the key is required when type is `external` | Front-panel logic level, programmed for every trigger type. |
| `trigger.rate_hz` | number | `10` in default.json; omit uses `0` | `> 0` and at most `1000000` when type is `rate` (period at least 1000 ns); the key is required for `rate` | Software-trigger rate in hertz. Missed periods are skipped. |
| `trigger.coincidence` | object or null | object below; required when type is `coincidence` | object, or null / omitted for the other types | Pair-request settings. A present object is checked even when type is not `coincidence`. |
| `trigger.coincidence.majority` | integer | `0` | `0..7` and strictly less than the number of pairs | Trigger when the number of pair requests in the window is greater than this. Two pairs that must both fire use `1`. |
| `trigger.coincidence.window_ns` | integer | `48` | `0, 8, 16, …, 120` | Coincidence window in nanoseconds. The hardware step is 8 ns. |
| `trigger.coincidence.pairs` | array | one pair, channels `[0, 1]`, logic `and` | at least one object | One request per adjacent couple. A channel and a couple appear once, and every listed channel is enabled. |
| `trigger.coincidence.pairs[].channels` | array of integers | `[0, 1]` | one or two channels of one couple `2i, 2i+1` | Channels that form that pair request. |
| `trigger.coincidence.pairs[].logic` | string | `and` | `and`, `or`, `even`, `odd` | `and` and `or` list both channels; `even` lists only the even channel; `odd` lists only the odd channel. |

## Trigger requests, analog monitor, raw registers

All three are optional and absent from `config/default.json`. `config/mon_test.json` uses the first two; the bench procedure is [mon_test.md](mon_test.md). They are applied after the trigger is armed, in this order, and every register is read back. Values go to stderr and `metadata.json`.

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `trigger_requests` | object or null | absent | object | Programs thresholds (baseline ± `channels[].threshold`) and the self-trigger pulse width on the listed channels, also when `trigger.type` is `external` or `rate`. The requests then do not start the acquisition, but MON/Σ majority and (with `trg_out`) TRG-OUT see them. With `coincidence`, the pair channels also get the width. |
| `trigger_requests.width_ns` | integer | `0` (register left at reset value, 16 ns per UM2792) | multiple of 8 | Writes 0x1n70 = `width_ns / 8`. A readback that differs is a stderr WARNING; the board's field width is not documented for this firmware. |
| `trigger_requests.channels` | array of integers | every enabled channel | enabled channels with `threshold >= 1` | Channels that get threshold and width. |
| `trigger_requests.trg_out` | boolean | `false` | `true` or `false` | Also routes the requests to TRG-OUT (`EXTOUT_ONLY`; with `coincidence`, `ACQ_AND_EXTOUT`). |
| `analog_monitor.mode` | string | required in the object | `majority`, `test`, `buffer_occupancy`, `voltage_level` | MON/Σ mode, 0x8144 bits[2:0] = 0, 1, 3, 4. `majority` is proportional to the number of channels over threshold. |
| `analog_monitor.level_volts` | number | required for `voltage_level` only | `0 .. 4095/4096` | DC level on MON, 0x8138 = round(V × 4096). |
| `register_writes` | array or null | absent | objects below | Raw writes for bench tests, in order, after everything else. |
| `register_writes[].address` | integer or `"0x..."` | required | multiple of 4, `0x0000..0xFFFC` | Register address. |
| `register_writes[].value` | integer or `"0x..."` | required | 32-bit | Value to write. |
| `register_writes[].mask` | integer or `"0x..."` | `0xFFFFFFFF` | non-zero | Only these bits change: `new = (old & ~mask) | (value & mask)`. |

`icxe-daq reg --config <file> --address 0xADDR [--value 0xVALUE]` reads one register, or writes it and reads it back, without resetting or programming the board.

## Baseline

`baseline` may be omitted. The code defaults match default.json.

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `baseline.apply_on_start` | boolean | `true` | `true` or `false` | `true` calibrates the ADCs and walks the DC offset to each `target_percent` before acquisition. |
| `baseline.tolerance_lsb` | number | `5` | `>= 0` | Stop stepping a channel when its mean is this close to the target, in ADC counts. |
| `baseline.max_iterations` | integer | `12` | `>= 1` | Cap on DC-offset steps after the two-point estimate. A channel still outside tolerance does not abort the run. |
| `baseline.triggers_per_step` | integer | `8` | `>= 1` | Software triggers averaged for each baseline measurement. |

## Output

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `name` | string | `v1730sb` in default.json; omit uses `run` | 1..64 of letters, digits, `-`, `_` | Prefix of the run directory `<output_directory>/<name>_<UTC>`. |
| `output_directory` | string | `data` | non-empty | Parent directory of the run folder. It is created if missing. |
| `waveforms_per_file` | integer | `1000` | `>= 1` | Events in one `wf_XXXXX.bin` before the next file opens. |

## Stop conditions

| Path | Type | Default | Allowed | What it does |
| --- | --- | --- | --- | --- |
| `duration_s` | number | `0` | `>= 0` | Stop after this many seconds of acquisition. `0` means no time limit. Baseline time is outside this clock. |

`run --max-events N` (`N >= 1`), SIGINT, and SIGTERM also stop a run. They are not JSON keys. `scope` uses `duration_s` and the signals, and ignores `--max-events`.

## Example

Same keys and values as `config/default.json`:

```json
{
  "name": "v1730sb",
  "output_directory": "data",
  "duration_s": 0,
  "waveforms_per_file": 1000,
  "record_length": 1020,
  "pre_trigger_samples": 200,
  "n_buffers": null,
  "dynamic_range": "2Vpp",
  "max_events_blt": 32,
  "connection": {
    "link_type": "optical",
    "link_num": 0,
    "conet_node": 0,
    "vme_base_address": "0x32140000",
    "use_vme_base_address": false
  },
  "baseline": {
    "apply_on_start": true,
    "tolerance_lsb": 5,
    "max_iterations": 12,
    "triggers_per_step": 8
  },
  "channels": [
    {
      "index": 0,
      "enabled": true,
      "polarity": "positive",
      "target_percent": 15,
      "threshold": 100
    },
    {
      "index": 1,
      "enabled": true,
      "polarity": "negative",
      "target_percent": 85,
      "threshold": 100
    },
    {
      "index": 2,
      "enabled": true,
      "polarity": "negative",
      "target_percent": 85,
      "threshold": 100
    }
  ],
  "trigger": {
    "type": "external",
    "io_level": "NIM",
    "rate_hz": 10,
    "coincidence": {
      "majority": 0,
      "window_ns": 48,
      "pairs": [
        {
          "channels": [0, 1],
          "logic": "and"
        }
      ]
    }
  }
}
```
