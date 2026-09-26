# GekkoAOT standalone DSP / AID

This directory owns the standalone GameCube DSP path used by the native host.
It does **not** link Dolphin Core, DSP-HLE, AudioInterface, Mixer or `Core::System`.

## v45 native LLE path

When the pinned ModernGekko/Dolphin source cache is materialized, GekkoAOT
compiles the GPL-2.0-or-later DSP interpreter *instruction semantic translation
units* directly against GekkoAOT-owned state, memory, mailbox, DMA, accelerator
and interrupt shims. The final native host contains no Dolphin `Core::System`
and does not fall back to Dolphin at runtime.

The DSP clock is driven at 1/6 of the 486 MHz Gekko clock. Retail ucode loaded by
the Nintendo SDK F3 task protocol is copied from MEM1 into native IRAM and then
executed by the standalone DSP core.

### Internal ROMs

Nintendo's DSP IROM and coefficient ROM are not distributed by GekkoAOT. A real
LLE task may call those ROM routines, so provide legally obtained dumps:

```bash
export GEKKOAOT_DSP_IROM=/path/to/dsp_rom.bin   # exactly 8192 bytes
export GEKKOAOT_DSP_COEF=/path/to/dsp_coef.bin  # exactly 4096 bytes
```

If unset, GekkoAOT also checks the usual Linux Dolphin user dump locations:
`~/.local/share/dolphin-emu/GC/dsp_rom.bin` and `dsp_coef.bin`.

If either ROM is unavailable, the LLE task executor stays disabled and the
existing structural SDK-bootstrap compatibility path remains active. No
proprietary ROM bytes are embedded in the project.

## Native AID audio

The hardware-visible `AUDIO_DMA_*` registers now feed a standalone audio-DMA
engine. It consumes one 32-byte block (8 stereo signed-16 frames) at the AID
sample rate (32.028 or 48.043 kHz), performs the GameCube big-endian conversion,
reloads the programmed DMA buffer, and raises the AID interrupt at the hardware
boundary. When SDL3 is available the PCM goes directly to an SDL3 audio stream.

AID timing is advanced from guest CPU cycles, not from VI/presentation. Thus a
high host presentation rate or v44 virtual-retrace acceleration does not alter
audio pitch.

At shutdown the native runner reports `dsp_native`, `dsp_rom`, `dsp_pc`,
`dsp_op`, `dsp_insns`, and `dsp_audio_frames` for bring-up diagnostics.
