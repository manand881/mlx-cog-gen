# mlx-cog-gen

MLX-powered Cloud Optimized GeoTIFF generator for Apple Silicon.

## About

This project takes GDAL's COG generation pipeline and replaces the pyramid/overview generation step with an MLX-accelerated implementation, making use of Apple Silicon hardware rather than letting it idle when you already have it on your machine.

**GDAL is a required system dependency.** This project links against the installed GDAL library and does not bundle it.

## Future Scope

- **Resampling acceleration** — offload bilinear, cubic, and lanczos resampling algorithms to Apple Silicon via MLX, since these are matrix operations that map naturally to GPU execution

## Usage

```bash
mlx_translate input.tif output_cog.tif -of COG -co COMPRESS=LZW
```

Just replace `gdal_translate` with `mlx_translate`.

## Installation

not yet

## Development

```bash
brew install gdal  # Required system dependency
brew install mlx   # Apple Silicon ML framework for overview acceleration
brew install cmake # Build system
```

## Building & Testing

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew
make
ctest --output-on-failure
```

## Contributing

I'm not yet sure of what this does so dont contribute yet.

## License

[MIT](https://choosealicense.com/licenses/mit/)
