#include <iostream>

#include <math.h>
#include <memory>
#include <string>

#include <ceres/ceres.h>
#include <cxxopts.hpp>
#include <glog/logging.h>

#include "bundleadjustment.h"
#include "observationset.h"

int main(int argc, char** argv)
{
    google::InitGoogleLogging(argv[0]);

    // Unbuffered, so redirected logs keep their tail and stay in order with Ceres' output
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Command line options
    cxxopts::Options options("ledreconstruct", "Reconstruct 3D locations of LEDs on an addressible light string.");
    options.add_options()
        ("videos", "Directory of videos to decode", cxxopts::value<std::string>())
        ("observations", "Directory of observation JSONs to solve from (instead of --videos)", cxxopts::value<std::string>())
        ("dump-observations", "Directory for per-video observation JSONs (written, and reused when newer than the video)", cxxopts::value<std::string>())
        ("c,calibration", "Path to camera calibration JSON file", cxxopts::value<std::string>())
        ("s,solution", "Output unaligned solution JSON path (optional)", cxxopts::value<std::string>())
        ("o,output", "Output aligned points JSON path (omit with --videos to only decode)", cxxopts::value<std::string>())
        ("seed", "Random seed for triangulation pair choice", cxxopts::value<uint32_t>()->default_value("0"))
        ("threads", "Solver threads (default: one per core; 1 makes runs bit-reproducible)", cxxopts::value<int>()->default_value("0"));

    std::string videoDir;
    std::string observationsDir;
    std::string dumpDir;
    std::string calPath;
    std::string outSolutionPath;
    std::string outJsonPath;
    int nThreads = 0;
    uint32_t nSeed = 0;

    try
    {
        cxxopts::ParseResult result = options.parse(argc, argv);

        if (result.count("videos") == result.count("observations"))
            throw cxxopts::OptionParseException("specify exactly one of --videos or --observations");

        if (result.count("videos"))
            videoDir = result["videos"].as<std::string>();

        if (result.count("observations"))
            observationsDir = result["observations"].as<std::string>();

        if (result.count("dump-observations"))
            dumpDir = result["dump-observations"].as<std::string>();

        if (result.count("solution"))
            outSolutionPath = result["solution"].as<std::string>();

        if (result.count("output"))
            outJsonPath = result["output"].as<std::string>();
        else if (videoDir.empty())
            throw cxxopts::option_required_exception("output");

        nThreads = result["threads"].as<int>();
        nSeed = result["seed"].as<uint32_t>();

        if (result.count("calibration"))
            calPath = result["calibration"].as<std::string>();
        else if (!outJsonPath.empty())
            throw cxxopts::option_required_exception("calibration");
    }
    catch (cxxopts::OptionException& e)
    {
        printf("%s\n\n%s", e.what(), options.help().c_str());
        return 1;
    }

    // Process
    ObservationSet observationSet;
    bool bLoaded = videoDir.empty() ? observationSet.LoadFromJson(observationsDir) : observationSet.LoadFromVideos(videoDir, dumpDir);
    if (!bLoaded)
    {
        printf("Error: no observations loaded\n");
        return 1;
    }

    if (outJsonPath.empty())
        return 0;

    // Big: keep it off the stack
    std::unique_ptr<BundleAdjuster> pBundleAdjuster = std::make_unique<BundleAdjuster>(&observationSet.m_observations, nSeed);
    BundleAdjuster& bundleAdjuster = *pBundleAdjuster;
    bundleAdjuster.m_nThreads = nThreads;

    if (!bundleAdjuster.LoadCameraCalibrationFromJson(calPath))
    {
        printf("Error: Failed to load calibration json: %s\n", calPath.c_str());
        return 1;
    };

    if (!bundleAdjuster.Reconstruct())
        return 1;

    bundleAdjuster.FindAlignmentTransform();

    if (!outSolutionPath.empty())
    {
        printf("Writing full solution to: %s\n", outSolutionPath.c_str());
        bundleAdjuster.WriteSolutionToJson(outSolutionPath);
    }

    printf("Writing aligned point cloud to: %s\n", outJsonPath.c_str());
    bundleAdjuster.WriteAlignedReconstructionToJson(outJsonPath);

    return 0;
}
