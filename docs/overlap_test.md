# Overlapping-trigger bench test (one board, B1)

Goal: find out which samples the V1730SB stores when a second trigger arrives while the first record window is still open, with and without overlap acceptance (0x8000 bit[1]). The result decides how overlapping events are joined into one waveform in the multi-board DAQ (PLAN.md, step 2c.1).

The manuals disagree:

* UM5118 (0x8000 bit[1] = 1): "the current acquisition window is prematurely closed by the arrival of a new trigger". The first event is cut short.
* UM2792 Fig. 8.8: the first event keeps its full length. The second event only stores the samples after the end of the first one; its pre-trigger part is "not recorded".

Both mean one continuous stream of samples, split at a different place. The test checks that, and where the split lies.

## Setup

```text
AFG3251 ──► linear fan-in/fan-out ──► B1 ch0
                                  └─► N96 ──► 428 trigger fan-out ──► TRG-IN of B1 (B2 to B4 not running)
```

AFG3251:

| Setting | Value |
| --- | --- |
| Function | Pulse |
| Output load | 50 Ω (Output menu, load impedance) |
| Levels | 0 V baseline, pulses to −300 mV (set high 0 mV, low −300 mV, polarity inverted if needed) |
| Pulse width | 50 ns, shortest edges |
| Period | spacing of the two pulses: 6 µs, 4 µs, 2 µs (see runs) |
| Burst | N-cycle, count 2 |
| Trigger | internal, interval 1 ms (1000 bursts/s) |

Check on the scope before running: two negative 50 ns pulses per burst at the chosen spacing, and one NIM pulse per input pulse at the N96 output.

Record window (configs below): 2500 samples = 5 µs, 750 samples = 1.5 µs before the trigger, 3.5 µs after it.

## Runs

`config/overlap_off.json` leaves overlap acceptance off (0x8000 bit[1] = 0), `config/overlap_on.json` sets it. Both read out B1 through the V4718. For each run:

```bash
./build/icxe-daq run --config config/overlap_off.json --max-events 2000
```

Set the AFG period first, then run both configs. Six runs:

| Run | Spacing | Config | What it tests |
| --- | --- | --- | --- |
| 1 | 6 µs | off | Reference: no overlap, 2 full events per burst |
| 2 | 6 µs | on | Same as 1 (nothing overlaps) |
| 3 | 4 µs | off | Second trigger after the first window ends (3.5 µs), but its pre-trigger part reaches back into the first window. Accepted? Same samples in both events? |
| 4 | 4 µs | on | Same question with overlap acceptance |
| 5 | 2 µs | off | Second trigger inside the first window: expected to be refused, 1 event per burst |
| 6 | 2 µs | on | The real overlap case: event lengths and split point |

Note for each run: the `directory=` line, the `register_writes: 0x8000 ...` line from stderr (readback must show bit 1 as configured), and the final `events=` line.

## What to send back

Copy the six run directories to `ICXe-DAQ/data/` on the development machine (the folder is not tracked by git). The metadata has a new `sample_counts` block (minimum, maximum, events shorter than the record), which already shows whether the board shortened events.

## How the result is read

* Within one burst, the trigger time tag difference of the two events must equal the spacing (to 16 ns).
* In the first event the first pulse sits near sample 750 (minus the NIM chain delay). If the second pulse is also in the first event, it sits at 750 + spacing / 2 ns per sample.
* Sample counts and pulse positions together show which samples each event holds:
  * UM5118 reading: first event shorter than 2500, second event full length.
  * Fig. 8.8 reading: first event full length, second event shorter, starting right after the first one ends.
  * Anything else (gap, duplicated samples) shows up as a pulse missing from, or appearing in both events.
