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
| Levels | 0 V between and inside bursts, pulses down to −300 mV. Set a positive pulse (high +300 mV, low 0 mV), then switch Invert on in the Output menu. Setting high 0 mV / low −300 mV instead leaves the output at −300 mV between bursts, which gives one long negative level with short pulses up to 0 V. |
| Pulse width | 50 ns, shortest edges |
| Period | spacing of the two pulses: 6 µs, 4 µs, 2 µs (see runs) |
| Burst | N-cycle, count 2 |
| Trigger | internal, interval 1 ms (1000 bursts/s) |

Check on the scope before running, after the fan-out: the level between bursts is 0 V, there are two negative 50 ns pulses per burst at the chosen spacing, and there is one NIM pulse per input pulse at the N96 output.

Record window (configs below): 2500 samples = 5 µs, 750 samples = 1.5 µs before the trigger, 3.5 µs after it.

## Runs

`config/overlap_off.json` leaves overlap acceptance off (`accept_overlapping_triggers: false`, 0x8000 bit[1] = 0), `config/overlap_on.json` sets it. (The runs on 2026-11-25 used the same bit through `register_writes`.) Both read out B1 through the V4718. For each run:

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

Note for each run: the `directory=` line, the `accept_overlapping_triggers: ...` line from stderr (the 0x8000 readback must show bit 1 as configured), and the final `events=` line.

## What to send back

Copy the six run directories to `ICXe-DAQ/data/` on the development machine (the folder is not tracked by git). The metadata has a new `sample_counts` block (minimum, maximum, events shorter than the record), which already shows whether the board shortened events.

## How the result is read

* Within one burst, the trigger time tag difference of the two events must equal the spacing (to 16 ns).
* In the first event the first pulse sits near sample 750 (minus the NIM chain delay). If the second pulse is also in the first event, it sits at 750 + spacing / 2 ns per sample.
* Sample counts and pulse positions together show which samples each event holds:
  * UM5118 reading: first event shorter than 2500, second event full length.
  * Fig. 8.8 reading: first event full length, second event shorter, starting right after the first one ends.
  * Anything else (gap, duplicated samples) shows up as a pulse missing from, or appearing in both events.

## Results (2026-11-25)

Runs in `data/overlap_{off,on}_{2,4,6}us_*`, 2000 events each. Pulse spacings measured from the time tags and the samples: 5.98, 4.10 and 2.14 µs. Pulse leading edge at sample 736–744, i.e. 6–14 samples (12–28 ns) before the trigger sample 750.

| Spacing | Overlap off | Overlap on |
| --- | --- | --- |
| 6 µs | 2 full events per burst | 2 full events per burst |
| 4 µs | 1 full event per burst; second trigger dropped | full event, then an event of 2048 samples whose pulse sits 10 samples before its trigger |
| 2 µs | 1 full event per burst, holding both pulses; second trigger dropped | full event holding both pulses, then an event of 1072 samples without a pulse |

Rule found, with no exception in 1081 shortened events:

* The first event keeps its full length (UM2792 Fig. 8.8; the UM5118 wording "prematurely closed" does not describe this firmware).
* Every event ends at trigger + post-trigger. A shortened event starts where the previous event ends, so its sample count is (trigger spacing) / 2 ns and the two concatenate without gap or duplicated samples.
* With overlap acceptance off, a trigger less than one record length after the previous trigger is dropped, even if the previous window has already closed (4 µs case). The event counter does not count it.

Side finding: the bursts came every 10–20 µs instead of every 1 ms (AFG trigger interval). The readout saturated at about 85 MB/s and the board dropped bursts while its memory was full. This did not change the rule above but shows the readout limit for 5 µs records on one optical link: about 17 k events/s for one channel.
