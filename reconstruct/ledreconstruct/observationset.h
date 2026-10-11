#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>


struct PointObservation_t
{
	// Image location (x = col, y = row)
	cv::Point2f p;

	// Decoded LED index
	int nLedIdx = -1;

	// Blob size
	int nPixels = 0;
};

// Labeled LED observations, one set per view (video).
class ObservationSet
{
public:
	// Decode every video in a directory, one view per video. If dumpDir is set, write each view's observations
	// there as <video>.json, and reuse that file instead of decoding when it is newer than the video.
	bool LoadFromVideos( const std::string &videoDir, const std::string &dumpDir );

	// Load observation JSONs (one view per file), as written by LoadFromVideos.
	bool LoadFromJson( const std::string &jsonDir );

	// Index like: m_observations[ nFrameIdx ][ nObservationIdxInThisFrame ], sorted by nLedIdx within each frame
	std::vector< std::vector< PointObservation_t > > m_observations;
};
