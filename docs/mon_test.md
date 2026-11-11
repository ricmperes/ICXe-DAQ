# MON/Σ majority bench test (one board)

Goal: find out whether the V1730 analog majority output can serve as a cheap 2-fold coincidence trigger with a 1 µs window. In the full system, the MON/Σ outputs of all boards are summed in a LeCroy 428 fan-in, discriminated by a CAEN N96, and fanned out to TRG-IN of every board.

What the manual (UM2792 rev. 2) says, and what is still open:

| Item | Manual | Open |
| --- | --- | --- |
| MON/Σ majority mode | 0x8144 bits[2:0] = 000 (default). Voltage proportional to the number of channels over threshold. | Step height: the text says 125 mV, the figure 1.25 mV. |
| Self-trigger pulse | 0x1n84 selects over-threshold level or a pulse; the pulse width is 0x1n70. The default after reset is "OR of two 16 ns pulses". | Whether MON counts the stretched pulse or the raw over-threshold level. Field width and unit of 0x1n70 (DPP manuals: 8 bits of 8 ns). |
| Requests without self-triggering | The self-trigger can be routed to acquisition, TRG-OUT, both, or neither. | Whether MON counts a channel whose request is routed nowhere. |

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

The last readback shows which bits of 0x1n70 exist; the largest width is (readback) × 8 ns. `reg` does not reset the board. The next `run` resets it, which restores the defaults. Note the values here.

## Step 1: does MON show a ~1 µs step per hit?

1. Connect the signals to channels 0 and 1 (detector, or a pulser split into both). Connect MON/Σ to a scope with 50 Ω termination. TRG-IN stays unconnected, so no events are stored.
2. `./build/icxe-daq run --config config/mon_test.json`. The baseline step runs first, then 60 s of acquisition.
3. On the scope, each hit should be a step lasting about `width_ns`. Repeat with `width_ns` 16, 200 and 1000.
   * Step length follows `width_ns`: MON uses the stretched pulse. Continue.
   * Step length follows the input pulse only: MON uses the over-threshold level. Try `register_writes` on 0x1n84 (see step 0) to change the shape. If nothing works, this approach cannot give a 1 µs window.
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

## Results to note

| Quantity | Value |
| --- | --- |
| 0x1n70 default and implemented bits | |
| 0x1n84 default | |
| MON step follows `width_ns`? | |
| `trg_out` needed? | |
| Step height (1 hit / 2 hits), MON noise | |
| Singles rate per channel | |
| 2-fold rate at 200 / 500 / 1000 / 2000 ns | |
| NIM chain delay (TRG-IN after first hit) | |
