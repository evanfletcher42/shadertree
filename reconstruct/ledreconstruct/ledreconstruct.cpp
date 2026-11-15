#include <iostream>

#include <math.h>
#include <memory>
#include <stdexcept>
#include <string>

#include <ceres/ceres.h>
#include <cxxopts.hpp>

#include "bundleadjustment.h"

int main(int argc, char** argv)
{
    // Command line options
    cxxopts::Options options("ledreconstruct", "Reconstruct 3D locations of LEDs on an addressible light string.");
    options.add_options()
        ("videos", "Directory of videos to decode", cxxopts::value<std::string>())
        ("observations", "Directory of observation JSONs to solve from (instead of --videos)", cxxopts::value<std::string>())
        ("dump-observations", "Directory for per-video observation JSONs (written, and reused when newer than the video)", cxxopts::value<std::string>())
        ("decode-images", "Directory for per-video decode visualizations (<video>.decode.png; is slow)", cxxopts::value<std::string>())
        ("c,calibration", "Path to camera calibration JSON file", cxxopts::value<std::string>())
        ("o,output", "Output aligned points JSON path (omit with --videos to only decode)", cxxopts::value<std::string>())
        ("site", "Also write site coordinates: <path>.ledcoords.json (paste into admin LedCoordsCfg) and <path>.leds.json", cxxopts::value<std::string>())
        ("threads", "Solver threads (default: one per CPU core)", cxxopts::value<int>()->default_value("0"));

    std::string videoDir;
    std::string observationsDir;
    std::string dumpDir;
    std::string imageDir;
    std::string calPath;
    std::string outJsonPath;
    int nThreads = 0;
    std::string sitePath;

    try
    {
        cxxopts::ParseResult result = options.parse(argc, argv);

        if (result.count("videos") == result.count("observations"))
            throw std::invalid_argument("specify exactly one of --videos or --observations");

        if (result.count("videos"))
            videoDir = result["videos"].as<std::string>();

        if (result.count("observations"))
            observationsDir = result["observations"].as<std::string>();

        if (result.count("dump-observations"))
            dumpDir = result["dump-observations"].as<std::string>();

        if (result.count("decode-images"))
            imageDir = result["decode-images"].as<std::string>();

        if (result.count("output"))
            outJsonPath = result["output"].as<std::string>();
        else if (videoDir.empty())
            throw std::invalid_argument("--output is required");

        nThreads = result["threads"].as<int>();

        if (result.count("site"))
            sitePath = result["site"].as<std::string>();

        if (result.count("calibration"))
            calPath = result["calibration"].as<std::string>();
        else if (!outJsonPath.empty())
            throw std::invalid_argument("--calibration is required");
    }
    catch (const std::exception& e)
    {
        printf("%s\n\n%s", e.what(), options.help().c_str());
        return 1;
    }

    // Process
    StageClock_t::time_point loadStart = StageClock_t::now();
    ObservationSet observationSet;
    bool bLoaded = videoDir.empty() ? observationSet.LoadFromJson(observationsDir) : observationSet.LoadFromVideos(videoDir, dumpDir, imageDir);
    if (!bLoaded)
    {
        printf("Error: no observations loaded\n");
        return 1;
    }

    printf("Loaded observations in %.2f s\n", SecondsSince(loadStart));

    if (outJsonPath.empty())
        return 0;

    std::unique_ptr<BundleAdjuster> pBundleAdjuster = std::make_unique<BundleAdjuster>(&observationSet.m_observations);
    BundleAdjuster& bundleAdjuster = *pBundleAdjuster;
    bundleAdjuster.m_nThreads = nThreads;

    if (!bundleAdjuster.LoadCameraCalibrationFromJson(calPath))
    {
        printf("Error: Failed to load calibration json: %s\n", calPath.c_str());
        return 1;
    };

    StageClock_t::time_point reconstructStart = StageClock_t::now();
    if (!bundleAdjuster.Reconstruct())
        return 1;

    printf("Reconstructed in %.2f s\n", SecondsSince(reconstructStart));

    bundleAdjuster.FindAlignmentTransform();

    printf("Writing aligned point cloud to: %s\n", outJsonPath.c_str());
    bundleAdjuster.WriteAlignedReconstructionToJson(outJsonPath);

    if (!sitePath.empty())
    {
        printf("Writing site coordinates to: %s.ledcoords.json, %s.leds.json\n", sitePath.c_str(), sitePath.c_str());
        bundleAdjuster.WriteSiteCoordinatesToJson(sitePath);
    }

    fflush(stdout);
    return 0;
}
