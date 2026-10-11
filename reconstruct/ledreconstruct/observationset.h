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

// Labeled LED observations, one set per view (video).
class ObservationSet
{
public:
	// Decode every video in a directory, one view per video.
	// If dumpDir is set, cache each view there as <video>.json; a cache newer than its video skips decoding.
	// If imageDir is set, write a decode image there for each video actually decoded.
	bool LoadFromVideos( const std::string &videoDir, const std::string &dumpDir, const std::string &imageDir );

	// Load observation JSONs (one view per file), as written by LoadFromVideos.
	bool LoadFromJson( const std::string &jsonDir );

	// Index like: m_observations[ nFrameIdx ][ nObservationIdxInThisFrame ], sorted by nLedIdx within each frame
	std::vector< std::vector< PointObservation_t > > m_observations;
};
