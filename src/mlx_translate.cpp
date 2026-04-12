#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include <chrono>
#include <gdal_priv.h>
#include <cpl_string.h>

#include "mlx_overviews.h"

namespace {
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;

static double elapsed_ms(Clock::time_point start)
{
    return Ms(Clock::now() - start).count();
}
} // namespace

/**
 * Compute overview decimation levels (powers of 2) until the smallest
 * overview fits within a single tile (default 512x512).
 */
static std::vector<int> computeOverviewLevels(int width, int height,
                                              int tileSize = 512)
{
    std::vector<int> levels;
    int factor = 2;
    int w = width;
    int h = height;

    while (w > tileSize || h > tileSize)
    {
        levels.push_back(factor);
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        factor *= 2;
    }

    return levels;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr,
                "Usage: mlx_translate <input.tif> <output_cog.tif> "
                "[-r AVERAGE|BILINEAR] [-co KEY=VALUE ...]\n");
        return 1;
    }

    const char *inputPath = argv[1];
    const char *outputPath = argv[2];

    // Parse -r (resampling method) and -co options
    ResampleMethod resampleMethod = ResampleMethod::AVERAGE;
    char **papszCOOptions = nullptr;
    for (int i = 3; i < argc; i++)
    {
        if (EQUAL(argv[i], "-r") && i + 1 < argc)
        {
            const char *method = argv[++i];
            if (EQUAL(method, "BILINEAR"))
                resampleMethod = ResampleMethod::BILINEAR;
            else if (!EQUAL(method, "AVERAGE"))
            {
                fprintf(stderr, "Unknown resampling method: %s. "
                                "Supported: AVERAGE, BILINEAR\n", method);
                return 1;
            }
        }
        else if (EQUAL(argv[i], "-co") && i + 1 < argc)
        {
            papszCOOptions = CSLAddString(papszCOOptions, argv[++i]);
        }
    }

    // Default compression
    if (!CSLFetchNameValue(papszCOOptions, "COMPRESS"))
        papszCOOptions =
            CSLSetNameValue(papszCOOptions, "COMPRESS", "LZW");

    if (!CSLFetchNameValue(papszCOOptions, "NUM_THREADS"))
        papszCOOptions =
            CSLSetNameValue(papszCOOptions, "NUM_THREADS", "ALL_CPUS");

    // Enable multi-threaded GDAL operations for the entire pipeline:
    // - source tile decompression during temp GTiff creation
    // - LZW tile compression during final COG write
    CPLSetConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");

    GDALAllRegister();

    // Open source
    GDALDataset *poSrcDS =
        static_cast<GDALDataset *>(GDALOpen(inputPath, GA_ReadOnly));
    if (!poSrcDS)
    {
        fprintf(stderr, "Failed to open: %s\n", inputPath);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    int nBands = poSrcDS->GetRasterCount();
    int srcW = poSrcDS->GetRasterXSize();
    int srcH = poSrcDS->GetRasterYSize();

    // Validate data type: only Float32 is supported
    for (int i = 1; i <= nBands; i++)
    {
        GDALDataType dt = poSrcDS->GetRasterBand(i)->GetRasterDataType();
        if (dt != GDT_Float32)
        {
            fprintf(stderr,
                    "Error: band %d has data type %s. "
                    "Only Float32 is supported.\n",
                    i, GDALGetDataTypeName(dt));
            GDALClose(poSrcDS);
            CSLDestroy(papszCOOptions);
            return 1;
        }
    }

    fprintf(stderr, "Input: %s (%dx%d, %d band(s))\n",
            inputPath, srcW, srcH, nBands);

    auto t0 = Clock::now();
    auto tCopySrc = t0;
    auto tCog = t0;
    double msCopySrc = 0.0;
    double msBuildOvr = 0.0;
    double msMlxOvr = 0.0;
    double msCog = 0.0;
    CPLErr eErr = CE_None;

    // Create in-memory temp GTiff copy.
    // Forward BIGTIFF if the user requested it, so the temp file doesn't hit
    // the 4 GB classic-TIFF limit before overviews are written.
    const char *tmpPath = "/vsimem/mlx_translate_tmp.tif";
    GDALDriver *poTiffDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");
    char **papszTmpOptions = nullptr;
    const char *pszBigTiff = CSLFetchNameValue(papszCOOptions, "BIGTIFF");
    if (pszBigTiff)
        papszTmpOptions = CSLSetNameValue(papszTmpOptions, "BIGTIFF", pszBigTiff);
    tCopySrc = Clock::now();
    GDALDataset *poTmpDS = poTiffDriver->CreateCopy(
        tmpPath, poSrcDS, FALSE, papszTmpOptions, nullptr, nullptr);
    msCopySrc = elapsed_ms(tCopySrc);
    CSLDestroy(papszTmpOptions);
    GDALClose(poSrcDS); // all data copied; release source immediately
    if (!poTmpDS)
    {
        fprintf(stderr, "Failed to create temp dataset\n");
        CSLDestroy(papszCOOptions);
        return 1;
    }

    // Compute overview levels
    std::vector<int> ovrLevels = computeOverviewLevels(srcW, srcH);

    if (ovrLevels.empty())
    {
        fprintf(stderr, "Input is too small to require overviews\n");
    }
    else
    {
        fprintf(stderr, "Overview levels: ");
        for (int l : ovrLevels)
            fprintf(stderr, "%d ", l);
        fprintf(stderr, "\n");

        auto tBuildOvr = Clock::now();
        // Step 1: Call GDAL BuildOverviews with "NONE" to allocate overview
        // band structure without any CPU resampling. "NONE" is handled in
        // GDALRegenerateOverviewsEx() (overview.cpp) as an immediate return:
        // the TIFF IFDs are created but no pixel data is computed.
        // MLX overwrites the slots in step 2.
        eErr = poTmpDS->BuildOverviews(
            "NONE", static_cast<int>(ovrLevels.size()),
            ovrLevels.data(), 0, nullptr, GDALDummyProgress, nullptr);
        msBuildOvr = elapsed_ms(tBuildOvr);
        if (eErr != CE_None)
        {
            fprintf(stderr, "Failed to allocate overview structure\n");
            GDALClose(poTmpDS);
            CSLDestroy(papszCOOptions);
            return 1;
        }

        auto tMlxOvr = Clock::now();
        // Step 2: Overwrite overview data with MLX GPU-computed values
        fprintf(stderr, "Running MLX overview generation on GPU...\n");
        std::vector<int> bandList(nBands);
        for (int i = 0; i < nBands; i++)
            bandList[i] = i + 1;

        eErr = MLXBuildOverviews(poTmpDS, nBands, bandList.data(), resampleMethod);
        msMlxOvr = elapsed_ms(tMlxOvr);
        if (eErr != CE_None)
        {
            fprintf(stderr, "MLX overview generation failed\n");
            GDALClose(poTmpDS);
            CSLDestroy(papszCOOptions);
            return 1;
        }
    }

    // Write final COG using the COG driver with pre-built overviews
    papszCOOptions =
        CSLSetNameValue(papszCOOptions, "OVERVIEWS", "FORCE_USE_EXISTING");

    GDALDriver *poCOGDriver =
        GetGDALDriverManager()->GetDriverByName("COG");
    if (!poCOGDriver)
    {
        fprintf(stderr, "COG driver not available\n");
        GDALClose(poTmpDS);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    tCog = Clock::now();
    fprintf(stderr, "Writing COG: %s\n", outputPath);
    GDALDataset *poCOGDS = poCOGDriver->CreateCopy(
        outputPath, poTmpDS, FALSE, papszCOOptions,
        GDALTermProgress, nullptr);
    msCog = elapsed_ms(tCog);

    fprintf(stderr, "COG write: %.1fms\n", msCog);

    if (!poCOGDS)
    {
        fprintf(stderr, "Failed to write COG output\n");
        GDALClose(poTmpDS);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    // Note: OVERVIEW_RESAMPLING is not written to IMAGE_STRUCTURE metadata.
    // The COG driver only stores it when it generates overviews itself.
    // With FORCE_USE_EXISTING, GDAL ignores -co OVERVIEW_RESAMPLING and
    // SetMetadataItem("...", "IMAGE_STRUCTURE") does not persist in GTiff.

    // Cleanup
    GDALClose(poCOGDS);
    GDALClose(poTmpDS);
    GDALDeleteDataset(nullptr, tmpPath);
    CSLDestroy(papszCOOptions);

    double msTotal = elapsed_ms(t0);
    fprintf(stderr, "\n========== Timing Summary ==========\n");
    fprintf(stderr, "  Source copy:      %.1fms\n", msCopySrc);
    fprintf(stderr, "  Build overviews:  %.1fms\n", msBuildOvr);
    fprintf(stderr, "  MLX GPU compute:  %.1fms\n", msMlxOvr);
    fprintf(stderr, "  COG write:       %.1fms\n", msCog);
    fprintf(stderr, "  ----------------------------------\n");
    fprintf(stderr, "  Total:           %.1fms\n", msTotal);
    fprintf(stderr, "========================================\n");

    fprintf(stderr, "Done.\n");
    return 0;
}
