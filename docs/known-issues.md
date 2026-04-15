# Known Issues

## Correctness

### NaN nodata values not supported
`mx::equal(padded, NaN)` returns false for every pixel because `NaN != NaN` by IEEE 754.
NaN nodata pixels cannot be masked correctly and would propagate through averaging,
corrupting all overviews.

**Current behavior:** The tool now rejects datasets with NaN no-data values at startup
with a clear error message: "MLXBuildOverviews: NaN no-data values are not supported.
Convert the dataset to use a numeric no-data value (e.g., -9999) before processing."

**Workaround:** Convert the dataset to use a numeric nodata value (e.g. `-9999`) before
processing.

---

### BILINEAR nodata boundary mismatch vs GDAL (FIXED 2026-04-15)
At nodata boundaries, our BILINEAR implementation previously disagreed with GDAL on which output
pixels should be nodata. In testing with a 960×960 circular raster (34% nodata), 883
output pixels at overview level 1 had a different nodata/valid classification compared
to GDAL BILINEAR, with max absolute error of 0.51 at those locations.

**Fix:** Implemented true separable masked convolution matching GDAL's `GDALResampleChunk_ConvolutionT`:
- Horizontal pass applies 1D tent weights with mask, produces intermediate values and mask
- Vertical pass applies 1D tent weights to intermediate values using intermediate mask
- Final 2D weights = product of 1D weights (matching GDAL)

**Status:** Fixed. All COG stats tests pass for both AVERAGE and BILINEAR within 5% tolerance.

---

### nodata=0 with valid zero-valued pixels
If a raster uses `0` as its nodata value but also contains genuinely valid pixels at
elevation 0 (e.g. a DEM covering coastal terrain at sea level), both GDAL and
`mlx_translate` will incorrectly treat those valid zero pixels as nodata and exclude
them from the average.

This is a fundamental limitation of the nodata-value approach, not a bug in our code.
The correct fix is at the dataset level: DEMs should use a nodata value outside the
valid data range (convention is `-9999` or `-32768`). Neither implementation can recover
a dataset authored with this mistake.

---

## Accuracy

### Overview dimensions: ceil vs floor (1px per level)
`BuildOverviews("NONE", factors)` allocates IFD structures using `ceil(N/2)` for
overview dimensions. However, `gdal_translate -co OVERVIEWS=AUTO` (the COG driver path
we benchmark against) uses `floor(N/2)`. Our overview bands are therefore 1 pixel wider
and taller per level than GDAL's.

The level count matches exactly for all tested rasters. The geographic extent is
unaffected. The difference is ~0.01% extra work per level.

This cannot be fixed without bypassing `BuildOverviews` for structure allocation,
since we rely on it to create the TIFF IFDs.

---

## Limitations

### Full band must fit in GPU/unified memory
MLX loads the entire raster band into GPU-accessible unified memory before computing
overviews. There is no chunked processing path. For very large rasters on machines with
limited RAM, `mlx_translate` will fail or cause memory pressure.

GDAL's chunked model (`GDAL_SWATH_SIZE`) handles arbitrarily large rasters regardless
of available memory. On a 16 GB M1 Pro, the practical limit for `mlx_translate` is
rasters whose full-res band fits comfortably in memory alongside the OS working set
(verified up to ~928 MB / `dem_5cm`).
