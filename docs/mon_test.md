# MON/Σ majority bench test (one board)

Goal: find out whether the V1730 analog majority output can serve as a cheap 2-fold coincidence trigger with a 1 µs window. In the full system, the MON/Σ outputs of all boards are summed in a LeCroy 428 fan-in, discriminated by a CAEN N96, and fanned out to TRG-IN of every board.

What the manuals (UM2792 rev. 2, UM5118 rev. 5 in `caen_documents/`) say, and what is still open:

| Item | Manual | Open |
| --- | --- | --- |
| MON/Σ majority mode | 0x8144 bits[2:0] = 000 (default). Voltage proportional to the number of channels over threshold. Voltage-level mode: 0x8138, 0.244 mV per LSB into 50 Ω. | Step height: the UM2792 text says 125 mV, its figure 1.25 mV; UM5118 gives no number. |
| Self-trigger pulse | 0x1n84 bit[2] = 0 (default): a pulse whose width is 0x1n70 bits[7:0] in 8 ns steps (default 2 = 16 ns, max 255 = 2040 ns); bit[2] = 1: over/under-threshold level. Needs AMC ≥ 0.2 (ours: 0.5). | Whether MON counts the stretched pulse or the raw over-threshold level. |
| Requests without self-triggering | The self-trigger can be routed to acquisition (0x810C), TRG-OUT (0x8110), both, or neither. | Whether MON counts a channel whose request is routed nowhere. |
| Self-trigger polarity | One bit for the whole board, 0x8000 bit[6] (0 = over, 1 = under threshold). | None: all channels on a board must have the same pulse polarity for self-triggering. |
| Overlapping triggers | 0x8000 bit[1] = 1 accepts them: the running event is closed early and storage continues in the next buffer, so events have different lengths (UM2792 Fig. 8.8). | Exactly which samples the shortened event and the next event cover. |

## Config keys used

`config/mon_test.json` is the starting point: channels 0 and 1, negative pulses, threshold 30 counts (about 3.7 mV at 2 Vpp), external NIM trigger, 2500-sample records with 750 pre-trigger samples (1.5 µs), 60 s runs.

* `trigger_requests.width_ns` writes 0x1n70 = width_ns / 8 on each listed channel, reads it back, and warns on stderr if the board did not keep it. The values before and after, and the 0x1n84 pair registers, go to stderr and to `metadata.json` (`trigger_requests`).
* `trigger_requests.trg_out` also routes the requests to TRG-OUT (`EXTOUT_ONLY`). TRG-OUT then shows the OR of the requests, which is a useful second scope trace.
* `analog_monitor.mode`: `majority`, or `voltage_level` with `level_volts` to put a known DC level on MON for calibrating the scope and the NIM chain.
* `register_writes` holds raw writes for experiments (e.g. the shape bit of 0x1n84). Each one is read back and logged.

See [config.md](config.md) for the full key reference.

## Step 0: registers (no acquisition)

```bash
./build/icxe-daq reg --config config/mon_test.json --address 0x1070              # ch0 width as left by the last run
./build/icxe-daq reg --config config/mon_test.json --address 0x1084              # pair 0-1 logic/shape
./build/icxe-daq reg --config config/mon_test.json --address 0x1070 --value 0xFFFFFFFF
```

UM5118 says only bits[7:0] exist, so the readback should be `0xff` (2040 ns); anything else is worth noting. 0x1084 should read 0x3 after a reset (OR, pulse mode). `reg` does not reset the board. The next `run` resets it, which restores the defaults.

## Step 1: does MON show a ~1 µs step per hit?

1. Connect the signals to channels 0 and 1 (detector, or a pulser split into both). Connect MON/Σ to a scope with 50 Ω termination. TRG-IN stays unconnected, so no events are stored.
2. `./build/icxe-daq run --config config/mon_test.json`. The baseline step runs first, then 60 s of acquisition.
3. On the scope, each hit should be a step lasting about `width_ns`. Repeat with `width_ns` 16, 200 and 1000.
   * Step length follows `width_ns`: MON uses the stretched pulse. Continue.
   * Step length follows the input pulse only: MON uses the over-threshold level, and this approach cannot give a 1 µs window. Check that 0x1n84 bit[2] reads 0 in the stderr log. For comparison, `"register_writes": [{"address": "0x1084", "value": "0x4", "mask": "0x4"}]` switches pair 0-1 to the level shape.
   * No steps at all: set `trigger_requests.trg_out` to `true` and repeat.
4. With a pulser into both channels, two hits that overlap in time should give a double-height step.

## Step 2: step height and noise

1. Measure the height of one step and of two steps on the scope, and the baseline noise of MON.
2. Optional calibration: `"analog_monitor": {"mode": "voltage_level", "level_volts": 0.125}`. MON should then sit at 125 mV. This checks the scope and the termination, and later the gain through the 428.

## Step 3: close the loop through the NIM chain

```text
MON/Σ ──► 428 section (inverting) ──► N96 ──► 428 / N96 output ──► TRG-IN (NIM)
```

1. Threshold at about 0.5 step: every single hit triggers. The event rate is the singles rate r₀ + r₁ (in 1 µs-wide hits).
2. Threshold at about 1.5 steps: only overlaps of two stretched hits trigger, i.e. a 2-fold coincidence within `width_ns`.
3. The `events=` lines on stdout give the count. Rate = events / `duration_s`.
4. Check against the expectation for uncorrelated dark counts: rate₂ ≈ 2 · r₀ · r₁ · τ with τ = `width_ns`. Repeat at 200, 500, 1000 and 2000 ns: the 2-fold rate should grow in proportion to τ.
5. With a pulser into both channels, scan the delay between the two copies. Triggers should stop once the delay exceeds `width_ns`.

Keep the scope on MON while doing this. The trigger lands up to `width_ns` after the first hit, plus the delay through the NIM chain, so check in the stored waveforms that the first hit is still inside the 1.5 µs pre-trigger.

## Step 4 (optional): digital 2-of-4 on TRG-OUT

Fallback that needs no analog sum, for a trigger made only from detectors on one board (e.g. the 4 PMTs). With one detector per channel pair and the pair logic set to one channel, TRG-OUT can output a majority of the pair requests (0x8110 bits[9:8] = 10, level in bits[12:10]; UM5118 Sec. 1.23). With 1 µs request pulses, this is a 2-fold coincidence within 1 µs. Example for channels 0 and 2 on pairs 0 and 1, pairs switched to "only even channel":

```json
"trigger_requests": {"width_ns": 1000, "channels": [0, 2], "trg_out": true},
"register_writes": [
  {"address": "0x1084", "value": "0x1", "mask": "0x3"},
  {"address": "0x1284", "value": "0x1", "mask": "0x3"},
  {"address": "0x8110", "value": "0x603", "mask": "0xC0001FFF"}
]
```

`0x603` = pairs 0 and 1 enabled, majority mode, level 1, i.e. at least 2 requests. The mask also clears bits 31 and 30 (software trigger and TRG-IN to TRG-OUT, both 1 after reset), otherwise a TRG-OUT → TRG-IN cable loops every trigger back. Check TRG-OUT on the scope, then cable it to TRG-IN.

## Step 5: overlapping triggers

Superseded by [overlap_test.md](overlap_test.md), which uses the function generator's burst mode instead of a delay module. The writer now stores events of any length.

## Results to note

| Quantity | Value |
| --- | --- |
| 0x1n70 default and readback of 0xFFFFFFFF | 0x2 (16 ns) / 0xff (2040 ns), as in UM5118 |
| 0x1n84 default | 0x3 (OR, pulse mode) |
| MON step follows `width_ns`? | Yes: 1000 ns step at `width_ns` = 1000 with a 200 ns input pulse (ch0 only); 200 ns setting also matches |
| `trg_out` needed? | No |
| Step height (1 hit / 2 hits), MON noise | 125 mV / 250 mV, positive, noise about 5 mV peak-to-peak |
| MON counts channels or couples? | Couples: ch0 + ch1 (same pair) gives one 125 mV step, ch0 + ch2 gives 250 mV |
| Delay from input pulse to MON step | About 200 ns (7.5 m cable fan-out to digitizer, 0.5 m fan-out to scope) |
| Singles rate per channel | |
| 2-fold rate at 200 / 500 / 1000 / 2000 ns | |
| 2-fold trigger through 428 (inverted) + N96 at 170 mV | Works: ch0 + ch2 trigger the board, a single channel does not |
| NIM chain delay (TRG-IN after first hit) | |
| TRG-OUT 2-of-N majority works? (step 4) | |
| Overlapping trigger accepted, shortened event? (step 5) | |
