# Config cheat sheet

JSON passed to `icxe-daq --config`. The loader is `load_config` in `src/config.cpp`. Enum strings ignore case.

**Every key is required and nothing has a default.** A file states every setting the program makes; a missing key and a key that is not in this sheet are both errors. Where a feature is not used, its key is set to `null` (or `[]` for lists), as listed below. Board settings that have no key are left at the values the board reset gives them (the program resets each board before programming it).

Rule for developers (CLAUDE.md): a new or changed key is documented here in the same change.

## File layout

One file describes a run on one or several boards: shared settings at the top level, and a `boards` list with what differs per board. Every board lists all 16 channels. The loader merges the shared settings into every board entry and checks each board as a whole; an error names the board (`board B2: ...`).

Complete example: `config/four_boards.json`. The other files in `config/`: `overlap_on.json` / `overlap_off.json` (B1, the overlap bench test), and `default.json`, `mon_test.json`, `PMT1.json`, `PMT_ext.json` (the board on its own optical port, serial 30176, named B5; refused while 30176 is protected).

`icxe-daq info`, `run`, `scope` and `reg` work on one board. With a file that lists several boards, `--board NAME` says which one (`--board B3`); with a one-board file it can be left out. A run on several boards at once comes with step 2c.4 (PLAN.md).

## Order of a run

What the program does with the keys, in this order, so the descriptions below can refer to it:

1. Open the board (connection, `boards[].vme_base_address`), check that it reports `boards[].serial` and is not protected, and check its status (clock source as `boards[].clock_source`, channels not in over-temperature shutdown, PLL locked; register 0x8104) and its failure register (no PLL lock loss, temperature failure or ADC power down; register 0x8178). Nothing is written before these checks pass.
2. Reset the board, then program record length, pre-trigger, buffers, I/O level, enabled channels, polarity, dynamic range and the nominal DC offset of each enabled channel.
3. Baseline step (`baseline`): ADC calibration, then the DC offset is adjusted until each channel sits at its `target_percent`. Uses software triggers; nothing is written to disk.
4. Arm the trigger (`trigger`, `boards[].coincidence`), then `trigger_requests`, `analog_monitor`, `accept_overlapping_triggers`, `register_writes`, in this order. Every register written here is read back; the values go to stderr and `metadata.json`.
5. Check the status once more (now also "board ready"), record temperatures, then acquire and write files until a stop condition (`duration_s`, `--max-events`, Ctrl-C). The status (0x8104 and 0x8178) is checked once per second; a problem (over-temperature, shutdown, ADCs powered down, clock or PLL) stops the run with `stop_reason` `board_status` ([data_format.md](data_format.md)).

## Shared keys

### Output and stop

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `name` | string | 1..64 of letters, digits, `-`, `_` | Name of the run. The run directory is `<output_directory>/<name>_<UTC start time>`, e.g. `data/four_boards_20261127T101500Z`; if that exists, `-2`, `-3`, … is appended. Use it to tell runs apart (e.g. `overlap_on_4us`). |
| `output_directory` | string | non-empty | Folder in which the run directory is created (relative to where the program is started, or absolute). It is created if missing. |
| `duration_s` | number | `>= 0` | Length of the acquisition in seconds, counted from the start of acquisition (the baseline step before it does not count). `0` means no time limit: the run then stops only on `--max-events N` or Ctrl-C / SIGTERM. |
| `waveforms_per_file` | integer | `>= 1` | Number of events in one waveform file `wf_XXXXX.bin` (one chunk). When a file is full, the next one is opened. Small values give many small chunks; each chunk gets its own start and end time in the metadata. |

`run --max-events N` (`N >= 1`), SIGINT, and SIGTERM also stop a run. They are not JSON keys. `scope` uses `duration_s` and the signals, and ignores `--max-events`.

### Record and buffer

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `record_length` | integer | positive multiple of 10 that fits in one buffer (table below) | Length of the recording window per trigger, in samples of 2 ns: 2500 samples = 5 µs. Every enabled channel stores this many samples per event (fewer when an overlapping trigger shortens an event, see `accept_overlapping_triggers`). Sets registers 0x800C/0x8020. |
| `pre_trigger_samples` | integer | `0..record_length` | How much of the window lies before the trigger, in samples. The board does not take a sample count but a post-trigger percentage (`100 * (record_length - pre_trigger_samples) / record_length`, rounded down), so the real value can differ slightly: 1020 and 200 give 80 % post, i.e. 204 pre and 816 post samples. The real values are in `metadata.json` (`pre_trigger_samples_actual`). |
| `n_buffers` | null or integer | `null`, or `Nb = 2^k` for `k = 0..10` with the record fitting | Number of events the board can hold before it is full. The board memory (5.12 MS per channel) is split into `Nb` equal buffers, one event each. `null` picks the largest `Nb` that still holds `record_length` (best: most events can wait while the PC reads), an integer forces that number. When all buffers are full, the board drops triggers (dead time). |
| `max_events_blt` | integer | `1..1024` | Maximum number of events the PC fetches from the board in one read (one block transfer over the link). Larger values mean fewer, bigger transfers and less overhead at high rates; the PC read buffer grows with it. 32 is fine for bench rates. |

The board memory and how many events fit, per `Nb` (the longest record is the buffer size minus 10 samples):

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

### Connection

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `connection.link_type` | string | `optical`, `eth_v4718`, `usb_v4718` | How the PC reaches the boards. `optical`: the A5818 card and a fibre, either to the V4718 bridge (boards reached over the VME backplane) or to a board's own optical port. `eth_v4718` / `usb_v4718` (V4718 over Ethernet or USB) are accepted by the loader but not implemented: opening fails. |
| `connection.link_num` | integer | `>= 0` | Which optical port of the A5818 card the fibre is plugged into (our card: 0). |
| `connection.conet_node` | integer | `>= 0` | Position on the optical daisy chain behind that port. With one device on the fibre (the V4718, or one board) it is 0. |
| `connection.use_vme_base_address` | boolean | `true` or `false` | `true`: the fibre goes to the V4718 and each board is addressed over VME by its `boards[].vme_base_address` (needed for several boards). `false`: the fibre goes to the board's own optical port; the base address is then not used (the board is opened with address 0) and only one board is allowed. |

### Baseline

The baseline step runs before the acquisition. It needs the inputs quiet (no pulses, or a low rate), because it averages whole waveforms taken with software triggers.

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `baseline.apply_on_start` | boolean | `true` or `false` | `true`: before the run, calibrate the ADCs (`CAEN_DGTZ_Calibrate`, a few seconds), measure each enabled channel at two DC-offset settings, compute the offset that puts the baseline at `target_percent`, then correct it step by step. The result (DAC code, mean, RMS) is in `metadata.json`. `false`: no calibration and no measurement; each channel gets the nominal DC offset computed from `target_percent` alone, so the real baseline is not checked and can differ from the target (thresholds then use the target, not a measured baseline). |
| `baseline.tolerance_lsb` | number | `>= 0` | How close (in ADC counts) the measured baseline mean must be to the target for a channel to count as done. Smaller values may need more steps. |
| `baseline.max_iterations` | integer | `>= 1` | Maximum number of correction steps after the two-point estimate. When it is reached, the run still starts; channels outside tolerance are marked `baseline_within_tolerance: false` in the metadata and reported on stderr. |
| `baseline.triggers_per_step` | integer | `>= 1` | Number of waveforms (software triggers) averaged for each baseline measurement. More gives a more precise mean but takes longer; a pulse that happens to fall into these waveforms shifts the mean. |

### Trigger

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `trigger.type` | string | `external`, `coincidence`, `rate` | What starts an event. `external`: a pulse on the front-panel TRG-IN (our NIM chain), self-triggers off. `coincidence`: the board triggers itself from its channels, using the pair logic in `boards[].coincidence` and the channel `threshold`s; TRG-IN is ignored. `rate`: the program sends software triggers at `trigger.rate_hz` (random-trigger runs, e.g. for noise or baseline studies); TRG-IN is ignored. |
| `trigger.io_level` | string | `NIM`, `TTL` | Signal standard of the front-panel LEMO connectors (TRG-IN, TRG-OUT, S-IN), set for every trigger type. Our NIM chain (N96, 428) needs `NIM`; a wrong level means TRG-IN never sees a pulse. |
| `trigger.rate_hz` | number or null | `> 0` and at most `1000000` (period at least 1000 ns) when type is `rate`; any number or `null` otherwise (not used) | Software-trigger rate for `trigger.type` `rate`. The triggers come from the PC, so their timing has some jitter; if the PC falls behind, missed triggers are skipped, not sent late. Not used for the other types; `null` makes that visible. |
| `accept_overlapping_triggers` | boolean | `true` or `false` | Register 0x8000 bit[1]. `false` (board reset value): after a trigger, the board ignores every further trigger until one record length (`record_length` × 2 ns) has passed, even if the window has already closed; such triggers are lost and not counted. `true`: a trigger inside the open window is accepted; the first event keeps its full length and the next one holds only the samples after it, so the two join without gap or duplicated samples (measured, [overlap_test.md](overlap_test.md)). The TPC needs `true`, otherwise an S2 within one record length of the S1 is lost. Written after the trigger is armed and before `register_writes`, read back, and logged. |

### Trigger requests, analog monitor, raw registers

A trigger request is the board's own decision that a channel is over threshold. It can start an event (`trigger.type` `coincidence`), but it can also be used only for monitoring while the run is started by TRG-IN: that is what `trigger_requests` does. The bench procedure that uses these keys is [mon_test.md](mon_test.md).

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `trigger_requests` | object or null | object, or `null` | Turns on threshold detection on the channels in `boards[].trigger_request_channels` without letting it start events (with `trigger.type` `external` or `rate`): each such channel gets its threshold (measured baseline ± `threshold`) and a request pulse of `width_ns`. The pulses then show up on MON/Σ in majority mode (our analog 2-fold trigger) and, with `trg_out`, on TRG-OUT. With `coincidence`, the pair channels also get the width. `null`: thresholds are not programmed unless `trigger.type` is `coincidence`, and MON/Σ shows nothing. |
| `trigger_requests.width_ns` | integer | multiple of 8, `0..2040` | Length of the request pulse a channel produces when it crosses threshold (register 0x1n70 = `width_ns / 8`). On MON/Σ each hit becomes a step of this length, so two hits within `width_ns` overlap and add up: this is the coincidence window of the analog 2-fold trigger (1000 ns = 1 µs). `0` leaves the register at its reset value (16 ns). |
| `trigger_requests.trg_out` | boolean | `true` or `false` | `true` also sends the requests (OR of the listed channels) to the TRG-OUT connector, e.g. to look at them on a scope or to feed external logic. `false` leaves TRG-OUT as the board reset sets it. |
| `analog_monitor` | object or null | object, or `null` | Settings of the analog MON/Σ output on the front panel. `null`: the register is not written and MON/Σ stays at its reset mode (majority). |
| `analog_monitor.mode` | string | `majority`, `test`, `buffer_occupancy`, `voltage_level` | What MON/Σ outputs (register 0x8144 bits[2:0]). `majority`: a voltage proportional to the number of channel pairs with an active request, 125 mV per pair (measured); summed over boards in the 428, this is our trigger signal. `test`: a sawtooth of 1 V amplitude at 24.41 kHz (checks the cable and scope). `buffer_occupancy`: a voltage proportional to the number of filled buffers, up to about 1 V when the memory is full (useful to watch the readout keep up and to see dead time coming). `voltage_level`: a fixed DC level from `level_volts` (to calibrate the scope or the NIM chain). |
| `analog_monitor.level_volts` | number or null | `0 .. 4095/4096` for `voltage_level`, `null` for the other modes | DC level on MON/Σ in mode `voltage_level` (register 0x8138 = round(V × 4096)). E.g. `0.125` puts out the height of one majority step. |
| `register_writes` | array | objects below, or `[]` | Raw register writes for bench tests, applied to every board after all other settings, in the order given. Each write is read back and logged. Meant for trying out a register before it gets a proper key; a setting used regularly should get its own key. `[]`: none. |
| `register_writes[].address` | integer or `"0x..."` | multiple of 4, `0x0000..0xFFFC` | Register address on the board, as in UM5118 (e.g. `"0x8000"`). `0x1n..` registers are per channel (n = channel number). |
| `register_writes[].value` | integer or `"0x..."` | 32-bit | Value to write (only the bits in `mask` are used). |
| `register_writes[].mask` | integer or `"0x..."` | non-zero | Which bits to change; the others keep their current value: `new = (old & ~mask) | (value & mask)`. `"0xFFFFFFFF"` writes the whole register. |

`icxe-daq reg --config <file> [--board NAME] --address 0xADDR [--value 0xVALUE]` reads one register, or writes it and reads it back, without resetting or programming the board.

### Protected boards

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `protected_serials` | array of integers | positive integers, or `[]` | Serial numbers of boards that belong to other users and must never be opened, reset or reprogrammed. A board entry with one of these serials is refused when the config is loaded, before anything is opened. After opening, the serial the board reports is also checked against this list, so a wrong base address that reaches a protected board stops before any write. `[]`: no protected boards. |

## Boards

`boards` is a non-empty array with one entry per board. Names, serials and base addresses are unique. With several boards, `connection.use_vme_base_address` is `true` and exactly one board has `clock_source` `internal`.

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `boards[].name` | string | 1..64 of letters, digits, `-`, `_` | Name of the board, used by `--board`, in error messages and in the metadata. Convention: B1 to B4 from left to right in the crate. |
| `boards[].vme_base_address` | string | whole string parses as a 32-bit integer (`0x` hex or decimal) | The board's VME address, set with its rotary switches (four switches 3, 2, 1, 0 give `0x32100000`). The V4718 reaches the board under this address. Not used when `connection.use_vme_base_address` is `false`. |
| `boards[].serial` | integer | `>= 1`, not in `protected_serials` | Serial number printed on the board and reported by `info`. Guards against a wrong address reaching another board: right after opening, every command (`info`, `reg`, `run`, `scope`) compares the serial the board reports with this value and stops before writing anything if they differ (`info` prints the board first, so you can see what is at that address). |
| `boards[].clock_source` | string | `internal`, `external` | Which clock the board runs on, as set by its clock switch (SW4 on our boards): `internal` for the clock master (B1), `external` for boards that get the clock from the board before them in the CLK-OUT → CLK-IN chain. The program cannot set this; `run` and `scope` check it (register 0x8104 bit 5) before programming, before the start and once per second, and stop on a mismatch. `info` shows it. |
| `boards[].coincidence` | object or null | object below, or `null` | Self-trigger logic of this board: which channel pairs must be over threshold to start an event. Required when `trigger.type` is `coincidence`; `null` otherwise (a present object is still checked). |
| `boards[].trigger_request_channels` | array of integers | enabled channels with `threshold >= 1`; non-empty when `trigger_requests` is an object, `[]` when it is `null` | Channels of this board that get threshold detection from `trigger_requests` (e.g. the channels that should contribute to MON/Σ). |
| `boards[].register_writes` | array | as `register_writes`, or `[]` | Raw writes for this board only, applied after the shared `register_writes`. |
| `boards[].channels` | array | all 16 channels, indexes `0..15` once each | Settings of every channel of this board, see Channels. |

### Coincidence

Self-triggering on the V1730 works on channel pairs (0-1, 2-3, …, 14-15): each pair gives one request, formed from its two channels by `logic`. The board starts an event when more than `majority` pair requests are active within `window_ns`. The pair registers are 0x1n84, the majority setting 0x810C.

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `coincidence.majority` | integer | `0..7` and strictly less than the number of pairs | The board triggers when the number of active pair requests is **greater than** this number. `0`: any one pair triggers. `1`: two pairs needed (e.g. 2-fold coincidence of two pairs). |
| `coincidence.window_ns` | integer | `0, 8, 16, …, 120` | Time window in which the pair requests must coincide to count together, in steps of 8 ns. Short, because it is meant for pulses that arrive nearly at once; for longer windows use the request width (`trigger_requests.width_ns`) and MON/Σ. |
| `coincidence.pairs` | array | at least one object | The pairs that take part. Every channel and every pair appears once, and every listed channel is enabled. |
| `coincidence.pairs[].channels` | array of integers | one or two channels of one pair `2i, 2i+1` | Channels that form this pair request. |
| `coincidence.pairs[].logic` | string | `and`, `or`, `even`, `odd` | How the pair request is formed: `and` both channels over threshold, `or` either of them, `even` only the even channel counts, `odd` only the odd channel counts. `and` and `or` list both channels; `even` and `odd` list only that one. |

### Channels

Each board lists all 16 channels. Only enabled channels are programmed and stored; the settings of a disabled channel are checked but not used, so a channel can be switched on later by changing only `enabled`. At least one channel per board is enabled.

| Path | Type | Allowed | What it does |
| --- | --- | --- | --- |
| `channels[].index` | integer | `0..15`, each once | Channel number on the board (front-panel input). |
| `channels[].enabled` | boolean | `true` or `false` | `true`: the channel is recorded; every event stores its waveform. `false`: the channel is not recorded and takes no space in the files. With an external trigger, every enabled channel stores a waveform for every trigger, whether it saw a pulse or not. |
| `channels[].polarity` | string | `positive`, `negative` | Direction of the pulses: `negative` for PMT and SiPM pulses that go down. Sets the threshold side (below or above the baseline) and the trigger edge (falling for negative, rising for positive). |
| `channels[].target_percent` | number | `0..100` | Where the baseline sits in the 14-bit ADC range (0 to 16383), in percent, set with the channel's DC offset. Negative pulses need room below the baseline, so a high value (`85` = ADC 13926, leaving 85 % of the range for the pulse); positive pulses a low value (`15`). Reached precisely only with `baseline.apply_on_start`. |
| `channels[].threshold` | integer | `>= 0`, and baseline ± this count must stay inside ADC `0..16383` | Distance from the measured baseline, in ADC counts, at which the channel counts as hit (1 count = 0.122 mV at 2 Vpp, so 30 counts = 3.7 mV). Used only for self-triggering: with `trigger.type` `coincidence`, or for the channels in `trigger_request_channels`. With a plain external trigger it has no effect. |
| `channels[].dynamic_range` | string | `2Vpp`, `0.5Vpp` | Input range of the channel (register 0x1n28): `2Vpp` (0.122 mV per count) or `0.5Vpp` (0.031 mV per count, for small signals; pulses above 0.5 V clip). |
