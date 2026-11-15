#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>


struct PointObservation_t
{
	// Image location (x = col, y = row)
	cv::Point2f p;

	// Detected LED index
	int nLedIdx = -1;

	int nPixels = 0;
};

// Labeled LED observations from all viewpoints
class ObservationSet
{
public:
	// Decode viewpoints from videos in a specified directory.
	// If dumpDir is not empty: Cache decoded observations there as <video>.json; re-use these if they're newer than the video
	// If imageDir is not empty: Write a debug image for each decoded video to <video>.decode.png
	bool LoadFromVideos( const std::string &videoDir, const std::string &dumpDir, const std::string &imageDir );

	// Load observation JSONs (one view per file), as written by LoadFromVideos.
	bool LoadFromJson( const std::string &jsonDir );

	// Addressing: m_observations[ nFrameIdx ][ nObservationIdxInThisFrame ]
	std::vector< std::vector< PointObservation_t > > m_observations;
};
