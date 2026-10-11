#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include <opencv2/core.hpp>


typedef struct PointObservation
{
	typedef enum ObservationStatus
	{
		eUnprocessed = 0,  // Haven't looked at it
		eNoAssociation,    // Does not have a forward link
		eAssociated,       // Has a forward link
		eAmbiguousCode,    // Decoding LED index failed
		eLabeled,          // Has a valid LED index
		nStatuses
	} ObservationStatus_t;

	// Image location
	cv::Point2f p;

	// Frame where this point was detected
	int nFrameIdx;

	// Detected LED index
	int nLedIdx;

	ObservationStatus_t status = ObservationStatus::eUnprocessed;

} PointObservation_t;

class PointTracker
{
public:
	PointTracker();
	~PointTracker();

	bool LoadBlobsFromJson(std::string folderPath);

	// Set of observed LEDs in each frame.  Index like: m_observations[ nFrameIdx ][ nObservationIdxInThisFrame ]
	std::vector< std::vector< PointObservation_t > > m_observations;

	static constexpr int k_nMaxLeds = 215*32*2;

private:
	std::vector< std::string > findJsonPaths(std::string folderPath);
};
