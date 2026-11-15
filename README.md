# IntelSSTDMIC

Native **Internal Microphone** for Intel Smart Sound (cAVS + SOF) laptops on macOS, wired into
AppleHDA's own input engine.

The PDM microphones on these machines hang off the audio DSP, not the HDA codec, so the native
"Internal Microphone" records silence. IntelSSTDMIC boots the Intel production-signed SOF firmware
on the DSP, streams the DMIC array into a host ring, and feeds those samples to AppleHDA through a
Lilu route on `AppleHDAEngine::convertInputSamples`. The device, controls, permissions, timestamps
and the whole Core Audio path stay Apple's - only the sample source changes.

This project is the configuration-driven rewrite of
[qqaoshi888/TGLDmicEx](https://github.com/qqaoshi888/TGLDmicEx). Thanks to the original author for
the DSP bring-up work.

## Documentation

- [System architecture and developer guide (English)](docs/architecture.md)
- [系统架构与开发指南（中文）](docs/architecture_CN.md)

## Features

- **Layout-driven**: a layout id selects the DSP engine, firmware identity, microphone geometry,
  HDA stream identifiers and supported formats. At build time every entry of
  `Layouts/Layouts.plist` is compiled into the binary **and** copied into the kext `Info.plist`
  (`IntelSSTDMLayouts` / `IntelSSTDMLayoutIDs`). There is no runtime plist dependency.
- **Firmware embedded**: `kext/Firmware/TGL/sof-tgl.ri`, `CML/sof-cml.ri` and `ADL/sof-adl.ri`
  are compiled into the binary. An OpenCore-injected Lilu plugin has no readable bundle at runtime,
  so no external firmware path is used.
- **Two-layer engine model**:
  - `HdaAudioEngine` is the default native path; its methods call the original AppleHDA functions.
  - `IntelDspAudioEngine` derives from it, implements the common SOF boot / IPC / ring / sample path,
    and overrides only what is needed. Concrete `AdlDspAudioEngine` / `TglDspAudioEngine` /
    `CmlDspAudioEngine` add platform identity, core mask and firmware image.
- **Factory + Manager**: `IDspAudioEngineFactory` owns creation; configuration is one strategy.
  `DspAudioEngineManager` implements the hook interface and forwards every call to the created
  engine or to the native fallback. If creation fails, the factory creates a native
  `HdaAudioEngine`, logs the error and the system keeps using AppleHDA's normal logic.
- **Static hooks only**: all Lilu routes are static functions with AppleHDA's ABI; they pass
  AppleHDA's own object explicitly as `self`.
- **Only the internal microphone** is intercepted. Headset mic, line-in, HDMI and all output
  engines always take the native path.
- **Lock-free sample path**: `active` + `inFlight` gate fills without a mutex. Lifecycle hooks are
  synchronous; no periodic timer or descriptor scanning exists.

## Layout

```
kext/
  IntelSSTDMIC/              kext sources
  Firmware/TGL/sof-tgl.ri    Intel production-signed SOF image (Tiger Lake)
  Firmware/CML/sof-cml.ri    Intel production-signed SOF image (Comet Lake / Cannon Lake)
  Firmware/ADL/sof-adl.ri    Intel production-signed SOF image (Alder Lake)
  Makefile                   builds build/IntelSSTDMIC.kext (+ .zip)
Layouts/Layouts.plist        build-time layout database
```

### Structure

```
IntelSSTDMIC (IOService, IDspHost)
 |- LayoutRegistry                embedded + Layouts.plist
 |- DspAudioEngineFactory         creation seam; native fallback
 |- DspAudioEngineManager         IAudioEngineHooks proxy; owns active/native
 |- HookManager                   static trampolines + NativeHdaOps
 \- IntelDspAudioEngine           active engine
      |- HdaAudioEngine           HDA registers/caps + native AppleHDA defaults
      |- AdlDspAudioEngine        cAVS 2.5 (Alder Lake)
      |- TglDspAudioEngine        cAVS 2.5
      \- CmlDspAudioEngine        cAVS 1.8
```

## Requirements

- Intel Alder Lake (cAVS 2.5), Tiger Lake (cAVS 2.5) or Comet Lake / Cannon Lake / Coffee Lake
  (cAVS 1.8) with the Intel HDA controller at `00:1f.3`.
- [Lilu](https://github.com/acidanthera/Lilu) loaded before this kext.
- AppleHDA (or a driver that leaves the HDA controller to AppleHDA).
- OpenCore or another bootloader that can inject a kext and boot arguments.
- Xcode Command Line Tools, `MacKernelSDK` and Lilu's `Headers`/`Library` resources to build.

## Build

```sh
git clone https://github.com/acidanthera/MacKernelSDK ../MacKernelSDK
# Lilu.kext/Contents/Resources/Headers and .../Library come from a Lilu release
cd kext
make SDK=../MacKernelSDK LILU=../Lilu.kext/Contents/Resources
```

Result: `kext/build/IntelSSTDMIC.kext` and `kext/build/IntelSSTDMIC.kext.zip`. Both firmware
images are compiled into the executable.

## Install

1. Put `IntelSSTDMIC.kext` in `EFI/OC/Kexts/`.
2. Add the kext to `config.plist` -> `Kernel -> Add`, **after Lilu**:

   ```xml
   <dict>
     <key>Arch</key><string>x86_64</string>
     <key>BundlePath</key><string>IntelSSTDMIC.kext</string>
     <key>Comment</key><string>Intel SST DMIC</string>
     <key>Enabled</key><true/>
     <key>ExecutablePath</key><string>Contents/MacOS/IntelSSTDMIC</string>
     <key>MaxKernel</key><string></string>
     <key>MinKernel</key><string></string>
     <key>PlistPath</key><string>Contents/Info.plist</string>
   </dict>
   ```

   The early `IOResources` personality makes the kext load before Lilu closes plugin registration.
3. Boot with `-sstdmiclayout=1` (or `-sstdmiclayout=auto`). The layout argument is the only switch; without it the driver stays on the native AppleHDA path.

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `-sstdmicoff` | Disable the kext entirely. |
| `-sstdmicdbg` | Extra Lilu debug logging. |
| `-sstdmiclayout=auto\|0\|N` | Master switch. `auto`: detect profile/model/platform default; `0`: keep native HDA; `N>0`: select layout (failure logs and falls back to native). Absent: no DSP hooks, fully native. |
| `-sstdmicprofile=<name>` | Select a layout by name/id string (alias). |
| `-sstdmicmodel=<text>` | Select a layout whose name/id contains the text. |
| `-sstdmicfw=adl\|tgl\|cml` | Platform/firmware selector; also chooses the platform default layout. |
| `-sstdmicgain=<pct>` | Software gain override (0-400). |
| `-sstdmiccoremask=<n>` | Override the DSP core mask. |
| `-sstdmicgproc=0` | Do not set `PPCTL.GPROCEN` (the global DSP processing enable) while the DSP runs. Default is to set it when the firmware left it clear, and restore it afterwards. |
| `-sstdmicsnoop=0\|2` | Do not touch PCI DEVC NSNPEN / leave it set. |
| `-sstdmicpower=0\|1` | Never / always join the HDA driver's power tree. |
| `-sstdmicadl`, `-sstdmiccml`, `-sstdmictgl` | Force platform family on unlisted or spoofed PCI ids. |
| `-sstdmicforce` | Attach on an unlisted Intel HDA PCI id. |
| `-sstdmicdma=2` | Force physical DMA addresses (diagnostics). |
| `-sstdmicasync` | Dispatch lifecycle events on the Manager workloop instead of calling synchronously (fallback). |

## Layouts

`-sstdmiclayout` is the master switch and has three modes:

- `auto`: if `-sstdmicprofile`/`-sstdmicmodel` is given, use it; otherwise read the ACPI NHLT
  table and pick the detected platform's layout with the same PDM microphone count (falling back to
  the platform default when the NHLT is missing). If the argument is absent the driver does not install DSP hooks and
  stays fully native.
- `0`: disable DSP creation; the factory creates the native HDA engine directly.
- `N > 0`: create layout N. If the id is unknown or engine initialization fails, the error is
  logged, `CreateError` is published, and the factory creates the native HDA fallback.

Layout IDs start at 1; 0 is reserved for "disabled".

Built-in layouts (built from `Layouts/Layouts.plist`):

| id | platform | mic | engine | notes |
| --- | --- | --- | --- | --- |
| 1 | Tiger Lake | 2 | `tgl` | ThinkPad X13 Gen 2 stereo |
| 2 | Comet Lake | 4 | `cml` | ThinkPad X1 Carbon Gen 7 |
| 3 | Tiger Lake | 2 | `tgl` | mono mix |
| 4 | Alder Lake | 2 | `adl` | ADL-P/U/M/S DMIC |
| 5 | Comet Lake | 2 | `cml` | Lenovo XiaoXin Pro 13 2020 (2 DMIC, ALC257), see [docs/XIAOXIN-PRO13.md](docs/XIAOXIN-PRO13.md) |

Schema (`Layouts.plist`):

| Key | Meaning |
| --- | --- |
| `id` | Numeric layout id. |
| `name` | Human-readable name. |
| `platform` | `adl`, `tgl` or `cml`. |
| `engine` | Factory selector: `tgl`, `cml`, or a future engine class key. |
| `micCount` | Physical microphones (metadata / validation). |
| `codecs` | Optional ALC model list (metadata; runtime engine identity is discovered from AppleHDA). |
| `dsp.firmware` | Firmware identity (informational; the platform engine embeds its image). |
| `dsp.streamIndex`, `dsp.pcmTag`, `dsp.pipelineId` | DSP host stream and IPC identifiers. |
| `dsp.sourceRate`, `sourceChannels`, `sourceBits` | DSP host ring format; `sourceChannels` must be 2 or 4 (selects the captured SOF pipeline) and bits 32. |
| `dsp.gain` | Software gain percentage. |
| `hda.outputChannels`, `channelMap` | Core Audio channel mapping. |
| `hda.sampleRates`, `bits` | Supported HDA engine formats (the DSP ring stays at 48 kHz and is resampled). |
| `hda.safetyFrames` | Expected `IOAudioEngineInputSampleOffset`. |
| `keepAliveSec`, `allowD0i3` | Reserved power policy fields. |

## Hooked AppleHDA functions

| Symbol | Purpose |
| --- | --- |
| `AppleHDAEngine::convertInputSamples` | Fill the internal mic's Core Audio buffer from the DSP ring; other streams use the original converter. |
| `AppleHDAEngineInput::initHardware` | Native init first, then identify the engine, cache its `IOAudioStream`/tag, initialize the DSP and load firmware. |
| `AppleHDAEngine::performAudioEngineStart` | Native start first, then start DSP recording (descriptor lookup + SOF pipeline start). |
| `AppleHDAEngineInput::performAudioEngineStop` | Native stop first, then stop DSP recording. |
| `AppleHDAEngineInput::stopHardware` | Native teardown, then stop the session, power the DSP down and release cached streams. |
| `AppleHDAEngineInput::performFormatChange` | Native call, then refresh HDA geometry/resampler step. |

All hooks are static trampolines. Missing lifecycle symbols only disable that event; the data hook
still installs.

## Diagnostics

Published on the `IntelSSTDMIC` ioreg node:

- `LayoutId`, `LayoutName`, `EngineClass`, `CreateError`, `DspActive`, `LayoutDisabled`, `Ready`,
  `DspAlive`, `Streaming`, `SetupDone`, `LastError`
- `ImicTag`, `ImicStreams`
- `HookFills`, `HookFrames`
- `HookRouteOk`, `HookRouteEngine` (bit0 init, bit1 performStart, bit2 performStop,
  bit3 stopHardware, bit4 performFormatChange)
- `HookKmodEntry`, `HookReqAccess`, `HookShouldLoad`, `HookPluginStart`, `HookArg`,
  `HookOnKextErr`, `HookKextCb`
- `Log` (last 128 lines)

Healthy recording state: `EngineClass` = `TglDspAudioEngine`, `DspActive=true`, `Streaming=true`,
`HookRouteOk=1`, `HookFills`/`HookFrames` rising.

If layout creation fails: `EngineClass=native`, `DspActive=false`, `CreateError` explains the
reason, and AppleHDA continues normally.

## Power and session behaviour

Lifecycle hooks are synchronous; there is no timer or periodic descriptor scan. The sample path is
lock-free (`active` + `inFlight`). Firmware boot is single-flight and never resets a core that is
serving an active session. On system sleep `setPowerState(0)` stops the session and powers the DSP
core down; wake only clears the power-lost state and the next `performAudioEngineStart` reloads the
firmware. Device-level power transitions while the system is awake never trigger DSP init or a
session restart. While recording the kext is a PM child of AppleHDA and holds it on.

## Licence

The DSP bring-up, loader and IPC sequences come from the original TGLDmicEx project. The firmware
images compiled in from `kext/Firmware` are Intel production-signed SOF releases from
[thesofproject/sof-bin](https://github.com/thesofproject/sof-bin) (BSD-3-Clause).
