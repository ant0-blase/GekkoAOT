# Native GX

`runtime/gx/host` defines the stable renderer-plugin ABI used by the standalone host.

`runtime/gx/aurora` implements that ABI with AuroraGX. Guest write-gather-pipe stores are accumulated until a complete retail GX command is available, then forwarded to Aurora's FIFO command processor. Array bases, texture Image3 pointers and TLUT source addresses are resolved through the host's MEM1/MEM2 resolver before rendering.
