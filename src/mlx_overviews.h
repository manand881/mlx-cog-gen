#pragma once

#include <gdal_priv.h>

/**
 * MLX-accelerated overview generation.
 *
 * Replaces GDALRegenerateOverviewsEx() for each band in the dataset.
 * Reads each band, downsamples iteratively on the Apple Silicon GPU using
 * average (box filter) resampling, and writes the results into the
 * pre-allocated overview bands.
 *
 * Overview bands must already exist (call GDALDataset::BuildOverviews() first
 * to allocate structure, then call this to overwrite with GPU-computed data).
 *
 * @param poDS      Dataset with overview structure already allocated.
 * @param nBands    Number of bands to process.
 * @param panBandList 1-based band indices to process.
 * @return CE_None on success, CE_Failure on error.
 */
CPLErr MLXBuildOverviews(GDALDataset *poDS, int nBands,
                         const int *panBandList);
