# Native GX

`runtime/gx/host` defines the renderer-plugin ABI used by the standalone host.
`runtime/gx/aurora` implements it with AuroraGX.

Recognized SDK array/matrix operations can submit ordered typed host commands.
Compatible adjacent draws share state preparation, while attribute plans and
resource caches reduce repeated work. Custom write-gather/FIFO streams retain
their parser and guest-visible accounting. Memory resolvers, write publication,
EFB copies and pending-draw lifetimes keep resource reuse coherent.

See [Architecture](../../docs/ARCHITECTURE.md) and
[Optimizations](../../docs/OPTIMIZATIONS.md) for current boundaries and controls.
