# Learnings

- GDAL's internal source files (`gdalrasterband.cpp`, `gdaldefaultoverviews.cpp`) are not designed to be compiled standalone — they are part of `libgdal` and depend on hundreds of other internal files
- Copying individual GDAL source files into a project and compiling them against the installed `libgdal` causes duplicate symbol conflicts
- The GDAL sparse clone must be checked out at the exact same version tag as the installed Homebrew GDAL (e.g. `v3.12.2`) — mismatches between source and headers cause API errors (e.g. `GDT_UInt8` vs `GDT_Int8`)
- Private GDAL headers (e.g. `gdal_utils_priv.h`, `gdalargumentparser.h`) are not shipped with the Homebrew install — they only exist in the source repo
- The correct way to extend GDAL behaviour is through its public C++ API, not by re-compiling its internals
- COG overview/pyramid generation in GDAL happens via `GDALBuildOverviews()` — this is the public API entry point we should hook into
- A baseline COG from `gdal_translate` on `sample_dem.tif` produces 4 overview levels (2386x2562 → 1193x1281 → 596x640 → 298x320) using cubic resampling
- `gdal-src` sparse clone in `scratch/` should be pinned to `v3.12.2` to match the Homebrew install
- MLX 0.31.0 is available via `brew install mlx` — provides C++ API for GPU-accelerated array ops on Apple Silicon
- The correct architecture: write `mlx_translate.cpp` using GDAL's public API for I/O, and our own MLX code for overview generation — no GDAL source files needed in the repo
- MLX 0.31.0 API quirks: use `mx::default_stream(device)` not `mx::Stream(device)` to get a stream for a device; `mx::mean()` requires `std::vector<int>` not an initializer list for axes; `mx::slice()` takes `Shape` (`SmallVector<int>`) — use initializer lists `{start, ...}` not `std::vector<int>`
- GDAL computes overview sizes as `ceil(N/2)` — so `targetH * 2` can exceed the source height by 1 for odd dimensions. Edge replication (replicate last row/col before reshape+mean) is the correct handling and matches GDAL's AVERAGE resampling behaviour at boundaries
- `scratch/` is fully gitignored — `sample_dem.tif`, `sample_dem_cog.tif`, and `gdal-src` sparse clone are local only and must be recreated on a fresh clone
- `build/` is gitignored — run `mkdir build && cd build && cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew && make` to recreate
- Baseline COG (`sample_dem_cog.tif`) was generated with: `gdal_translate scratch/sample_dem.tif scratch/sample_dem_cog.tif -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO`
- The `gdal-src` sparse clone is at `v3.12.2` tag in `scratch/gdal-src` — useful for reference but not part of the build
- **Always clone GDAL at the exact Homebrew-installed version** — check version first with `gdal-config --version`, then clone directly at that tag: `git clone --filter=blob:none --sparse --branch v<version> https://github.com/OSGeo/gdal gdal-src`. Never clone main/latest and switch after — the default sparse clone tracks main and causes API mismatches

## gdal_translate -of COG Pipeline

1. **Open source file** — reads input `.tif` into a `GDALDataset`
2. **Create output dataset** — creates a GTiff with `LAYOUT=COG` creation option
3. **Copy raster data** — copies pixel data band by band into the output
4. **Build overviews** — calls `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
5. **`GDALRegenerateOverviewsEx()`** — the actual resampling step per band, produces each overview level
6. **Write COG structure** — tiles data and writes final file with overviews embedded

**Our replacement point is `GDALRegenerateOverviewsEx()`** — instead of calling it, we read band data into an MLX array, downsample iteratively per overview level on GPU, and write results back via GDAL's public API. Everything else stays as GDAL.

## Planned Code Structure

```
src/
  mlx_translate.cpp   — main program, CLI + GDAL I/O
  mlx_overviews.cpp   — MLX-accelerated overview/pyramid generation
  mlx_overviews.h     — header
tests/
  test_mlx.cpp        — verifies MLX install, GPU access, array ops, downsample
CMakeLists.txt
tests/CMakeLists.txt
```

## Next Steps

1. Write `src/mlx_overviews.h` and `src/mlx_overviews.cpp` — MLX implementation of overview generation
2. Write `src/mlx_translate.cpp` — opens input GeoTIFF, writes COG, calls our MLX overview step
3. Wire up in `CMakeLists.txt`
4. Benchmark against baseline `gdal_translate` on `sample_dem.tif`
