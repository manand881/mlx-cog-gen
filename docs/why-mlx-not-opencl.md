# Why MLX and not OpenCL

## OpenCL is deprecated on macOS

Apple deprecated OpenCL in macOS 10.14 (Mojave, 2018) and has made no
investment in the driver since. The implementation is frozen at OpenCL 1.2,
ships no new GPU features, and could be removed in a future OS release. Building
on a deprecated API means accumulating technical debt against a shrinking
maintenance window.

## OpenCL does not understand unified memory

On Apple Silicon the CPU and GPU share a single physical memory pool. The key
consequence: a buffer allocated on the CPU is already visible to the GPU at the
same address — no copy required.

OpenCL was designed for discrete GPUs with separate VRAM. Its programming model
requires explicit host↔device transfers (`clEnqueueWriteBuffer` /
`clEnqueueReadBuffer`) even when there is nothing to transfer. On Apple Silicon
those calls are a no-op under the hood, but you still pay the driver overhead
and write the boilerplate.

MLX was designed from scratch for Apple Silicon's unified memory model. An
`mx::array` lives in shared memory by default. Passing GDAL's in-memory raster
tiles to the GPU and reading results back requires zero copies and zero explicit
transfer calls.

## MLX sits on Metal; OpenCL does not

Apple's current GPU stack is Metal. MLX compiles its operations to Metal
shaders, gets hardware scheduling, and benefits from every Metal driver update.

Apple's OpenCL implementation is a compatibility layer over an older internal
path. It does not use the same compilation and dispatch infrastructure as Metal,
so it does not benefit from Metal driver improvements. On the same hardware,
Metal-backed compute consistently outperforms OpenCL in Apple's own benchmarks.

## MLX has lazy evaluation and operation fusion

MLX builds a computation graph and dispatches it lazily. Operations that would
be separate kernel launches in OpenCL — pad, multiply, sum, divide — can be
fused into a single pass over memory. Reduced kernel launches and fewer memory
round-trips are especially valuable at the large tile sizes used in GeoTIFF
overview generation.

OpenCL has no built-in graph or fusion mechanism. Each `clEnqueueNDRangeKernel`
call is a separate dispatch.

## Developer ergonomics

OpenCL requires verbose C API calls: platform enumeration, device selection,
context and command queue creation, explicit kernel compilation from source
strings, and manual argument binding. Implementing a 2×2 average downsample
with nodata masking in OpenCL takes several hundred lines of C before any
compute logic is written.

MLX exposes NumPy-style operations in C++. The same downsample kernel is
~20 lines of array operations (`mx::pad`, `mx::reshape`, `mx::mean`). The GPU
dispatch, memory management, and synchronisation are handled by the framework.

## Summary

| | OpenCL | MLX |
|---|---|---|
| macOS status | Deprecated (2018) | Actively developed |
| Unified memory | Explicit transfers | Zero-copy by design |
| GPU backend | Legacy path | Metal |
| Operation fusion | Manual | Built-in |
| API verbosity | High | Low |

For a Mac-only tool targeting Apple Silicon, MLX is the appropriate choice on
every axis.
