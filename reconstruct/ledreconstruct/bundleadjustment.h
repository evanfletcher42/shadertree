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

	// Load a camera calibration from a JSON file
	bool LoadCameraCalibrationFromJson(std::string& jsonPath);

	// Initialize the bundle adjustment problem.
	bool InitializeBA();

	// Attempt to reconstruct everything. Runs the entire process.
	bool Reconstruct();

	// Find correspondences between the two specified frames.
	bool FindFrameCorrespondences(int nFrame1, int nFrame2,
		std::vector< const PointObservation_t* >& frame1Obs, std::vector<cv::Point2f>& frame1Points,
		std::vector< const PointObservation_t* >& frame2Obs, std::vector<cv::Point2f>& frame2Points);

	// Attempt to find a homography that explains image point correspondences between two frames
	bool TwoFrameHomography( int nFrame1, int nFrame2, cv::Mat& homography );

	// DSO-style initialization. Unprojects points from frame 1, then solves a 2-frame BA problem.
	bool TwoFramePlanarInit(int nFrame1, int nFrame2);
	
	// Initialize the specified camera's cameraFromWorld transform via a PnP solve on the current map.
	bool InitCameraPosePnP(int nFrameIdx);

	// Attempt to initialize poses of new cameras that can see points in the current map.  
	// Returns the number of new cameras initialized.  
	int InitializeNewCameras();

	// Attempt to triangulate points that are visible in 2+ frames with known poses.
	int TriangulateNewPoints();

	// Construct a map with:
	// - key: all LEDs in the problem with a specified initialization state (param bInitialized)
	// - value: all frame indices currently in the problem (aka has a pose) that observe this LED
	std::unordered_map< int, std::vector< int > > FindObservingFrames( bool bInitialized ) const;

	// Triangulate a LED based on observations from specified frames.
	// Returns the number of inlier observations.
	int TriangulateConsensus( int nLedIdx, const std::vector< int > &frames );

	// Performs DLT triangulation over a specified subset of frame indices (param subset).
	// Compares this result against all specified frame indices (param frames) and returns which agree with the solution.
	std::vector< int > TriangulateSubset( int nLedIdx, const std::vector< int > &frames, const std::vector< cv::Mat > &projections,
		const std::vector< cv::Point2f > &undistorted, const std::vector< int > &subset );

	// Initialize points by linearly interpolating positions between other points.
	// Works okay for LED strands with consistent spacing.
	int InterpolateNewPoints();

	// Returns a specified frame's observation of a specified LED, or nullptr.
	const PointObservation_t* FindObservation(int nFrameIdx, int nLedIdx) const;

	// Test whether an LED is in front of all cameras that observe it.
	// Useful for resetting bad points or poor triangulations.
	bool TestLedInFrontOfAllObservingCameras( int nLedIdx );

	// Mark as unknown points that are in invalid locations (e.g. behind a camera).
	int CullInvalidPoints();

	// Reprojection error of a given observation in pixels.
	// Returns infinity if the observation is behind the specified camera.
	double ComputeReprojectionError(const std::array<double, 6>& cameraFromWorld, const PointObservation_t& obs) const;

	// Scan through the problem and remove or re-initialize LEDs and/or cameras with outlier residuals.
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

	// Writes the LED configuration JSON compatible with the shadertree web system.
	// Writes <basePath>.ledcoords.json: flat x,y,z per LED; 0,0,0 if not reconstructed.
	// Writes <basePath>.leds.json: [{addr, x, y, z, interpolated}].
	bool WriteSiteCoordinatesToJson(const std::string& basePath, bool bFlip);

	// Minimum number of covisible LEDs between the pair of frames used to initialize the BA problem.
	static constexpr int k_nMinInitSharedLeds = 100;

	// Minimum number of map point observations required to initialize the pose of a new camera.
	// Should be >= 4.  
	static constexpr int k_nMinObservationsForPnPInit = 6;

	// Minimum inlier fraction when initializing new camera viewpoints via PnP.
	static constexpr double k_flMinPnPInlierFrac = 0.8;

	// LED interpolation config
	static constexpr int k_nMaxInterpGap = 3;
	static constexpr double k_flMaxInterpChordRatio = 1.1;

	// Reprojection error threshold for outliers, in pixels
	static constexpr double k_flMaxReprojectionErrorPx = 10.0;

	// Vector (per frame) of vectors (per led observed) of LED observations
	std::vector< std::vector< PointObservation_t > >* m_pObservations;

	// Frames marked as outliers during the solve
	std::unordered_set< int > m_rejectedFrames;

	// Camera poses, indexed by frame id
	// Convention: pInCamera = cameraFromWorld @ pInWorld
	std::unordered_map< int, std::array<double, 6> > m_cameraFromWorldPoses;

	// Number of ceres-solver threads. 0 = one per physical CPU core
	// Note: multi-threaded ceres-solver is nondeterministic
	int m_nThreads = 0;

	// LED points in 3D space ("the map")
	double m_ledPoints[k_nMaxLeds][3];

	// Marks whether a given LED has been initialized.
	bool m_pointsInitialized[k_nMaxLeds];

	// Marks whether a given LED has been interpolated.
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

	// Perf timing
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
