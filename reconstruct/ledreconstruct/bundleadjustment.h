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

	// Initialize the bundle adjustment problem from the pair of frames sharing the most LEDs whose motion
	// can't be explained by a homography, via TwoFramePlanarInit.
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

	// Initialize uninitialized LEDs seen by 2+ posed frames, by DLT over all of them.
	int TriangulateNewPoints();

	// Initialize points by linearally interpolating their positions between other points.  
	// Works okay for LED strings.  
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

	// Drop cameras whose median reprojection error exceeds k_flMaxReprojectionErrorPx (for good), then observations
	// above it, then LEDs left with fewer than 2 observations.  Returns the number of LEDs culled.
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

	// Output site coordinates (stweb LedCoordsCfg): Y up with the tree's wide end at the bottom (bFlip inverts that
	// choice), y in [-1, 1], centered on the tree's axis.  Writes <basePath>.ledcoords.json (flat x,y,z per address,
	// unmapped = 0,0,0; paste into admin) and <basePath>.leds.json ([{addr, x, y, z}] for mapped LEDs).
	bool WriteSiteCoordinatesToJson(const std::string& basePath, bool bFlip);

	// The init pair must share at least this many LEDs.
	static constexpr int k_nMinInitSharedLeds = 100;

	// Minimum number of map point observations required to initialize the pose of a new camera.
	// Should be >= 4.  
	static constexpr int k_nMinObservationsForPnPInit = 6;

	// A new camera's PnP solve must have at least this fraction of inliers.
	static constexpr double k_flMinPnPInlierFrac = 0.8;

	// Reprojection error above which an observation (or a camera's median) is an outlier.
	static constexpr double k_flMaxReprojectionErrorPx = 10.0;

	// Sorted by LED index within each frame.  Outlier observations are erased from these.
	std::vector< std::vector< PointObservation_t > >* m_pObservations;

	// Cameras culled for a bad pose; never re-added.
	std::unordered_set< int > m_rejectedFrames;

	// Map of camera poses, indexed by frame id.
	// Each pose describes the change of basis from world coordinates to camera coodrinates.
	std::unordered_map< int, std::array<double, 6> > m_cameraFromWorldPoses;

	// Ceres threads; 0 = one per core.  Ceres' multithreaded Schur elimination isn't bit-reproducible.
	int m_nThreads = 0;

	// LED points in 3D space ("the map")
	double m_ledPoints[k_nMaxLeds][3];

	// Marks whether a given LED has been initialized.
	bool m_pointsInitialized[k_nMaxLeds];

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
