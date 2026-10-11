#pragma once

#include <ceres/ceres.h>
#include <unordered_set>
#include <string>

#include "ledobservationresidual.h"

#include "ledconstants.h"
#include "observationset.h"

#include "pointsvisualizer.h"
#include "stagetimes.h"

class BundleAdjuster
{
public:
	BundleAdjuster( std::vector< std::vector< PointObservation_t > > *pObservations );

	~BundleAdjuster();

	// Load a camera calibration from a JSON file.
	bool LoadCameraCalibrationFromJson(std::string& jsonPath);

	// Initialize from the frame pair with parallax that shares the most LEDs.
	bool InitializeBA();

	// Attempt to reconstruct everything.  Runs the entire process.
	bool Reconstruct();

	// Find correspondences between the two specified frames.
	bool FindFrameCorrespondences(int nFrame1, int nFrame2,
		std::vector< const PointObservation_t* >& frame1Obs, std::vector<cv::Point2f>& frame1Points,
		std::vector< const PointObservation_t* >& frame2Obs, std::vector<cv::Point2f>& frame2Points);

	// Attempt to find a homography that explains image point correspondences between two frames.
	bool TwoFrameHomography( int nFrame1, int nFrame2, cv::Mat& homography );

	// Do a DSO-style initialization, where points are unprojected from frame 1, and the initial frame 1 -> frame 2 
	// transform guess is identity. An immediate bundle adjustment over these 2 frames makes init proper. 
	bool TwoFramePlanarInit(int nFrame1, int nFrame2);
	
	// Initialize the specified camera's cameraFromWorld transform via a PnP solve on the current map.
	bool InitCameraPosePnP(int nFrameIdx);

	// Attempt to initialize poses of new cameras that can see points in the current map.  
	// Returns the number of new cameras initialized.  
	int InitializeNewCameras();

	// Triangulate uninitialized LEDs seen by 2+ posed frames.
	int TriangulateNewPoints();

	// Posed frames observing each LED whose m_pointsInitialized is bInitialized.
	std::unordered_map< int, std::vector< int > > FindObservingFrames( bool bInitialized ) const;

	// Triangulate an LED into m_ledPoints. Returns how many observations agree.
	// Tries a DLT over all observations first.
	// If no more than half agree, uses the observations that agree with the best 2-frame DLT.
	int TriangulateConsensus( int nLedIdx, const std::vector< int > &frames );

	// DLT over a subset of observations. Returns every observation that agrees with it.
	std::vector< int > TriangulateSubset( int nLedIdx, const std::vector< int > &frames, const std::vector< cv::Mat > &projections,
		const std::vector< cv::Point2f > &undistorted, const std::vector< int > &subset );

	// Fill short in-strand gaps by linear interpolation between solved LEDs.
	int InterpolateNewPoints();

	// A frame's observation of an LED, or nullptr.
	const PointObservation_t* FindObservation(int nFrameIdx, int nLedIdx) const;

	// Test whether an LED is in front of all cameras that observe it.
	// Useful for resetting bad points or poor triangulations.
	bool TestLedInFrontOfAllObservingCameras( int nLedIdx );

	// Mark as unknown points that are in invalid locations (e.g. behind a camera).
	int CullInvalidPoints();

	// Reprojection error of one observation in pixels; infinite if the point is behind the camera.
	double ComputeReprojectionError(const std::array<double, 6>& cameraFromWorld, const PointObservation_t& obs) const;

	// In order, using k_flMaxReprojectionErrorPx:
	// - drop cameras whose median error is above it, for good
	// - re-triangulate LEDs where most observations are above it
	// - drop observations above it
	// - drop LEDs left with fewer than 2 observations
	// Returns the number of LEDs culled.
	int CullReprojectionOutliers();

	// Set up a bundle adjustment problem over the entire map, as specified.
	bool SetupBundleAdjustmentProblem( ceres::Problem &problem, bool bSolveIntrinsics, bool bSolvePoses, bool bSolvePoints );

	bool SolveProblem(ceres::Problem& problem);

	// Find the transform that aligns the reconstruction along the Z axis.  
	bool FindAlignmentTransform();

	// Output a file containing all recovered LED locations and camera poses.
	bool WriteSolutionToJson(std::string& outPath);

	// Output a file containing LED locations, after alignment.
	bool WriteAlignedReconstructionToJson(const std::string& outPath);

	// LedCoordsCfg coordinates: Y up, y in [-1, 1], tree axis at x = z = 0.
	// Wide end down unless bFlip.
	// Writes <basePath>.ledcoords.json: flat x,y,z per address, 0,0,0 if unmapped.
	// Writes <basePath>.leds.json: [{addr, x, y, z, interpolated}].
	bool WriteSiteCoordinatesToJson(const std::string& basePath, bool bFlip);

	// The init pair must share at least this many LEDs.
	static constexpr int k_nMinInitSharedLeds = 100;

	// Minimum number of map point observations required to initialize the pose of a new camera.
	// Should be >= 4.  
	static constexpr int k_nMinObservationsForPnPInit = 6;

	// A new camera's PnP solve must have at least this fraction of inliers.
	static constexpr double k_flMinPnPInlierFrac = 0.8;

	// Longest gap interpolation fills.
	static constexpr int k_nMaxInterpGap = 3;
	// Max end-to-end distance across a gap, in units of (gap + 1) LED pitches.
	static constexpr double k_flMaxInterpChordRatio = 1.1;

	// Reprojection error above which an observation (or a camera's median) is an outlier.
	static constexpr double k_flMaxReprojectionErrorPx = 10.0;

	// Sorted by LED index within each frame.
	// Outlier observations are erased from these.
	std::vector< std::vector< PointObservation_t > >* m_pObservations;

	// Cameras culled for a bad pose; never re-added.
	std::unordered_set< int > m_rejectedFrames;

	// Map of camera poses, indexed by frame id.
	// Each pose describes the change of basis from world coordinates to camera coodrinates.
	std::unordered_map< int, std::array<double, 6> > m_cameraFromWorldPoses;

	// Ceres threads; 0 = one per core.
	// More than one isn't bit-reproducible.
	int m_nThreads = 0;

	// LED points in 3D space ("the map")
	double m_ledPoints[k_nMaxLeds][3];

	// Marks whether a given LED has been initialized.
	bool m_pointsInitialized[k_nMaxLeds];

	// Interpolated, not solved.
	// Never in a solve or --output.
	bool m_pointsInterpolated[k_nMaxLeds];

private:
	static void composeRT(double* pose, cv::Mat& poseRT);

	void ComputeAlignedPoint(double* pAligned, int nLedIdx) const;

	// The frame ID which defines the origin.
	int m_nOriginFrameId;

	// Alignment transform.
	double m_alignedFromWorld[6];

	// Camera calibration, valid for images of m_imageSize.
	cv::Size m_imageSize;
	cv::Mat m_cameraMat;
	cv::Mat m_distCoefs;
	double m_intrinsics[LedObservationResidual::nIntrinsicsTraits];

	// Visualizations
	CPointsVisualizer m_visualizer;

	CStageTimes m_stageTimes;
};

class VisualizerCallback : public ceres::IterationCallback
{
public:
	explicit VisualizerCallback(BundleAdjuster* pBundleAdjuster, CPointsVisualizer* pVisualizer);
	~VisualizerCallback();

	ceres::CallbackReturnType operator()(const ceres::IterationSummary& summary);

	BundleAdjuster* m_pBundleAdjuster = NULL;
	CPointsVisualizer* m_pVisualizer = NULL;
};
