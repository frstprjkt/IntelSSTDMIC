# Lenovo XiaoXin Pro 13 2020 (Comet Lake, 2 DMIC)

Layout 5. Verified on one machine, see "Verification" below.

| Item | Value |
| --- | --- |
| Machine | Lenovo XiaoXin Pro 13 2020 (82DN), i7-10710U |
| Audio controller | Intel Comet Lake PCH cAVS 1.8, PCI `8086:02c8`, subsystem `17aa:380f` |
| Codec | Realtek ALC257, AppleALC layout-id 11 |
| Microphones | 2 PDM DMIC on one PDM controller (ACPI NHLT: 2 mics, array type 0x0a) |
| macOS | 15.8.2 (OpenCore) |

## What differs from the 4-mic Comet Lake layout (2)

1. **`PPCTL.GPROCEN` is clear at boot.** This firmware leaves the global DSP processing enable
   (PPCTL bit 30) cleared. Without it the DSP ignores `ADSPCS.SPA` and the boot fails at
   `core power up`. The kext now sets it while the DSP runs and restores it afterwards
   (`-sstdmicgproc=0` turns this off).
2. **2 ch SOF pipeline.** The 4 ch IPC table does not fit one PDM controller. `IpcReplay2ch.hpp`
   was captured from Linux SOF on this machine. The layout's
   `sourceChannels` (2 or 4) selects the table and the HDA capture format.
3. **Cores 1..3 are powered down** after the ROM IPC ack, as Linux `cl_dsp_init` does.
4. **Layout auto-selection uses the NHLT.** `-sstdmiclayout=auto` counts the PDM mics in the ACPI
   NHLT table and chooses the Comet Lake layout with that many mics (2 -> layout 5, 4 -> layout 2).

## Install

`EFI/OC/Kexts/IntelSSTDMIC.kext` after Lilu, and boot with:

```
-sstdmiclayout=auto
```

(`-sstdmiclayout=5` forces this layout.)

## Verification

Checked in ioreg (`ioreg -r -c IntelSSTDMIC -l`):

- `Log`: `NHLT: ... 2 PDM mic(s)`, `layout 5 auto-selected`, `firmware running (FW_READY)`,
  `cores 0xe powered down`, `streaming into descriptor 0 (tag 1), 8 B/frame`.
- `DspAlive = Yes`, `HookFillFail = 0`, `FillFailMask = 0`, `HookEngineError = 0`.
- Internal microphone records after a cold boot and after sleep/wake.

Not checked: other XiaoXin/IdeaPad Comet Lake machines, 4 ch recording on this layout.

## Known issue

On the first sleep/wake after boot the log can show `FAIL ROM init (0x00000005)` followed by one
failed `stop`/`teardown` IPC, because the restore path restarts a DSP whose firmware is still
running. The session recovers on its own, later wakes were clean and recording was not affected.

## Headphone crackling after sleep

Unrelated to this kext. If the headphone output is quiet or crackles after wake, AppleALC layout 11's
`WakeConfigData` is missing pin 0x19. A patch for that is in the TGLDmicEx project
(`tools/applealc/patch-wake.py`).
