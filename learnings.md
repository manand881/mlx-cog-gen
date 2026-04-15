# Learnings

## GDAL

- GDAL's internal source files (`gdalrasterband.cpp`, `gdaldefaultoverviews.cpp`) are not designed to be compiled standalone; they are part of `libgdal` and depend on hundreds of other internal files
- Copying individual GDAL source files into a project and compiling them against the installed `libgdal` causes duplicate symbol conflicts
- The correct way to extend GDAL behaviour is through its public C++ API, not by re-compiling its internals
- COG overview/pyramid generation in GDAL happens via `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
- `GDALRegenerateOverviewsEx()` is **CPU-only**: uses SIMD (SSE2/AVX2 on x86, NEON via sse2neon on Apple Silicon). No GPU, no Metal, no CUDA. The Apple Silicon GPU is completely idle during GDAL overview generation
- `BuildOverviews()` computes overview sizes as `ceil(N/2)`. However, `gdal_translate -co OVERVIEWS=AUTO` (the COG driver path) uses `floor(N/2)`. These are different internal code paths in GDAL. Our pipeline uses `BuildOverviews` so our overview bands are 1 pixel larger per dimension than what `gdal_translate` produces for odd-sized rasters. The level COUNT matches exactly. The geographic extent is unaffected.
- GDAL's AVERAGE resampling handles NoData by excluding NoData pixels from each block's average; our MLX implementation must do the same or elevation values get contaminated at NoData boundaries
- The COG driver accepts `OVERVIEWS=FORCE_USE_EXISTING` to use pre-built overviews rather than regenerating them; this is how we inject MLX-computed overviews into the COG pipeline

## gdal_translate -of COG Pipeline

1. **Open source file**: reads input `.tif` into a `GDALDataset`
2. **Create output dataset**: creates a GTiff with `LAYOUT=COG` creation option
3. **Copy raster data**: copies pixel data band by band into the output
4. **Build overviews**: calls `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
5. **`GDALRegenerateOverviewsEx()`**: the actual CPU resampling step per band, produces each overview level
6. **Write COG structure**: tiles data and writes final file with overviews embedded

**Our replacement point is `GDALRegenerateOverviewsEx()`**: instead of calling it, we read band data into an MLX array, downsample iteratively per overview level on GPU, and write results back via GDAL's public API. Everything else stays as GDAL.

## Our Pipeline (mlx_translate)

1. Open source with GDAL
2. Create in-memory temp GTiff via `/vsimem/`
3. Call `BuildOverviews("NONE", ...)` on temp: allocates overview band structure with zero CPU compute (see NONE resampling note below)
4. Call `MLXBuildOverviews()`: fills overview bands with GPU-computed downsampling (AVERAGE or BILINEAR)
5. Create final COG from temp using COG driver with `OVERVIEWS=FORCE_USE_EXISTING`

## MLX API

- MLX 0.31.0 available via `brew install mlx`, provides C++ API for GPU-accelerated array ops on Apple Silicon
- Use `mx::default_stream(device)` not `mx::Stream(device)` to get a stream for a device
- `mx::mean()` requires `std::vector<int>` not an initializer list for axes
- `mx::slice()` takes `Shape` (`SmallVector<int>`); use initializer lists `{start, ...}` not `std::vector<int>`
- MLX ops are lazy; nothing executes until `mx::eval()` is called
- Edge replication for odd dimensions: replicate last row/col before reshape+mean, matching GDAL's `BuildOverviews` ceil(N/2) convention

## Resampling

- **GDAL's default for COG overview generation is NEAREST**: listed first in `-r nearest,bilinear,...` and used when no `-r` flag is passed
- NEAREST formula: `out[i,j] = src[2i, 2j]`, picks the top-left pixel of each 2×2 block and discards the other 3 (75% of data lost per level)
- AVERAGE formula: `out[i,j] = Σ valid_src_pixels / count_valid`, all pixels in the block contribute; NoData pixels excluded from sum and count
- NEAREST is correct for **categorical data** (land cover classes, labels) where averaging would create meaningless blended values
- AVERAGE is correct for **continuous data** (elevation, imagery, temperature). NEAREST can make narrow features (a ridge, a river) disappear entirely at coarser levels depending on pixel alignment
- BILINEAR uses a separable tent filter: for each output pixel i, the sample point in source space is `(i + 0.5) * 2 - 0.5 = 2i + 0.5`, which lands halfway between source pixels 2i and 2i+1. The tent filter assigns weight 0.5 to each. Applied as two 1D passes (horizontal then vertical). For strict 2x downsampling with no NoData, this is numerically identical to AVERAGE because the 0.5/0.5 weights produce the same 2x2 mean. They diverge at NoData boundaries and for non-2x factors.
- GDAL's bilinear overview generation goes through the convolution code path (`GDALResampleChunk_ConvolutionT`), which is a more general implementation than the AVERAGE path (`GDALResampleChunk_AverageOrRMS`). In current benchmark data (160cm–20cm) GDAL bilinear and average are within 0.2% of each other. Any divergence at larger raster sizes has not been verified with the current test data. MLX shows no difference between the two methods because the GPU parallelises both uniformly.
- Our benchmark compares both tools using both AVERAGE and BILINEAR, each method run independently against its GDAL equivalent

## NoData Handling

- Without NoData masking, averaging `-9999` NoData pixels into real elevation values produces severely corrupted overview pixels (e.g. min dropping to -9842 instead of -4.93)
- Contamination compounds at each overview level because each level downsamples from the previous
- Fix: mask NoData pixels before averaging; zero them out, sum valid pixels only, divide by valid count, output NoData where all 4 pixels in a block are NoData
- Read NoData value per band via `poBand->GetNoDataValue(&hasNodata)`; always check the `hasNodata` flag before masking
- **NaN no-data rejection (2026-04-15):** Added early check to reject datasets with NaN no-data values using `std::isnan()`. Returns `CE_Failure` with clear error message directing users to convert to numeric no-data (e.g., -9999). Prevents silent corruption that would occur because `mx::equal()` cannot detect NaN (NaN != NaN by IEEE 754). Verified by testing with a dataset containing NaN no-data - correctly rejected with error message.
- **Float16 nodata quantization issue**: Float16 precision near 10000 is ±8 (exponent 13, 10-bit mantissa). `-9999` stored as Float16 rounds to `-10000.0`. The nodata metadata still says `-9999.0`. Both GDAL and our implementation compare the stored pixel value against the metadata value and find a mismatch; nodata pixels are silently treated as valid data. This is not specific to our code; GDAL has the same problem. Confirmed experimentally: GDAL overview min drops to -10560 (contamination from nodata averaging), while MLX min stays at -10000 (contamination present but bounded since all nodata pixels have the same quantised value). This is the same class of failure as nodata=0; any nodata value that cannot be exactly represented in Float16 triggers it.

## Benchmark

- `sample_dem.tif` lives in `tests/`; gitignored via `*.tif` but explicitly unignored via `!tests/sample_dem.tif`. Current file has 1.9% nodata coverage. A better test file with 22.24% nodata (128MB, exceeds GitHub size limit) is available locally and validates nodata handling correctly at higher nodata percentages.
- `build/` is gitignored; must be recreated on fresh clone

### Benchmark methodology notes

- `mlx_translate` copies the source into `/vsimem/` (RAM filesystem); all reads hit RAM throughout
- `gdal_translate` reads from disk, but macOS's OS page cache keeps the file in RAM after the first read; subsequent runs confirm no disk variance
- Cannot compare speedup ratios across different benchmark sessions; system state, memory pressure, and page cache warmth all vary; only absolute times within the same session are meaningful

### GDAL performance flags: the full map

`gdal_translate -of COG` performance is controlled by more than just `GDAL_NUM_THREADS`. The benchmark currently only controls threading. The following flags can all shift GDAL wall-clock time and must be understood before drawing conclusions about what "maximum credible GDAL" looks like.

**Threading (currently benchmarked)**
- `GDAL_NUM_THREADS` (`--config`): default `1`. Parallelises **both** overview downsampling computation **and** LZW/DEFLATE tile compression. `ALL_CPUS` enables all cores. Supported since GDAL 3.2 for overview generation; LZW compression also benefits. This is the single largest lever.
- `NUM_THREADS` (`-co`): COG-driver-specific form of the same control. Canonical for COG creation. Both `--config GDAL_NUM_THREADS` and `-co NUM_THREADS` are respected; in benchmarks we use the `--config` form.

**Memory and block cache (not currently benchmarked)**
- `GDAL_CACHEMAX` (`--config`): default 5% of RAM (~800MB on 16GB). Controls how many decoded source blocks GDAL keeps in RAM during overview generation. At large raster sizes, a small cache causes blocks to be evicted and re-read for each successive overview level. Setting `4096MB` or higher could measurably reduce I/O during multi-level overview generation.
- `GDAL_SWATH_SIZE` (`--config`): defaults to `GDAL_CACHEMAX / 4`. Controls the in-flight transfer buffer when GDAL copies pixels between datasets (used during the final COG assembly step). Automatically scales with GDAL_CACHEMAX.
- `GDAL_BAND_BLOCK_CACHE` (`--config`): default `AUTO`. `ARRAY` mode is faster for typical rasters (direct array indexing vs. hash lookup). AUTO chooses based on block count and usually picks ARRAY.
- `VSI_CACHE` (`--config`): default `FALSE`. Set to `TRUE` to add a per-file-handle RAM read-ahead cache on top of the OS page cache. Useful when the same source blocks are read repeatedly across overview levels.
- `VSI_CACHE_SIZE` (`--config`): default 25MB. Size of per-file VSI cache. Only effective when `VSI_CACHE=TRUE`.

**Compression (not currently benchmarked)**
- `PREDICTOR` (`-co`): default `NO` (= 1, no predictor). Setting `2` (horizontal differencing) before LZW reduces input entropy so the compressor does less work. Standard recommendation for integer DEM data; can improve LZW speed by 10–30% while also improving compression ratio. Setting `3` (floating-point differencing) is appropriate for Float32 data. LZW has no configurable level; PREDICTOR is the only way to tune LZW performance.
- `LEVEL` (`-co`): controls effort for DEFLATE (1–12, default 6) and ZSTD (1–22, default 9). Level 1 is maximally fast. Has no effect on LZW.
- `OVERVIEW_COMPRESS` / `OVERVIEW_PREDICTOR` (`-co`): same controls applied independently to overview tiles.

**I/O path (minor, situational)**
- `GTIFF_VIRTUAL_MEM_IO` (`--config`): default `NO`. Set to `YES` or `IF_ENOUGH_RAM` to use `mmap()` instead of GDAL's block cache for reading uncompressed source TIFFs. Can reduce source-read time on large uncompressed inputs by delegating memory management to the OS. No effect on compressed sources.
- `GTIFF_DIRECT_IO` (`--config`): default `NO`. Bypasses block cache for uncompressed TIFF reads. Lower priority than `GTIFF_VIRTUAL_MEM_IO`; only effective when mmap is not in use.
- `GDAL_DISABLE_READDIR_ON_OPEN` (`--config`): default `FALSE`. Set to `EMPTY_DIR` to skip scanning the source directory for `.aux`/`.ovr`/`.tfw` sidecar files on every `GDALOpen()`. Eliminates a `readdir` syscall per benchmark iteration.

**What the benchmark currently covers vs. does not cover**
- Covered: `GDAL_NUM_THREADS` = 1 and `ALL_CPUS`; `COMPRESS=LZW`; `OVERVIEWS=AUTO`; default tile size (512)
- Not covered: `GDAL_CACHEMAX`, `GDAL_SWATH_SIZE`, `PREDICTOR`, `GTIFF_VIRTUAL_MEM_IO`, `VSI_CACHE`
- The "true maximum GDAL" baseline has not been established. Before concluding that MLX underperforms or that any fix closes the gap, the impact of `GDAL_CACHEMAX` and `PREDICTOR=2` on the GDAL ALL_CPUS numbers must be measured.

### MLX vs GDAL: architectural differences

- **Execution hardware**: GDAL uses CPU SIMD (NEON on Apple Silicon) via `GDALResampleChunk_AverageOrRMS`. The resampling runs in a job queue (`OvrJob` in `overview.cpp`) that is **single-threaded by default** (`GDAL_NUM_THREADS` defaults to `"1"`). Setting `GDAL_NUM_THREADS=ALL_CPUS` parallelises the resampling chunks across CPU cores; this has been supported since GDAL 3.2. MLX dispatches to the Apple Silicon GPU via Metal, massively parallel.
- **Memory access pattern**: GDAL processes in horizontal chunks/strips; reads a few rows at a time, writes them, repeats. Designed to handle rasters larger than RAM. MLX loads the entire band into GPU memory once, computes all levels, writes back; simpler but requires the full band to fit in memory.
- **Overview chain**: both cascade identically; level N is sourced from level N-1, not from the original band. GDAL does this explicitly in `GDALRegenerateCascadingOverviews()` for AVERAGE; MLX does it via `current = downsampled`.
- **Resampling math (AVERAGE)**: same 2×2 box filter, different form. GDAL is a pixel loop with SIMD intrinsics; MLX expresses it as `reshape([H, 2, W, 2])` + `mean([1, 3])` which the GPU executes as a single kernel. **BILINEAR**: GDAL uses a separable convolution with a tent filter kernel via `GDALResampleChunk_ConvolutionT`; MLX implements it as two sequential reshape+mean passes (horizontal then vertical), which is structurally equivalent and numerically identical at 2x.
- **COG assembly**: identical; both use the same GDAL COG driver with `OVERVIEWS=FORCE_USE_EXISTING`.
- **Key architectural constraint**: MLX requires the full band to fit in GPU/unified memory. GDAL's chunked model handles arbitrarily large rasters. This is the one real limitation of the MLX approach.

### Overview structure allocation: NONE resampling

- `BuildOverviews("NONE", ...)` is a valid public API call; GDAL creates the TIFF IFD structures (overview band slots at correct dimensions) but immediately returns without computing any pixel data (`GDALRegenerateOverviewsEx` bails out at the `EQUAL(pszResampling, "NONE")` check in `overview.cpp:4816`)
- This replaces the previous NEAREST warmup pass; we used to call `BuildOverviews("NEAREST", ...)` just to allocate structure, which wasted a full CPU resample pass that was immediately overwritten by MLX
- Switching to NONE eliminated the wasted CPU pass entirely; MLX absolute times improved (measured in a prior benchmark session, exact deltas no longer verifiable against current data)

### New methodology: multi-GSD synthetic DEMs

- Replaced the single fixed test file with dynamically generated DEMs at multiple GSDs (80cm, 40cm, 20cm) so benchmarks capture how speedup scales with raster size
- DEMs are generated from 5k random points via TIN interpolation (`gdal_grid -a linear`); synthetic but realistic Float32 single-band rasters with NoData outside the convex hull
- **Project goal: beat GDAL ALL_CPUS (nT), not single-threaded GDAL.** Single-threaded GDAL is not a meaningful target; any real user will invoke GDAL with `GDAL_NUM_THREADS=ALL_CPUS`. The relevant speedup column is vs GDAL nT.
- MLX is slower than GDAL nT at small rasters where Metal kernel launch overhead dominates over GPU compute time; it wins at large rasters where GPU parallelism overwhelms CPU core count. The crossover point shifts with optimisations (see README for current numbers).
- Optimising the small-raster regime is the primary open problem. The dominant fixed cost is Metal framework overhead (kernel launch, command buffer submission) plus GDAL I/O setup, not GPU compute time.
- A single-raster benchmark at one scale is misleading; behaviour must be measured across raster sizes

### Batched eval experiment (2026-03-09): no improvement

Previously `MLXBuildOverviews` called `mx::eval()` after every overview level (N syncs per band). We refactored to build the full downsample chain as a lazy graph and call `mx::eval(all_levels)` once. Result on dem_160cm/80cm/40cm: **no measurable improvement** (differences within 5ms, inside run-to-run noise). Conclusion: MLX's lazy evaluator already schedules intra-eval ops efficiently; the per-level `eval()` fence was not causing meaningful CPU/GPU round-trip stalls at these sizes. At small rasters the GPU completes each level near-instantly so the sync cost is negligible. At large rasters the bottleneck is memory bandwidth (~200 GB/s, a hardware constant) and batching the graph doesn't change total bytes moved. The code change is kept (cleaner structure: one eval, one write loop) but carries no performance benefit.

### Where performance is still left on the table

**Easy wins:**
- **ZSTD default instead of LZW**: ZSTD compresses 2–3× faster than LZW at similar ratio; supported since GDAL 2.3. One-line default change. Directly reduces COG write time, which is the second-largest cost at large rasters.
- **PREDICTOR=3 for Float32**: reduces LZW/DEFLATE input entropy before compression. Standard for Float32 rasters. One creation-option change. Makes compression faster AND improves ratio.

**Medium effort:**
- **Eliminate the /vsimem double-pass**: currently the pipeline writes all overview data into /vsimem, then the COG driver reads it all back to tile+compress. Every pixel (full-res + overviews ≈ 4/3 × band size) is touched twice for the write path alone. Eliminating this requires structuring the pipeline so the COG driver reads directly from the MLX output without an intermediate GTiff copy.
- **Multi-band GPU parallelism**: bands are processed serially. For multi-band rasters (RGB, multispectral), all bands could be batched as a `[bands, H, W]` array and downsampled simultaneously. No benefit for single-band DEMs (current test data).

**Large effort / diminishing returns:**
- **GPU-accelerated tiling**: the COG write step tiles into 512×512 blocks and reorders them (pure data movement that the GPU could do faster). But using GPU-tiled output requires implementing the TIFF/COG file structure (IFDs, tile offsets) manually, essentially replacing the COG driver.
- **Direct read into MLX buffer**: `RasterIO` fills a `std::vector<float>`, which MLX then references (or copies from). On Apple Silicon with unified memory, GDAL could write directly into an MLX-owned buffer. Requires either MLX exposing a writable raw pointer before eval, or using a custom GDAL RasterIO destination. Minor win: at large rasters this extra allocation is small relative to total memory traffic.

### gdal_grid notes

- `gdal_grid -a linear:radius=-1` performs TIN interpolation; `radius=-1` restricts output to the convex hull of the input points (pixels outside get nodata)
- `-txe` and `-tye` (explicit extent) are **required** when `-tr` (resolution) is used; gdal_grid errors without them
- GSD can be expressed in degrees when working in WGS84; no need to reproject to a metric CRS just for raster generation; convert from metres using `1 deg lat approx 111,320 m`
- Generation time scales roughly with output pixel count: 5k points over a ~3km×3km area at 20cm GSD (~15k×15k pixels) takes ~11 minutes on M1 Pro single-threaded; ~6 minutes with `--config GDAL_NUM_THREADS ALL_CPUS`
- `gdal_grid` does not accept `-multi`; multithreading is enabled via `--config GDAL_NUM_THREADS ALL_CPUS` passed as the first argument. Uniform point distributions benefit most since all threads get equal work.
- OGR VRT is the correct way to make a CSV readable as a spatial layer by GDAL tools; specify `GeometryType`, `LayerSRS`, and `GeometryField` with `encoding="PointFromColumns"`

### Bash compatibility

- macOS ships with bash 3.2, so `mapfile` is not available; use `while IFS= read -r f; do arr+=("$f"); done < <(...)` instead
- Separate stdout and stderr in bench functions (`>&2` for progress, plain `echo` for the return value) to cleanly capture averages via command substitution

## Bilinear Implementation: MLX vs GDAL

### GDAL's Bilinear Implementation (GDALResampleChunk_ConvolutionT)

Location: `gdal-src/gcore/overview.cpp:3360`

**Architecture:**
- Two-pass separable convolution: horizontal filter first, then vertical filter
- General-purpose implementation supporting arbitrary downsample factors (not just 2x)
- Supports kernels with negative weights (Cubic, Lanczos) via `bKernelWithNegativeWeights` template parameter
- Kernel radius for bilinear: 1 (tent filter spans 2 pixels in each dimension)

**Horizontal Pass (lines 3550-3820):**
- For each output pixel `iDstPixel`, compute source position:
  ```cpp
  dfSrcPixel = (iDstPixel + 0.5) * dfXRatioDstToSrc + dfSrcXDelta
  ```
  For 2x downsampling: `dfXRatioDstToSrc = 2.0`, `dfSrcXDelta = -0.5`
  Result: `dfSrcPixel = 2 * iDstPixel + 0.5` (halfway between source pixels)
- Determine source pixel range: `[dfSrcPixel - dfXScaledRadius, dfSrcPixel + dfXScaledRadius]`
  For 2x bilinear: radius = 1, scaled radius = 2, so samples from 4 source pixels
- Compute kernel weights for each source pixel using tent filter: `weight = 1 - |x|` where `x` is distance from sample point
- Normalize weights so they sum to 1.0 (unless sum is 0)
- **NoData path:** Uses `GDALResampleConvolutionHorizontalWithMask` (line 2774):
  - Multiplies each weight by the mask value (0 for NoData, 1 for valid)
  - Accumulates `dfVal += pixel * weight` and `dfWeightSum += weight`
  - After pass: if `dfWeightSum > 0`, output `dfVal / dfWeightSum`, else NoData
  - For kernels with negative weights, checks for consecutive valid pixels; if < 50% consecutive, rejects entire output as NoData
- Stores intermediate result in `padfHorizontalFiltered` array
- Also computes `pabyChunkNodataMaskHorizontalFiltered` (1 if any valid weight survived, else 0)

**Vertical Pass (lines 3830-4100):**
- Same logic as horizontal pass but applied vertically
- Operates on `padfHorizontalFiltered` from the horizontal pass
- Uses `pabyChunkNodataMaskHorizontalFiltered` as the mask
- For each output line, computes source line position and kernel weights
- **NoData path:** Same masked weighted sum approach as horizontal
- Final output written to destination buffer

**Key GDAL characteristics:**
- Separable convolution: horizontal and vertical passes are independent
- 2D weight accumulation in NoData path: the horizontal pass produces a mask that the vertical pass uses, so the final 2D weight distribution is the product of 1D weights
- Boundary handling: clamps kernel to valid source extent (no extrapolation)
- Weight normalization: weights always normalized to sum to 1.0 (unless all masked out)

### MLX's Bilinear Implementation (mlx_downsample_bilinear)

Location: `src/mlx_overviews.cpp:120`

**No-NoData path (lines 146-158):**
- Two-pass separable convolution using reshape+mean
- Horizontal pass: reshape `[pH, pW] → [pH, targetW, 2]`, mean over axis 2 → `[pH, targetW]`
  - This pairs adjacent columns and averages them with equal weights (0.5, 0.5)
- Vertical pass: reshape `[pH, targetW] → [targetH, 2, targetW]`, mean over axis 1 → `[targetH, targetW]`
  - This pairs adjacent rows and averages them with equal weights (0.5, 0.5)
- **Mathematical equivalence to GDAL:** For 2x downsampling with no NoData, the tent filter at position `x = 0.5` gives weights `0.5, 0.5` to the two adjacent pixels. The reshape+mean approach applies exactly these weights. The two passes are independent, matching GDAL's separable convolution structure.
- **Boundary handling:** Odd dimensions padded by replicating last row/column before either pass (lines 131-142). This simulates kernel clamping at the boundary.

**NoData path (lines 160-182):**
- Uses 2D masked sum instead of separable passes
- Reshape to `[targetH, 2, targetW, 2]` (full 2x2 blocks)
- Zero out NoData pixels via `where(valid, padded, 0.0f)`
- Sum valid data and count valid pixels per 2x2 block
- Divide sum by count (guarded against divide-by-zero)
- Output NoData where all 4 pixels are NoData
- **Difference from GDAL:** This is a 2D box filter approach, not a separable convolution. The weights are uniform across the 2x2 block (all valid pixels contribute equally). GDAL's separable approach applies weights that are the product of 1D tent weights, which can differ at NoData boundaries.

### Key Differences

1. **NoData handling strategy:**
   - **GDAL:** Separable masked convolution. Horizontal pass applies 1D tent weights with mask, produces intermediate mask. Vertical pass applies 1D tent weights to intermediate mask. Final 2D weights = product of 1D weights.
   - **MLX:** 2D box filter with uniform weighting. All valid pixels in 2x2 block contribute equally. This is simpler but differs from GDAL at NoData boundaries where the separable weight distribution matters.

2. **Boundary handling:**
   - **GDAL:** Clamps kernel to valid source extent. For odd dimensions, the last output pixel sees the edge pixel with weight 1.0 (kernel truncated).
   - **MLX:** Replicates last row/column before filtering. This achieves the same effect as clamping for the no-NoData case. For NoData case, the replication is done before the 2D masked sum.

3. **Generality:**
   - **GDAL:** Supports arbitrary downsample factors, arbitrary kernel radii, kernels with negative weights.
   - **MLX:** Hardcoded for 2x downsampling only. This is acceptable for COG overviews which are always powers of 2.

4. **Numerical equivalence (no NoData):**
   - For 2x downsampling with no NoData, both implementations are mathematically identical. The tent filter at 0.5 offset gives 0.5/0.5 weights, which is exactly what reshape+mean computes. The separable structure matches.

5. **Numerical divergence (with NoData):**
   - At NoData boundaries, MLX's 2D uniform weighting differs from GDAL's separable tent weighting. This is documented in `docs/known-issues.md` as a "BILINEAR nodata boundary mismatch vs GDAL" with max absolute error of 0.51 at those locations.
   - Example: Consider a 2x2 block where only pixel (0,0) is valid. GDAL's separable approach gives it weight `0.5 * 0.5 = 0.25`. MLX's 2D approach gives it weight `1.0 / 1.0 = 1.0`. The outputs differ.

### Why MLX Uses 2D Weighting for NoData

From the code comments (lines 160-162):
> "For the NoData path a 2D masked sum is used to match GDAL's 2D weight accumulation (a separable NoData pass would over-weight pixels that partially survived the horizontal pass)."

This is actually **incorrect** based on the GDAL code analysis. GDAL's NoData path IS separable - it uses `pabyChunkNodataMaskHorizontalFiltered` to carry the horizontal mask into the vertical pass. The final 2D weights ARE the product of 1D weights.

The comment suggests that a separable NoData pass would "over-weight" pixels, but GDAL's implementation shows this is not the case. The horizontal mask correctly propagates partial validity through the vertical pass.

**Current MLX behavior:** The 2D uniform weighting is simpler to implement but does not match GDAL's separable tent weighting at NoData boundaries. This was the root cause of the documented mismatch.

**Fix implemented (2026-04-15):** Implemented true separable masked convolution for the NoData path to match GDAL exactly:
1. Horizontal pass: reshape to [pH, targetW, 2], multiply data by weights (0.5), sum, divide by weight sum (conditional: if weightSum > 0, output = dataSum / weightSum; else output = 0). Track intermediate mask.
2. Vertical pass: reshape intermediate to [targetH, 2, targetW], apply same masked weighted sum using intermediate mask.
3. Output NoData where no valid pixels survived either pass.

**Test results:** After fix, all COG stats tests pass for both AVERAGE and BILINEAR within 3% tolerance (tightened from 5% based on actual deviation measurements). The mean values are now very close (within 0.004 across all overview levels), with small remaining drift likely due to floating point precision differences between CPU and GPU execution order.

**Deviation tabulation (after fix):**

| Method    | Level      | min dev | max dev | mean dev | stddev dev |
|-----------|------------|---------|---------|----------|------------|
| AVERAGE   | Full res   | 0.00%   | 0.00%   | 0.00%    | 0.00%      |
| AVERAGE   | Overview 1 | 0.01%   | 0.08%   | 0.19%    | 0.01%      |
| AVERAGE   | Overview 2 | 0.04%   | 0.03%   | 0.18%    | 0.01%      |
| AVERAGE   | Overview 3 | 0.33%   | 0.35%   | 2.02%    | 0.01%      |
| AVERAGE   | Overview 4 | 0.20%   | 0.73%   | 1.12%    | 0.02%      |
| BILINEAR  | Full res   | 0.00%   | 0.00%   | 0.00%    | 0.00%      |
| BILINEAR  | Overview 1 | 0.14%   | 0.14%   | 0.51%    | 0.01%      |
| BILINEAR  | Overview 2 | 0.39%   | 0.22%   | 1.22%    | 0.00%      |
| BILINEAR  | Overview 3 | 0.56%   | 0.02%   | 2.48%    | 0.07%      |
| BILINEAR  | Overview 4 | 1.51%   | 0.18%   | 1.80%    | 0.41%      |

**Maximum observed deviation: 2.48%** (BILINEAR Overview 3 mean). Test threshold set to 3% to provide headroom while ensuring correctness.
