#include "mlx_overviews.h"

#include <mlx/mlx.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mx = mlx::core;

/**
 * Average (box filter) 2x downsample on GPU.
 *
 * Takes a 2D float32 MLX array of shape [H, W] and returns [targetH, targetW]
 * where targetH = ceil(H/2), targetW = ceil(W/2).
 *
 * If H or W is odd, the last row/column is duplicated before averaging so
 * that edge pixels are not discarded.
 */
static mx::array mlx_downsample_average(const mx::array &input, int targetH,
                                        int targetW)
{
    int H = input.shape()[0];
    int W = input.shape()[1];

    // GDAL uses ceil(N/2) for overview sizes, so targetH*2 may exceed H by 1
    // when the source dimension is odd. Replicate the last row/column so the
    // boundary pixel averages with itself — matching GDAL's AVERAGE behaviour.
    mx::array padded = input;

    if (targetH * 2 > H)
    {
        mx::array lastRow = mx::slice(padded, {H - 1, 0}, {H, W});
        padded = mx::concatenate({padded, lastRow}, 0);
    }

    if (targetW * 2 > W)
    {
        int curH = padded.shape()[0];
        mx::array lastCol = mx::slice(padded, {0, W - 1}, {curH, W});
        padded = mx::concatenate({padded, lastCol}, 1);
    }

    // Reshape to [targetH, 2, targetW, 2] then average over the 2-sized axes
    mx::array reshaped = mx::reshape(padded, {targetH, 2, targetW, 2});
    mx::array result = mx::mean(reshaped, std::vector<int>{1, 3});

    return result;
}

CPLErr MLXBuildOverviews(GDALDataset *poDS, int nBands, const int *panBandList)
{
    for (int iBand = 0; iBand < nBands; iBand++)
    {
        GDALRasterBand *poBand = poDS->GetRasterBand(panBandList[iBand]);
        int W = poBand->GetXSize();
        int H = poBand->GetYSize();
        int nOvrCount = poBand->GetOverviewCount();

        if (nOvrCount == 0)
            continue;

        // Read full band as float32
        std::vector<float> bandData(static_cast<size_t>(W) * H);
        CPLErr eErr = poBand->RasterIO(GF_Read, 0, 0, W, H, bandData.data(),
                                       W, H, GDT_Float32, 0, 0);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "MLXBuildOverviews: RasterIO read failed for band %d",
                     panBandList[iBand]);
            return eErr;
        }

        // Load into MLX array on GPU
        mx::array current =
            mx::array(bandData.data(), {H, W}, mx::float32);
        mx::eval(current);

        // Iteratively downsample each overview level from the previous level
        for (int iOvr = 0; iOvr < nOvrCount; iOvr++)
        {
            GDALRasterBand *poOvr = poBand->GetOverview(iOvr);
            int oW = poOvr->GetXSize();
            int oH = poOvr->GetYSize();

            mx::array downsampled =
                mlx_downsample_average(current, oH, oW);
            mx::eval(downsampled);

            // Write result into the overview band
            std::vector<float> ovrData(static_cast<size_t>(oW) * oH);
            std::memcpy(ovrData.data(), downsampled.data<float>(),
                        static_cast<size_t>(oW) * oH * sizeof(float));

            eErr = poOvr->RasterIO(GF_Write, 0, 0, oW, oH, ovrData.data(),
                                   oW, oH, GDT_Float32, 0, 0);
            if (eErr != CE_None)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "MLXBuildOverviews: RasterIO write failed for band "
                         "%d overview %d",
                         panBandList[iBand], iOvr);
                return eErr;
            }

            current = downsampled;
        }

        fprintf(stderr, "  Band %d: %d overview level(s) computed on GPU\n",
                panBandList[iBand], nOvrCount);
    }

    return CE_None;
}
