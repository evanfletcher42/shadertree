#include <algorithm>
#include <fstream>
#include <limits>

#include <Eigen/Core>
#include <ceres/rotation.h>
#include <json/json.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/core/eigen.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "bundleadjustment.h"

static double Median(std::vector<double> values)
{
	std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
	return values[values.size() / 2];
}

BundleAdjuster::BundleAdjuster(std::vector<std::vector<PointObservation_t>>* pObservations)
	: m_pObservations(pObservations)
{
	// No points are initialized (yet)
	memset(m_ledPoints, 0, sizeof(m_ledPoints));
	memset(m_pointsInitialized, 0, sizeof(m_pointsInitialized));

	// Initial alignment transform -> identity
	memset(m_alignedFromWorld, 0, sizeof(m_alignedFromWorld));

	// Origin frame ID is unknownd
	m_nOriginFrameId = -1;

	// Initialize camera calibration to something that would never work.
	// User should load camera calibration from JSON.  
	m_cameraMat = cv::Mat::zeros(3, 3, CV_32FC1);
	m_distCoefs = cv::Mat::zeros(1, 5, CV_32FC1);
}

BundleAdjuster::~BundleAdjuster()
{
}

bool BundleAdjuster::LoadCameraCalibrationFromJson(std::string& jsonPath)
{
	Json::Value calJson;
	std::ifstream ifs(jsonPath);

	if (!ifs)
		return false;

	ifs >> calJson;

	if (!calJson.isMember("width") ||
		!calJson.isMember("height") ||
		!calJson.isMember("fx") ||
		!calJson.isMember("fy") ||
		!calJson.isMember("cx") ||
		!calJson.isMember("cy") ||
		!calJson.isMember("dist"))
		return false;

	m_imageSize = cv::Size(calJson["width"].asInt(), calJson["height"].asInt());

	m_cameraMat = cv::Mat::eye(cv::Size(3, 3), CV_32FC1);
	m_cameraMat.at<float>(0, 0) = calJson["fx"].asFloat();
	m_cameraMat.at<float>(1, 1) = calJson["fy"].asFloat();
	m_cameraMat.at<float>(0, 2) = calJson["cx"].asFloat();
	m_cameraMat.at<float>(1, 2) = calJson["cy"].asFloat();

	m_distCoefs = cv::Mat::zeros(1, 5, CV_32FC1);
	for (int i = 0; i < 5; i++)
	{
		m_distCoefs.at<float>(i) = calJson["dist"][i].asFloat();
		printf("Dist: %f\n", m_distCoefs.at<float>(i));
	}

	m_intrinsics[LedObservationResidual::eFx] = calJson["fx"].asDouble();
	m_intrinsics[LedObservationResidual::eFy] = calJson["fy"].asDouble();
	m_intrinsics[LedObservationResidual::eCx] = calJson["cx"].asDouble();
	m_intrinsics[LedObservationResidual::eCy] = calJson["cy"].asDouble();
	m_intrinsics[LedObservationResidual::eK1] = calJson["dist"][0].asDouble();
	m_intrinsics[LedObservationResidual::eK2] = calJson["dist"][1].asDouble();
	m_intrinsics[LedObservationResidual::eP1] = calJson["dist"][2].asDouble();
	m_intrinsics[LedObservationResidual::eP2] = calJson["dist"][3].asDouble();
	m_intrinsics[LedObservationResidual::eK3] = calJson["dist"][4].asDouble();


	return true;
}

bool BundleAdjuster::InitializeBA()
{
	// Init from the pair of frames sharing the most LEDs whose motion a homography can't explain (parallax, so
	// triangulation works).  Pairs are tried most-shared first; frame 1 (the origin) is the one with more LEDs.
	StageClock_t::time_point pairSearchStart = StageClock_t::now();
	int nFrames = (int)m_pObservations->size();
	std::vector< std::array<int, 3> > pairs;  // shared LEDs, frame 1, frame 2
	for (int i = 0; i < nFrames; i++)
	{
		for (int j = i + 1; j < nFrames; j++)
		{
			std::vector< const PointObservation_t* > obs1, obs2;
			std::vector< cv::Point2f > points1, points2;
			FindFrameCorrespondences(i, j, obs1, points1, obs2, points2);

			if ((int)obs1.size() < k_nMinInitSharedLeds)
				continue;

			bool bFirstHasMore = (*m_pObservations)[i].size() >= (*m_pObservations)[j].size();
			pairs.push_back({ (int)obs1.size(), bFirstHasMore ? i : j, bFirstHasMore ? j : i });
		}
	}
	std::sort(pairs.begin(), pairs.end(), std::greater< std::array<int, 3> >());

	printf("Looking for perspective differences...\n");
	int nFrame1 = -1;
	int nFrame2 = -1;
	for (const std::array<int, 3>& pair : pairs)
	{
		cv::Mat homography;
		if (TwoFrameHomography(pair[1], pair[2], homography))
			continue;

		nFrame1 = pair[1];
		nFrame2 = pair[2];
		break;
	}

	if (nFrame1 < 0)
	{
		printf("InitializeBA: No pair of frames shares %d LEDs with enough parallax\n", k_nMinInitSharedLeds);
		return false;
	}

	m_stageTimes.Add("init pair search", SecondsSince(pairSearchStart));
	printf("Init with frames %d and %d\n", nFrame1, nFrame2);

	// Do a DSO-style two frame init (where frame 1 points are unprojected to Z=1)
	if (!TwoFramePlanarInit(nFrame1, nFrame2))
		return false;

	// Refine with a two-frame bundle adjustment.  
	ceres::Problem problem;

	if (!SetupBundleAdjustmentProblem(problem, false, true, true))
		return false;

	return SolveProblem(problem);
}

bool BundleAdjuster::Reconstruct()
{
	if (!InitializeBA())
	{
		printf("Reconstruct: Initialization failed\n");
		return false;
	}

	m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);

	int nInitPointsRemoved = CullInvalidPoints();
	printf("Reconstruct: Removed %d invalid points from initial BA solve\n", nInitPointsRemoved);

	int nIterations = 0;
	while (true)
	{
		printf("\nReconstruct: Iteration %d\n", nIterations);
		nIterations++;

		printf("\nReconstruct: Initializing new viewpoints...\n");
		m_visualizer.SetTitle("Initializing new viewpoints...");
		int nCamerasAdded = InitializeNewCameras();

		m_visualizer.SetTitle("Init new viewpoints");
		m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);

		if (nCamerasAdded <= 0)
		{
			printf("Reconstruct: No new viewpoints\n");
			break;
		}

		printf("Solving poses for new viewpoints...\n");
		m_visualizer.SetTitle("Solving new viewpoint poses...");
		ceres::Problem problem1;
		SetupBundleAdjustmentProblem(problem1, false, true, false);
		SolveProblem(problem1);

		m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);

		printf("Solving poses + points for new viewpoints...\n");
		m_visualizer.SetTitle("Solving points and poses (new viewpoints)...");

		ceres::Problem problem2;
		SetupBundleAdjustmentProblem(problem2, false, true, true);
		SolveProblem(problem2);

		CullReprojectionOutliers();

		m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);

		printf("Triangulating new points...\n");
		m_visualizer.SetTitle("Triangulating new points...\n");

		int nPointsAdded = TriangulateNewPoints();
		printf("Triangulated %d points\n", nPointsAdded);

		printf("Culling invalid points...\n");
		int nPointsRemoved = CullInvalidPoints();
		printf("Culled %d invalid points\n", nPointsRemoved);

		m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);

		// Skip 2nd BA solve if we didn't add new points
		if (nPointsAdded <= 0)
			continue;

		printf("Solving poses + points for new points...\n");
		m_visualizer.SetTitle("Solving points and poses (new points)...");
		// Refine intrinsics here too, once each round's new points are in.
		ceres::Problem problem3;
		SetupBundleAdjustmentProblem(problem3, true, true, true);
		SolveProblem(problem3);

		CullReprojectionOutliers();

		m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);
	}

	// Final BA, intrinsics included.

	printf("\nReconstruct: Solving final BA...\n");
	m_visualizer.SetTitle("Solving final BA...");

	ceres::Problem problem;
	SetupBundleAdjustmentProblem(problem, true, true, true);
	SolveProblem(problem);

	CullReprojectionOutliers();

	// With final BA, try one last round of triangulation.  
	int nPointsAdded = TriangulateNewPoints();
	printf("Reconstruct: Triangulated %d points\n", nPointsAdded);

	int nPointsRemoved = CullInvalidPoints();
	printf("Reconstruct: Culled %d invalid points\n", nPointsRemoved);

	m_visualizer.OnDataUpdate(m_cameraFromWorldPoses, (double*)m_ledPoints, m_pointsInitialized);

	// Paper over any gaps with linear interpolation.  
	// Attempt one last round of bundle adjustment in case any of these LEDs are observed.  They won't move if not.  

	m_visualizer.SetTitle("Interpolate missing points & solve...");
	//InterpolateNewPoints();
	ceres::Problem problem4;
	SetupBundleAdjustmentProblem(problem4, true, true, true);
	SolveProblem(problem4);

	printf("Reconstruct complete\n");
	printf("Solution used %zu of %zu frames\n", m_cameraFromWorldPoses.size(), m_pObservations->size());
	m_stageTimes.Print("Reconstruct");

	return true;
}

bool BundleAdjuster::FindFrameCorrespondences(int nFrame1, int nFrame2, 
	std::vector< const PointObservation_t* > &frame1Obs, std::vector<cv::Point2f>& frame1Points, 
	std::vector< const PointObservation_t* > &frame2Obs, std::vector<cv::Point2f>& frame2Points)
{
	// Both lists are sorted by LED index: merge them.
	const std::vector<PointObservation_t>& observations1 = (*m_pObservations)[nFrame1];
	const std::vector<PointObservation_t>& observations2 = (*m_pObservations)[nFrame2];

	size_t i1 = 0;
	size_t i2 = 0;
	while (i1 < observations1.size() && i2 < observations2.size())
	{
		const PointObservation_t& obs1 = observations1[i1];
		const PointObservation_t& obs2 = observations2[i2];

		if (obs1.nLedIdx < obs2.nLedIdx)
		{
			i1++;
			continue;
		}

		if (obs2.nLedIdx < obs1.nLedIdx)
		{
			i2++;
			continue;
		}

		frame1Points.push_back(obs1.p);
		frame2Points.push_back(obs2.p);

		frame1Obs.push_back(&obs1);
		frame2Obs.push_back(&obs2);

		i1++;
		i2++;
	}

	return true;
}

bool BundleAdjuster::TwoFrameHomography(int nFrame1, int nFrame2, cv::Mat &homography)
{
	std::vector< cv::Point2f > frame1Pts;
	std::vector< cv::Point2f > frame2Pts;

	std::vector< const PointObservation_t* > frame1Obs;
	std::vector< const PointObservation_t* > frame2Obs;

	if (!FindFrameCorrespondences(nFrame1, nFrame2, frame1Obs, frame1Pts, frame2Obs, frame2Pts))
	{
		printf("TwoFrameHomography: Failed to find correspondences between frames %d and %d\n", nFrame1, nFrame2);
		return false;
	}
		

	if (frame1Pts.size() < 4)
	{
		// While we could find *a* homography, it wouldn't be particularly meaningful
		printf("TwoFrameHomography: Too few points (%zu)\n", frame1Pts.size());
		return false;
	}

	// Undistort points

	// A reasonable camera matrix is expected for findHomography, which expects coordinates in units of pixels.
	// Outlier rejection seems to misbehave if provided normalized camera coordinates.
	cv::Mat optimalCameraMat = cv::getOptimalNewCameraMatrix(m_cameraMat, m_distCoefs, m_imageSize, 0);

	std::vector< cv::Point2f > frame1PtsUndistorted;
	std::vector< cv::Point2f > frame2PtsUndistorted;
	cv::undistortPoints(frame1Pts, frame1PtsUndistorted, m_cameraMat, m_distCoefs, cv::noArray(), optimalCameraMat);
	cv::undistortPoints(frame2Pts, frame2PtsUndistorted, m_cameraMat, m_distCoefs, cv::noArray(), optimalCameraMat);

	std::vector<uint8_t> inlierMask(frame1Pts.size(), 0);

	const double k_flRansacReprojThresholdPx = 10.0;
	const int k_nMaxIters = 2000;
	const double k_flConfidence = 0.995;

	homography = cv::findHomography(frame1PtsUndistorted, frame2PtsUndistorted, cv::RANSAC, k_flRansacReprojThresholdPx, inlierMask, k_nMaxIters, k_flConfidence);

	if (homography.empty())
	{
		printf("TwoFrameHomography: returned empty matrix\n");
		return false;
	}

	// Judge whether that was acceptable
	std::vector< cv::Point2f > transformedPoints;
	try
	{
		cv::perspectiveTransform(frame1PtsUndistorted, transformedPoints, homography);
	}
	catch (cv::Exception& e)
	{
		printf("Exception in cv::perspectiveTransform: %s\n", e.what());
		return false;
	}

	int nInliers = 0;
	for (int i = 0; i < frame1PtsUndistorted.size(); i++)
	{
		if (!inlierMask[i])
			continue;

		nInliers++;
	}
	printf("\n");

	const double k_flInlierThreshold = 0.75;
	double flInlierRatio = ((double)(nInliers)) / frame1Pts.size();

	if (flInlierRatio < k_flInlierThreshold)
	{
		printf("TwoFrameHomography: Not enough inliers (%d / %zu)\n", nInliers, frame1Pts.size());
		return false;
	}
		
	printf("TwoFrameHomography: Homography works (%d / %zu)\n", nInliers, frame1Pts.size());
	return true;
}

bool BundleAdjuster::TwoFramePlanarInit(int nFrame1, int nFrame2)
{
	std::vector< cv::Point2f > frame1Pts;
	std::vector< cv::Point2f > frame2Pts;

	std::vector< const PointObservation_t* > frame1Obs;
	std::vector< const PointObservation_t* > frame2Obs;

	if (!FindFrameCorrespondences(nFrame1, nFrame2, frame1Obs, frame1Pts, frame2Obs, frame2Pts))
	{
		return false;
	}
		

	printf("Init with: frame%d %zu frame%d %zu\n", nFrame1, frame1Obs.size(), nFrame2, frame2Obs.size());


	// Unproject frame 1's points to 1 meter in front of the camera.
	std::vector< cv::Point2f > frame1PtsXY;
	cv::undistortPoints(frame1Pts, frame1PtsXY, m_cameraMat, m_distCoefs);

	// ============== Initialize parts of the problem as best we can ================

	// Initialize both camera poses as identity
	m_cameraFromWorldPoses[nFrame1][0] = 0.0;
	m_cameraFromWorldPoses[nFrame1][1] = 0.0;
	m_cameraFromWorldPoses[nFrame1][2] = 0.0;
	m_cameraFromWorldPoses[nFrame1][3] = 0.0;
	m_cameraFromWorldPoses[nFrame1][4] = 0.0;
	m_cameraFromWorldPoses[nFrame1][5] = 0.0;

	m_cameraFromWorldPoses[nFrame2][0] = 0.0;
	m_cameraFromWorldPoses[nFrame2][1] = 0.0;
	m_cameraFromWorldPoses[nFrame2][2] = 0.0;
	m_cameraFromWorldPoses[nFrame2][3] = 0.0;
	m_cameraFromWorldPoses[nFrame2][4] = 0.0;
	m_cameraFromWorldPoses[nFrame2][5] = 0.0;

	// Mark frame 1 as the origin frame
	m_nOriginFrameId = nFrame1;
	printf("m_nOriginFrameId = %d\n", nFrame1);

	// Set point locations for all points
	for (int i = 0; i < frame1Obs.size(); i++)
	{
		int nLedIdx = frame1Obs[i]->nLedIdx;

		m_ledPoints[nLedIdx][0] = frame1PtsXY[i].x;
		m_ledPoints[nLedIdx][1] = frame1PtsXY[i].y;
		m_ledPoints[nLedIdx][2] = 1.0;

		printf("LED %d: %f %f %f\n", frame1Obs[i]->nLedIdx, m_ledPoints[frame1Obs[i]->nLedIdx][0], m_ledPoints[frame1Obs[i]->nLedIdx][1], m_ledPoints[frame1Obs[i]->nLedIdx][2]);

		m_pointsInitialized[nLedIdx] = true;
	}

	return true;
}

bool BundleAdjuster::InitCameraPosePnP(int nFrameIdx)
{
	std::vector< cv::Point3d > mapPoints;
	std::vector< cv::Point2d > imgPoints;

	for (const PointObservation_t& obs : (*m_pObservations)[nFrameIdx])
	{
		// Does this point have a label?
		if (obs.nLedIdx < 0)
			continue;

		// Is this a point in the current map?
		if (!m_pointsInitialized[obs.nLedIdx])
			continue;

		cv::Point3d mapPt(m_ledPoints[obs.nLedIdx][0], m_ledPoints[obs.nLedIdx][1], m_ledPoints[obs.nLedIdx][2]);
		mapPoints.push_back(mapPt);
		imgPoints.push_back(obs.p);
	}

	if (mapPoints.size() < k_nMinObservationsForPnPInit)
	{
		//printf("InitCameraPosePnP: Not enough map points observed (%zu, required %d)\n", mapPoints.size(), k_nMinObservationsForPnPInit);
		return false;
	}

	cv::Mat rCameraFromWorld, tCameraFromWorld;
	std::vector<int> inliers;
	bool bSuccess = cv::solvePnPRansac(mapPoints, imgPoints, m_cameraMat, m_distCoefs, rCameraFromWorld, tCameraFromWorld, false, 100, 8.0, 0.99, inliers, cv::SOLVEPNP_EPNP);

	if (!bSuccess)
	{
		//printf("InitCameraPosePnP: PnP solve for frame %d failed\n", nFrameIdx);
		return false;
	}

	// Sanity check: Most input points need to be inliners
	if (inliers.size() < mapPoints.size() * k_flMinPnPInlierFrac)
	{
		return false;
	}

	// Sanity check: At least k_nMinObservationsForPnPInit need to be inliers
	if (inliers.size() < k_nMinObservationsForPnPInit)
	{
		return false;
	}

	// Initialize camera
	m_cameraFromWorldPoses[nFrameIdx][0] = rCameraFromWorld.at<double>(0);
	m_cameraFromWorldPoses[nFrameIdx][1] = rCameraFromWorld.at<double>(1);
	m_cameraFromWorldPoses[nFrameIdx][2] = rCameraFromWorld.at<double>(2);

	m_cameraFromWorldPoses[nFrameIdx][3] = tCameraFromWorld.at<double>(0);
	m_cameraFromWorldPoses[nFrameIdx][4] = tCameraFromWorld.at<double>(1);
	m_cameraFromWorldPoses[nFrameIdx][5] = tCameraFromWorld.at<double>(2);

	// Sanity check: All points in the map that this camera claims to observe need to be in front of the camera
	double pInCamera[3];
	for (const PointObservation_t& obs : (*m_pObservations)[nFrameIdx])
	{
		// Is this a point in the current map?
		if (!m_pointsInitialized[obs.nLedIdx])
			continue;

		const double* const pInWorld = m_ledPoints[obs.nLedIdx];

		ceres::AngleAxisRotatePoint(m_cameraFromWorldPoses[nFrameIdx].data(), pInWorld, pInCamera);
		pInCamera[0] += m_cameraFromWorldPoses[nFrameIdx][3];
		pInCamera[1] += m_cameraFromWorldPoses[nFrameIdx][4];
		pInCamera[2] += m_cameraFromWorldPoses[nFrameIdx][5];

		if (pInCamera[2] <= 0)
		{
			m_cameraFromWorldPoses.erase(nFrameIdx);
			return false;
		}
	}

	return true;
}

int BundleAdjuster::InitializeNewCameras()
{
	CScopedStageTimer timer(&m_stageTimes, "pnp");

	int nFramesAdded = 0;

	// Attempt to initialize every camera frame that is not already in the map.
	for (int nFrameIdx = 0; nFrameIdx < m_pObservations->size(); nFrameIdx++)
	{
		if (m_cameraFromWorldPoses.find(nFrameIdx) != m_cameraFromWorldPoses.end())
			continue;

		if (m_rejectedFrames.count(nFrameIdx))
			continue;

		bool bSuccess = InitCameraPosePnP(nFrameIdx);

		if (bSuccess)
		{
			nFramesAdded++;
		}
	}

	printf("InitializeNewCameras: Added %d viewpoints\n", nFramesAdded);

	return nFramesAdded;
}

int BundleAdjuster::TriangulateNewPoints()
{
	CScopedStageTimer timer(&m_stageTimes, "triangulate");

	// Posed frames observing each uninitialized LED, and where.
	std::unordered_map<int, std::vector< int > > ledTrackedFrames;
	std::unordered_map<int, std::vector< cv::Point2f> > ledPoints;

	for (auto& kv : m_cameraFromWorldPoses)
	{
		int nFrameIdx = kv.first;

		for (const PointObservation_t& obs : (*m_pObservations)[nFrameIdx])
		{
			if (m_pointsInitialized[obs.nLedIdx])
				continue;

			ledTrackedFrames[obs.nLedIdx].push_back(nFrameIdx);
			ledPoints[obs.nLedIdx].push_back(obs.p);
		}
	}

	int nPointsTriangulated = 0;

	for (const auto& kv : ledTrackedFrames)
	{
		const int nLedIdx = kv.first;
		const std::vector<int>& frames = kv.second;

		if (frames.size() < 2)
			continue;

		// Linear (DLT) triangulation over every observing frame, in normalized camera coordinates.
		std::vector<cv::Point2f> pUndistorted;
		cv::undistortPoints(ledPoints[nLedIdx], pUndistorted, m_cameraMat, m_distCoefs);

		cv::Mat A((int)frames.size() * 2, 4, CV_64F);
		for (int i = 0; i < (int)frames.size(); i++)
		{
			cv::Mat camFromWorld;
			composeRT(m_cameraFromWorldPoses[frames[i]].data(), camFromWorld);
			camFromWorld.convertTo(camFromWorld, CV_64F);

			cv::Mat(pUndistorted[i].x * camFromWorld.row(2) - camFromWorld.row(0)).copyTo(A.row(2 * i));
			cv::Mat(pUndistorted[i].y * camFromWorld.row(2) - camFromWorld.row(1)).copyTo(A.row(2 * i + 1));
		}

		cv::Mat pInWorldHomogenous;
		cv::SVD::solveZ(A, pInWorldHomogenous);

		double flW = pInWorldHomogenous.at<double>(3);
		if (fabs(flW) < 1e-12)
			continue;

		m_ledPoints[nLedIdx][0] = pInWorldHomogenous.at<double>(0) / flW;
		m_ledPoints[nLedIdx][1] = pInWorldHomogenous.at<double>(1) / flW;
		m_ledPoints[nLedIdx][2] = pInWorldHomogenous.at<double>(2) / flW;
		m_pointsInitialized[nLedIdx] = true;

		if (!TestLedInFrontOfAllObservingCameras(nLedIdx))
		{
			m_pointsInitialized[nLedIdx] = false;
			continue;
		}

		nPointsTriangulated++;
	}

	return nPointsTriangulated;
}

int BundleAdjuster::InterpolateNewPoints()
{
	// Initialize points via linear interpolation across gaps.  
	// These LED strands are sequentially numbered with uniform spacing.

	int nLastInitializedLedIdx = 0;
	int nNewPoints = 0;

	// Find first LED
	for (int i = 0; i < k_nMaxLeds; i++)
	{
		if (m_pointsInitialized[i])
		{
			nLastInitializedLedIdx = i;
			break;
		}
	}

	// Find gaps
	bool bGap = false;
	for (int i = nLastInitializedLedIdx; i < k_nMaxLeds; i++)
	{
		if (bGap)
		{
			if (m_pointsInitialized[i])
			{
				int nDt = i - nLastInitializedLedIdx;
				double flDx = (m_ledPoints[i][0] - m_ledPoints[nLastInitializedLedIdx][0]) / nDt;
				double flDy = (m_ledPoints[i][1] - m_ledPoints[nLastInitializedLedIdx][1]) / nDt;
				double flDz = (m_ledPoints[i][2] - m_ledPoints[nLastInitializedLedIdx][2]) / nDt;

				for (int j = 1; j < i - nLastInitializedLedIdx; j++)
				{
					int k = nLastInitializedLedIdx + j;
					m_ledPoints[k][0] = m_ledPoints[nLastInitializedLedIdx][0] + flDx * j;
					m_ledPoints[k][1] = m_ledPoints[nLastInitializedLedIdx][1] + flDy * j;
					m_ledPoints[k][2] = m_ledPoints[nLastInitializedLedIdx][2] + flDz * j;
					m_pointsInitialized[k] = true;

					printf("Linear interp init LED %d\n", k);

					nNewPoints++;
				}
			}
		}

		bGap = !m_pointsInitialized[i];

		if (m_pointsInitialized[i])
		{
			nLastInitializedLedIdx = i;
		}
	}

	printf("InterpolateNewPoints: Added %d new LEDs\n", nNewPoints);
	return nNewPoints;
}

const PointObservation_t* BundleAdjuster::FindObservation(int nFrameIdx, int nLedIdx) const
{
	const std::vector<PointObservation_t>& observations = (*m_pObservations)[nFrameIdx];
	std::vector<PointObservation_t>::const_iterator it = std::lower_bound(observations.begin(), observations.end(), nLedIdx,
		[](const PointObservation_t& obs, int nIdx) { return obs.nLedIdx < nIdx; });

	return (it != observations.end() && it->nLedIdx == nLedIdx) ? &*it : nullptr;
}

bool BundleAdjuster::TestLedInFrontOfAllObservingCameras(int nLedIdx)
{
	if (!m_pointsInitialized[nLedIdx])
		return false;

	const double* const pInWorld = m_ledPoints[nLedIdx];

	bool bLedObserved = false;

	double pInCamera[3];
	for (auto& kv : m_cameraFromWorldPoses)
	{
		int nFrameIdx = kv.first;
		std::array<double, 6>& cameraFromWorld = kv.second;

		if (!FindObservation(nFrameIdx, nLedIdx))
			continue;

		bLedObserved = true;

		ceres::AngleAxisRotatePoint(cameraFromWorld.data(), pInWorld, pInCamera);
		pInCamera[0] += cameraFromWorld[3];
		pInCamera[1] += cameraFromWorld[4];
		pInCamera[2] += cameraFromWorld[5];

		if (pInCamera[2] <= 0)
			return false;
	}

	// Return false if nothing saw the LED at all
	if (!bLedObserved)
		return false;

	return true;
}

int BundleAdjuster::CullInvalidPoints()
{
	CScopedStageTimer timer(&m_stageTimes, "cull");

	int nLedsRemoved = 0;

	double pInCamera[3];
	for (auto& kv : m_cameraFromWorldPoses)
	{
		int nFrameIdx = kv.first;
		std::array<double, 6>& cameraFromWorld = kv.second;

		for (const PointObservation_t& obs : (*m_pObservations)[nFrameIdx] )
		{
			if (!m_pointsInitialized[obs.nLedIdx])
				continue;

			const double* const pInWorld = m_ledPoints[obs.nLedIdx];

			ceres::AngleAxisRotatePoint(cameraFromWorld.data(), pInWorld, pInCamera);
			pInCamera[0] += cameraFromWorld[3];
			pInCamera[1] += cameraFromWorld[4];
			pInCamera[2] += cameraFromWorld[5];

			if (pInCamera[2] <= 0)
			{
				m_pointsInitialized[obs.nLedIdx] = false;
				nLedsRemoved++;
			}
		}
	}

	return nLedsRemoved;
}

double BundleAdjuster::ComputeReprojectionError(const std::array<double, 6>& cameraFromWorld, const PointObservation_t& obs) const
{
	LedObservationResidual residual(obs.p.x, obs.p.y);
	double flResidual[2];
	if (!residual(cameraFromWorld.data(), m_intrinsics, m_ledPoints[obs.nLedIdx], flResidual))
		return std::numeric_limits<double>::infinity();

	return std::hypot(flResidual[0], flResidual[1]);
}

int BundleAdjuster::CullReprojectionOutliers()
{
	CScopedStageTimer timer(&m_stageTimes, "cull");

	int nCamerasRemoved = 0;
	for (auto it = m_cameraFromWorldPoses.begin(); it != m_cameraFromWorldPoses.end(); )
	{
		const int nFrameIdx = it->first;

		std::vector<double> errors;
		for (const PointObservation_t& obs : (*m_pObservations)[nFrameIdx])
		{
			if (m_pointsInitialized[obs.nLedIdx])
				errors.push_back(ComputeReprojectionError(it->second, obs));
		}

		if (nFrameIdx == m_nOriginFrameId || errors.empty() || Median(errors) <= k_flMaxReprojectionErrorPx)
		{
			++it;
			continue;
		}

		printf("CullReprojectionOutliers: Removing frame %d (median error %.1f px)\n", nFrameIdx, Median(errors));
		m_rejectedFrames.insert(nFrameIdx);
		it = m_cameraFromWorldPoses.erase(it);
		nCamerasRemoved++;
	}

	int nObservationsRemoved = 0;
	for (const auto& kv : m_cameraFromWorldPoses)
	{
		const std::array<double, 6>& cameraFromWorld = kv.second;
		std::vector<PointObservation_t>& observations = (*m_pObservations)[kv.first];

		size_t nBefore = observations.size();
		observations.erase(std::remove_if(observations.begin(), observations.end(), [&](const PointObservation_t& obs)
		{
			return m_pointsInitialized[obs.nLedIdx] && ComputeReprojectionError(cameraFromWorld, obs) > k_flMaxReprojectionErrorPx;
		}), observations.end());

		nObservationsRemoved += (int)(nBefore - observations.size());
	}

	std::vector<int> observationCounts(k_nMaxLeds, 0);
	for (const auto& kv : m_cameraFromWorldPoses)
	{
		for (const PointObservation_t& obs : (*m_pObservations)[kv.first])
		{
			observationCounts[obs.nLedIdx]++;
		}
	}

	int nLedsRemoved = 0;
	for (int i = 0; i < k_nMaxLeds; i++)
	{
		if (!m_pointsInitialized[i] || observationCounts[i] >= 2)
			continue;

		m_pointsInitialized[i] = false;
		nLedsRemoved++;
	}

	printf("CullReprojectionOutliers: Removed %d cameras, %d observations, %d LEDs\n", nCamerasRemoved, nObservationsRemoved, nLedsRemoved);
	return nLedsRemoved;
}

bool BundleAdjuster::SetupBundleAdjustmentProblem(ceres::Problem& problem, bool bSolveIntrinsics, bool bSolvePoses, bool bSolvePoints)
{
	CScopedStageTimer timer(&m_stageTimes, "problem setup");

	if (m_nOriginFrameId < 0)
	{
		printf("SetupBundleAdjustmentProblem: Problem must be initialized first\n");
		return false;
	}

	for (auto& poseKV : m_cameraFromWorldPoses)
	{
		int nFrameIdx = poseKV.first;
		std::array<double, 6>& pose = poseKV.second;

		const std::vector<PointObservation_t>& obsThisFrame = (*m_pObservations)[nFrameIdx];

		for (const PointObservation_t &obs : obsThisFrame)
		{
			// Points with initializations only
			if (!m_pointsInitialized[obs.nLedIdx])
				continue;

			ceres::CostFunction* cf = LedObservationResidual::Create(obs);
			problem.AddResidualBlock(cf, new ceres::CauchyLoss(2.0), pose.data(), m_intrinsics, m_ledPoints[obs.nLedIdx]);

			if (!bSolvePoints)
			{
				problem.SetParameterBlockConstant(m_ledPoints[obs.nLedIdx]);
			}
		}

		if (!bSolvePoses)
		{
			problem.SetParameterBlockConstant(pose.data());
		}
	}

	if (problem.NumResiduals() <= 0)
	{
		printf("SetupBundleAdjustmentProblem: Error: Empty problem\n");
		return false;
	}

	// Confirm the origin frame is in the problem
	std::vector< ceres::ResidualBlockId > residualBlocksForOriginCamera;
	problem.GetResidualBlocksForParameterBlock(m_cameraFromWorldPoses[m_nOriginFrameId].data(), &residualBlocksForOriginCamera);
	if (residualBlocksForOriginCamera.size() <= 0)
	{
		printf("SetupBundleAdjustmentProblem: Error: Origin is not in the problem\n");
		return false;
	}

	// The camera at the origin defines the coordinate system, and is always locked
	problem.SetParameterBlockConstant(m_cameraFromWorldPoses[m_nOriginFrameId].data());

	if (!bSolveIntrinsics)
	{
		problem.SetParameterBlockConstant(m_intrinsics);
	}

	return true;
}

bool BundleAdjuster::SolveProblem(ceres::Problem& problem)
{
	CScopedStageTimer timer(&m_stageTimes, "solve");

	ceres::Solver::Options options;

	// Parallel processing: Use a number of threads appropriate for the machine and the problem
	int k_nMinimumResidualsPerThread = 16;
	int nMachineThreads = (m_nThreads > 0) ? m_nThreads : (int)std::thread::hardware_concurrency();
	if (nMachineThreads <= 0)
		nMachineThreads = 8;  // detection failed

	options.num_threads = std::clamp(problem.NumResidualBlocks() / k_nMinimumResidualsPerThread, 1, nMachineThreads);

	options.linear_solver_type = ceres::LinearSolverType::SPARSE_SCHUR;
	options.trust_region_strategy_type = ceres::TrustRegionStrategyType::LEVENBERG_MARQUARDT;
	options.use_nonmonotonic_steps = true;

	options.max_num_iterations = 200;

	options.minimizer_progress_to_stdout = true;

	// TBD: This is a bundle adjustment problem - we may be able to save a bit of preprocessing time by specifying
	//		an explicit linear_solver_ordering that splits all points and all cameras into separate groups.

	// TBD: Convergence tuning...

	// Visualizer callback
	VisualizerCallback visualizerCallback(this, &m_visualizer);
	options.callbacks.push_back(&visualizerCallback);
	options.update_state_every_iteration = true;

	ceres::Solver::Summary summary;
	ceres::Solve(options, &problem, &summary);

	printf("%s\n", summary.BriefReport().c_str());

	// Reprojection RMS without the robust loss, for comparing runs
	ceres::Problem::EvaluateOptions evalOptions;
	evalOptions.apply_loss_function = false;
	double flCost = 0.0;
	if (problem.Evaluate(evalOptions, &flCost, nullptr, nullptr, nullptr))
	{
		printf("Reprojection RMS: %f px over %d observations\n", std::sqrt(2.0 * flCost / problem.NumResidualBlocks()), problem.NumResidualBlocks());
	}

	// Update the OpenCV float copy of the camera intrinsics, in case this solve changed them.  
	m_cameraMat.at<float>(0, 0) = (float)(m_intrinsics[LedObservationResidual::eFx]);
	m_cameraMat.at<float>(1, 1) = (float)(m_intrinsics[LedObservationResidual::eFy]);
	m_cameraMat.at<float>(0, 2) = (float)(m_intrinsics[LedObservationResidual::eCx]);
	m_cameraMat.at<float>(1, 2) = (float)(m_intrinsics[LedObservationResidual::eCy]);

	m_distCoefs.at<float>(0) = (float)(m_intrinsics[LedObservationResidual::eK1]);
	m_distCoefs.at<float>(1) = (float)(m_intrinsics[LedObservationResidual::eK2]);
	m_distCoefs.at<float>(2) = (float)(m_intrinsics[LedObservationResidual::eP1]);
	m_distCoefs.at<float>(3) = (float)(m_intrinsics[LedObservationResidual::eP2]);
	m_distCoefs.at<float>(4) = (float)(m_intrinsics[LedObservationResidual::eK3]);

	return summary.IsSolutionUsable();
}

bool BundleAdjuster::FindAlignmentTransform()
{
	ceres::Problem problem;

	for (int i = 0; i < k_nMaxLeds; i++)
	{
		if (!m_pointsInitialized[i])
			continue;

		ceres::CostFunction* cf = AlignmentResidual::Create(m_ledPoints[i]);

		// Use robust loss - there may be outlier points in the reconstruction
		problem.AddResidualBlock(cf, new ceres::CauchyLoss(0.5), m_alignedFromWorld);
	}

	ceres::Solver::Options options;
	options.num_threads = 1;
	options.linear_solver_type = ceres::LinearSolverType::SPARSE_NORMAL_CHOLESKY;
	options.trust_region_strategy_type = ceres::TrustRegionStrategyType::LEVENBERG_MARQUARDT;
	options.use_nonmonotonic_steps = true;
	options.max_num_iterations = 100;
	options.minimizer_progress_to_stdout = false;

	ceres::Solver::Summary summary;
	ceres::Solve(options, &problem, &summary);
	printf("Alignment solve result: %s\n", summary.BriefReport().c_str());
	printf("Alignment transform:\n\tR: %0.06f %0.06f %0.06f\n\tT: %0.06f %0.06f %0.06f\n",
		m_alignedFromWorld[0], m_alignedFromWorld[1], m_alignedFromWorld[2], 
		m_alignedFromWorld[3], m_alignedFromWorld[4], m_alignedFromWorld[5]);

	if (!summary.IsSolutionUsable())
		return false;

	// TBD: Tree might be upside down.
	//		Figure out which end of the tree is pointing up.

	return true;
}

bool BundleAdjuster::WriteSolutionToJson(std::string& outPath)
{
	Json::Value outJson;

	outJson["leds"] = Json::Value(Json::arrayValue);

	for (int i = 0; i < k_nMaxLeds; i++)
	{
		Json::Value ledEntry(Json::arrayValue);
		
		if (m_pointsInitialized[i])
		{
			ledEntry.append(m_ledPoints[i][0]);
			ledEntry.append(m_ledPoints[i][1]);
			ledEntry.append(m_ledPoints[i][2]);
		}
		else
		{
			ledEntry.append(NAN);
			ledEntry.append(NAN);
			ledEntry.append(NAN);
		}

		outJson["leds"].append(ledEntry);
	}
	
	outJson["poses"] = Json::Value();
	for (const auto& kv : m_cameraFromWorldPoses)
	{
		const int nFrameIdx = kv.first;
		const std::array<double, 6> &cameraFromWorld = kv.second;

		Json::Value cameraEntry(Json::arrayValue);
		for (int i = 0; i < 6; i++)
		{
			cameraEntry.append(cameraFromWorld[i]);
		}
		outJson["poses"][nFrameIdx] = cameraEntry;
	}

	Json::StreamWriterBuilder builder;
	builder["commentStyle"] = "None";
	builder["indentation"] = "  ";

	std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
	std::ofstream ofs(outPath);

	if (!ofs)
		return false;

	writer->write(outJson, &ofs);

	return true;
}

bool BundleAdjuster::WriteAlignedReconstructionToJson(const std::string& outPath)
{
	Json::Value outJson;

	outJson["leds"] = Json::Value(Json::arrayValue);

	double pAligned[3];

	for (int i = 0; i < k_nMaxLeds; i++)
	{
		// Apply alignment transform
		ceres::AngleAxisRotatePoint(m_alignedFromWorld, m_ledPoints[i], pAligned);
		pAligned[0] += m_alignedFromWorld[3];
		pAligned[1] += m_alignedFromWorld[4];
		pAligned[2] += m_alignedFromWorld[5];

		Json::Value ledEntry(Json::arrayValue);

		if (m_pointsInitialized[i])
		{
			ledEntry.append(pAligned[0]);
			ledEntry.append(pAligned[1]);
			ledEntry.append(pAligned[2]);
		}
		else
		{
			ledEntry.append(NAN);
			ledEntry.append(NAN);
			ledEntry.append(NAN);
		}

		outJson["leds"].append(ledEntry);
	}

	Json::StreamWriterBuilder builder;
	builder["commentStyle"] = "None";
	builder["indentation"] = "  ";

	std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
	std::ofstream ofs(outPath);

	if (!ofs)
		return false;

	writer->write(outJson, &ofs);

	return true;
}

void BundleAdjuster::composeRT(double* pose, cv::Mat& poseRT)
{
	cv::Mat rvec(3, 1, CV_64F, pose);
	cv::Mat tvec(3, 1, CV_64F, pose + 3);

	cv::Mat R;
	cv::Rodrigues(rvec, R);

	poseRT = cv::Mat::zeros(3, 4, CV_64F);

	cv::Rect roiR = cv::Rect(0, 0, 3, 3);
	cv::Rect roiT = cv::Rect(3, 0, 1, 3);

	cv::Mat poseR = poseRT(roiR);
	cv::Mat poseT = poseRT(roiT);

	R.copyTo(poseR);
	tvec.copyTo(poseT);

	poseRT.convertTo(poseRT, CV_32F);
}

VisualizerCallback::VisualizerCallback(BundleAdjuster* pBundleAdjuster, CPointsVisualizer* pVisualizer) 
	: m_pBundleAdjuster(pBundleAdjuster)
	, m_pVisualizer(pVisualizer)
{
}

VisualizerCallback::~VisualizerCallback()
{
}

ceres::CallbackReturnType VisualizerCallback::operator()(const ceres::IterationSummary& summary)
{
	// Update visualization every frame
	m_pVisualizer->OnDataUpdate(m_pBundleAdjuster->m_cameraFromWorldPoses, (double*)(m_pBundleAdjuster->m_ledPoints), m_pBundleAdjuster->m_pointsInitialized);
	return ceres::SOLVER_CONTINUE;
}
