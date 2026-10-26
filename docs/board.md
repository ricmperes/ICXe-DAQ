# V1730SB on the direct A5818 link

The digitizer is a CAEN **WV1730SBXAAA** (V1730SB). The crate only supplies power. Waveforms leave the board through the front-panel optical port. The factory front-panel sticker reads **DPP-ALL**. This program targets the standard waveform firmware. DPP manuals do not apply.

A single open on 2026-10-26 (optical link 0, node 0, base 0) called only `CAEN_DGTZ_GetInfo` and `CAEN_DGTZ_GetDPPFirmwareType`, then stopped and closed. `GetInfo` already returns the ROC and AMC revision strings, so no firmware-revision register was read again.

| Item | Reported value |
| --- | --- |
| Sticker | DPP-ALL |
| Model | V1730SB (model code 32) |
| Serial | 30176 |
| Family | 11 |
| ROC | `04.29 - Build 8716` |
| AMC | `00.05 - Build 9130` |
| DPP enum | `-1` (`CAEN_DGTZ_NotDPPFirmware`) |

`-1` is standard waveform. The sticker does not mean a DPP image is running.

| Item | Value |
| --- | --- |
| Channels | 16 |
| ADC | 14-bit |
| Sampling | 500 MS/s |
| Memory | 5.12 MS/channel SRAM |
| Input range | 2 Vpp or 0.5 Vpp |

Up to eight digitizers can be daisy-chained on the A5818 later. This board is CONET node 0 on optical link 0.

## Clock-master switch

The front-panel/internal clock-master switch must be in the position that lets the ADC clock lock. It was changed from the position that left the clock unlocked, and the board was power-cycled. In the unlocked position, register `0x8104` stayed `0` (board ready never set), `CAEN_DGTZ_Reset` timed out, `CAEN_DGTZ_SWStartAcquisition` returned `CAEN_DGTZ_CommError`, and `0x812C` stayed `0` even though `0x8100` bit 2 latched. After that power cycle, acquisition works. The factory sticker still says DPP-ALL. The running image is standard waveform (ROC `04.29`, AMC `00.05`).

## Address and open call

The rotary switches read **3214**, which is the VME base address **`0x32140000`**. That value is stored for a later V4718. A direct CONET open passes base address **0**. Passing `0x32140000` on this link makes the open fail or address the wrong target.

```c
CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_OpticalLink, /* LinkNum */ 0,
                        /* ConetNode */ 0, /* VMEBaseAddress */ 0, &handle);
```

`CAEN_DGTZ_OpticalLink` is `1` in `/usr/include/CAENDigitizerType.h`. The installed `CAENDigitizer.h` (2.18.0) marks `CAEN_DGTZ_OpenDigitizer` deprecated in favor of `CAEN_DGTZ_OpenDigitizer2`. The arguments above are still the direct-link open: optical link, link 0, node 0, base 0.

## This PC

`lspci -nn -d 202c:5818`:

```text
d1:00.0 Communication controller [0780]: CAEN S.p.A. A5818 [202c:5818]
```

The card is in physical slot 6. The subsystem is Xilinx `10ee:0007`. With `a5818` unloaded the BARs are disabled (`Mem-`, `BusMaster-`) and `lspci` reports `Kernel modules: a5818`. Driver load is covered in [drivers.md](drivers.md).

CAEN user libraries are already installed. `ldconfig -p` resolves these sonames (CAEN versioned filenames use a `.v` infix):

| Library | ldconfig soname | Path |
| --- | --- | --- |
| CAENVME 4.0.2 | `libCAENVME.so.v4.0.2` | `/usr/lib/libCAENVME.so.v4.0.2` |
| CAENComm 1.7.0 | `libCAENComm.so.v1.7.0` | `/usr/lib/libCAENComm.so.v1.7.0` |
| CAENDigitizer 2.18.0 | `libCAENDigitizer.so.v2.18.0` | `/usr/lib/libCAENDigitizer.so.v2.18.0` |

Unversioned links `libCAENVME.so`, `libCAENComm.so`, and `libCAENDigitizer.so` point at those files. `libCAENDigitizer.so.v2.17.3` is also on disk; the unversioned digitizer link points at 2.18.0.

Headers in `/usr/include`: `CAENDigitizer.h`, `CAENDigitizerType.h`, `CAENComm.h`, `CAENVMElib.h`, `CAENVMEoslib.h`, `CAENVMEtypes.h`.
