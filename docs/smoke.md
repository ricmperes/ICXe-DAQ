# Hardware smoke (2026-10-26)

Smoke succeeds. The front-panel/internal clock-master switch was changed from the position that left the ADC clock unlocked to the position that lets the ADC clock lock. In the unlocked position, register `0x8104` stayed `0` (board ready never set), `CAEN_DGTZ_Reset` timed out, `CAEN_DGTZ_SWStartAcquisition` returned `CAEN_DGTZ_CommError`, and `0x812C` stayed `0` even though `0x8100` bit 2 latched. After a power cycle with the switch changed, acquisition works.

`data/v1730sb_20261026T143011Z/` completed (`stop_reason` `max_events`, `ended_utc` `2026-10-26T14:30:12Z`). It wrote 10 events at record length 1020 (`wf_00000.bin`). `tools/read_waveforms.py` on that file reports array shape `(10, 3, 1020)` (events, channels, samples), mask `0x7`, channel axis `[0, 1, 2]`.

The running image is **standard waveform**, not DPP. The factory sticker still says DPP-ALL. One open (link 0, node 0, base 0) reported model V1730SB, serial 30176, family 11, ROC `04.29 - Build 8716`, AMC `00.05 - Build 9130`, and `CAEN_DGTZ_GetDPPFirmwareType` enum `-1` (`CAEN_DGTZ_NotDPPFirmware`).

## info

`./build/icxe-daq info --config config/default.json` had already exited 0 on an earlier open:

```text
model: V1730SB
model_code: 32
serial: 30176
channels: 16
adc_bits: 14
family_code: 11
form_factor: 0
roc_firmware: 04.29 - Build 8716
amc_firmware: 00.05 - Build 9130
dpp_firmware: standard waveform
board_info_0x8140: 0x10080b
memory_size_field: 8
link_type: optical
link_num: 0
conet_node: 0
vme_base_address: 0x32140000
use_vme_base_address: false
open_base: 0x0
```

`0x8140` is `0x10080b`. Bits `[7:0]` are family code 11. Bits `[15:8]` are memory-size field 8 (5.12 MS/channel in the library's buffer math: `field * 640 * 1024`). Bits `[23:16]` are 16 channels.

## Unlocked clock (earlier opens)

A rate-trigger config (`build/smoke-rate.json`, record length 1020, `baseline.apply_on_start` true, `--max-events 4`, output under `build/smoke-out`) was used so the check did not depend on an external NIM pulse. `config/default.json` was not modified.

With the clock unlocked, `CAEN_DGTZ_Reset` returned `CAEN_DGTZ_Timeout` (`-18`). The library writes `0xEF24` and waits about 1.6 s for bit 8 of `0x8104` (board ready: PLL and ADCs synchronized). On those opens that register stayed `0x00000000`.

`CAEN_DGTZ_SetRecordLength` stored `0x800C = 0xA` (k=10, 1024 buffers, 5110 samples on the 5.12 MS map). Post-trigger readback was 80%. `GetRecordLength` matched 1020, so the custom size fits the buffer.

`CAEN_DGTZ_SWStartAcquisition` writes `0x8100` bit 2 and waits about 0.5 s for bit 2 of `0x8104`. Status stayed `0x00000000`, so the call returned `CAEN_DGTZ_CommError` (`-1`). The control register read back `0x8100 = 0x00000004`, so the run bit did latch.

That mismatch is not treated as fatal until events are checked. With software trigger already enabled, four `CAEN_DGTZ_SendSWtrigger` calls were issued, then `0x812C` was read once. It was `0x00000000`. The same six registers on that path:

| Register | Value | Meaning |
| --- | --- | --- |
| `0x8100` | `0x00000004` | bits `[1:0]` are software-controlled; bit 2 (run) is set |
| `0x8104` | `0x00000000` | bit 2 (RUN status) and bit 8 (board ready) are clear |
| `0x812C` | `0x00000000` | no events stored |
| `0x810C` | `0x80000000` | bit 31, software trigger, is set |
| `0x8120` | `0x00000007` | channels 0, 1, and 2 are enabled |
| `0x800C` | `0x0000000A` | k=10; a 1020-sample record fits 5110 samples |

Trigger source, channel enable, acquisition-mode bits, and buffer-versus-record length were already programmed. The acquisition state machine was not running: `0x8104` never left 0, so a latched `0x8100` bit 2 did not store events. Those runs stopped and the digitizer was closed. Baseline never measured a waveform, and the run start was not reached.

A second open wrote the software clock sync at `0x813C` (the strobe the library uses on other families before it polls `0x8104` bit 8) and waited 2.0 s. `0x8104` stayed `0x00000000`, and `0x812C` stayed 0 after the same four software triggers. That write is not part of the start path.

`CAEN_DGTZ_ReadData(CAEN_DGTZ_POLLING_MBLT)` takes libCAENDigitizer's handle mutex and then calls its internal register read with the CAENComm handle as a digitizer index. That read locks the same mutex again when the two handles are the same integer. The mutex is the library's. Readout now calls `CAEN_DGTZ_ReadData` with `CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT`, which holds the mutex once and transfers the output buffer. No event was stored on those opens, so that call was not exercised on a waveform then.
