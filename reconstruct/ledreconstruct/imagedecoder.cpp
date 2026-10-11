#include <algorithm>
#include <filesystem>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <opencv2/videoio.hpp>

#include "imagedecoder.h"
#include "ledconstants.h"
#include "stagetimes.h"

// Moving-sum width over per-frame mean intensity; also how far from either end a sync peak must be.
static constexpr int k_nSyncWindowFrames = 5;

// A sync peak must rise this fraction of the way from the median to the max of the moving sum.
static constexpr double k_flSyncPeakFrac = 0.5;

// Sync peaks are local maxima within this fraction of the expected cycle length on either side.
static constexpr double k_flSyncPeakSeparationFrac = 0.4;

// A pair of sync peaks bounds a usable cycle if its gap is within this fraction of the median gap.
static constexpr double k_flSyncGapTolerance = 0.05;

// Warn if the container fps is off by more than this fraction.
static constexpr double k_flFpsTolerance = 0.01;

// Warn if the measured cycle length is off by more than this; peaks are whole frames, so +-1 is rounding.
static constexpr double k_flCycleToleranceFrames = 1.5;

// Only scan this many cycles' worth of frames for sync; 2.5 always holds two whole pulses away from the ends.
static constexpr double k_flMaxScanCycles = 2.5;

// Smaller same-index blobs are noise.
static constexpr int k_nMinBlobPixels = 9;

// (8, 4, 4)_2 augmented Hadamard code: codeword bits MSB-first, indexed by nibble.
static constexpr uint8_t k_hadamardCodewords[16] =
{
	0x00, 0x55, 0x33, 0x66, 0x0F, 0x5A, 0x3C, 0x69,
	0xFF, 0xAA, 0xCC, 0x99, 0xF0, 0xA5, 0xC3, 0x96,
};

enum LedColor_t
{
	eRed = 0,
	eGreen = 1,
	eBlue = 2,
};


static std::string FileName( const std::string &path )
{
	return std::filesystem::path( path ).filename().string();
}


static double Median( std::vector< double > values )
{
	std::sort( values.begin(), values.end() );
	size_t nMid = values.size() / 2;
	return ( values.size() % 2 ) ? values[nMid] : 0.5 * ( values[nMid - 1] + values[nMid] );
}


static double ExpectedCycleFrames( double flFps )
{
	return k_nCycleSlots * k_nFramesPerSlot * flFps / k_flCaptureFps;
}


// Opponent-color argmax; ties resolve to the earlier of R, G, B.
static uint8_t ClassifyColor( const uint8_t *pBgr )
{
	uint8_t b = pBgr[0];
	uint8_t g = pBgr[1];
	uint8_t r = pBgr[2];

	if ( r >= g && r >= b )
		return eRed;

	return ( g >= b ) ? eGreen : eBlue;
}


// Per-pixel color classes of a frame, 2 bits each, 4 pixels per byte in row-major order.
using ColorMap_t = std::vector< uint8_t >;

static ColorMap_t ClassifyFrame( const cv::Mat &frame )
{
	ColorMap_t colors( ( frame.total() + 3 ) / 4, 0 );
	size_t i = 0;
	for ( int y = 0; y < frame.rows; y++ )
	{
		const uint8_t *pBgr = frame.ptr< uint8_t >( y );
		for ( int x = 0; x < frame.cols; x++, i++ )
		{
			colors[i >> 2] |= ClassifyColor( pBgr + 3 * x ) << ( ( i & 3 ) * 2 );
		}
	}

	return colors;
}


static uint8_t GetColor( const ColorMap_t &colors, size_t i )
{
	return ( colors[i >> 2] >> ( ( i & 3 ) * 2 ) ) & 3;
}


// One pass over up to k_flMaxScanCycles cycles: mean intensity (for sync) and color classes of every frame.
static bool ScanVideo( std::vector< double > *pIntensities, std::vector< ColorMap_t > *pColorMaps, cv::Size *pFrameSize, double *pflFps, const std::string &videoPath )
{
	cv::VideoCapture cap( videoPath );
	if ( !cap.isOpened() )
		return false;

	double flFps = cap.get( cv::CAP_PROP_FPS );
	if ( fabs( flFps - k_flCaptureFps ) > k_flFpsTolerance * k_flCaptureFps )
	{
		printf( "%s: warning: video is %.3f fps, ledcodes.glsl assumes %.2f\n", FileName( videoPath ).c_str(), flFps, k_flCaptureFps );
	}
	if ( flFps <= 1.0 )
	{
		flFps = k_flCaptureFps;
	}

	int nMaxFrames = ( int )ceil( k_flMaxScanCycles * ExpectedCycleFrames( flFps ) );
	cv::Mat frame;
	while ( ( int )pIntensities->size() < nMaxFrames && cap.read( frame ) )
	{
		cv::Scalar mean = cv::mean( frame );
		pIntensities->push_back( ( mean[0] + mean[1] + mean[2] ) / 3.0 );
		pColorMaps->push_back( ClassifyFrame( frame ) );
		*pFrameSize = frame.size();
	}

	*pflFps = flFps;
	return true;
}


// Frames at the centers of the white sync pulses.
static std::vector< int > FindSyncPeaks( const std::vector< double > &intensities, double flCycleFrames )
{
	int nFrames = ( int )intensities.size();
	int nHalfWindow = k_nSyncWindowFrames / 2;

	std::vector< double > movingSum( nFrames, 0.0 );
	for ( int i = 0; i < nFrames; i++ )
	{
		for ( int j = std::max( 0, i - nHalfWindow ); j <= std::min( nFrames - 1, i + nHalfWindow ); j++ )
		{
			movingSum[i] += intensities[j];
		}
	}

	std::vector< int > peaks;
	if ( nFrames == 0 )
		return peaks;

	double flMedian = Median( movingSum );
	double flMax = *std::max_element( movingSum.begin(), movingSum.end() );
	double flThreshold = flMedian + k_flSyncPeakFrac * ( flMax - flMedian );
	int nSeparation = ( int )( k_flSyncPeakSeparationFrac * flCycleFrames );

	for ( int i = k_nSyncWindowFrames + 1; i < nFrames - k_nSyncWindowFrames; i++ )
	{
		if ( movingSum[i] < flThreshold )
			continue;

		if ( !peaks.empty() && i - peaks.back() <= nSeparation )
			continue;

		int nFirst = std::max( 0, i - nSeparation );
		int nLast = std::min( nFrames - 1, i + nSeparation );
		if ( movingSum[i] < *std::max_element( movingSum.begin() + nFirst, movingSum.begin() + nLast + 1 ) )
			continue;

		peaks.push_back( i );
	}

	return peaks;
}


// First pair of consecutive sync peaks whose gap agrees with the median gap.
static bool FindSyncCycle( int *pnStart, int *pnEnd, const std::vector< int > &peaks, double flCycleFrames, const std::string &videoName )
{
	if ( peaks.size() < 2 )
	{
		printf( "%s: found %d sync pulse(s), need 2\n", videoName.c_str(), ( int )peaks.size() );
		return false;
	}

	std::vector< double > gaps;
	for ( size_t i = 1; i < peaks.size(); i++ )
	{
		gaps.push_back( peaks[i] - peaks[i - 1] );
	}

	double flMedianGap = Median( gaps );
	if ( fabs( flMedianGap - flCycleFrames ) > k_flCycleToleranceFrames )
	{
		printf( "%s: warning: sync cycle is %.1f frames, expected %.1f\n", videoName.c_str(), flMedianGap, flCycleFrames );
	}

	for ( size_t i = 1; i < peaks.size(); i++ )
	{
		if ( fabs( gaps[i - 1] - flMedianGap ) > k_flSyncGapTolerance * flMedianGap )
			continue;

		*pnStart = peaks[i - 1];
		*pnEnd = peaks[i];
		return true;
	}

	printf( "%s: no two sync pulses are a consistent cycle apart\n", videoName.c_str() );
	return false;
}


// Accumulates per-pixel transition bits over the color-slot centers of one cycle:
// R->G->B->R is a 1, the reverse a 0, no change an erasure (clears *pKnown).
static bool AccumulateCodes( cv::Mat *pCode, cv::Mat *pKnown, cv::Mat *pFirstColor, const std::vector< ColorMap_t > &colorMaps, cv::Size frameSize, int nSyncStart, int nSyncEnd )
{
	double flSlotFrames = ( nSyncEnd - nSyncStart ) / ( double )k_nCycleSlots;

	*pCode = cv::Mat::zeros( frameSize, CV_32SC1 );
	*pKnown = cv::Mat::ones( frameSize, CV_8UC1 );
	*pFirstColor = cv::Mat( frameSize, CV_8UC1 );

	uint32_t *pCodes = pCode->ptr< uint32_t >();
	uint8_t *pKnowns = pKnown->ptr< uint8_t >();
	uint8_t *pFirsts = pFirstColor->ptr< uint8_t >();
	size_t nPixels = frameSize.area();

	// Sync peak is the boundary between the two sync slots, so color slot k is centered k + 1.5 slots later.
	const ColorMap_t *pPrevColors = nullptr;
	for ( int k = 0; k < k_nColorSlots; k++ )
	{
		size_t nFrame = ( size_t )( nSyncStart + flSlotFrames * ( k + 1.5 ) + 0.5 );
		if ( nFrame >= colorMaps.size() )
			return false;

		const ColorMap_t &colors = colorMaps[nFrame];
		if ( !pPrevColors )
		{
			for ( size_t i = 0; i < nPixels; i++ )
			{
				pFirsts[i] = GetColor( colors, i );
			}

			pPrevColors = &colors;
			continue;
		}

		for ( size_t i = 0; i < nPixels; i++ )
		{
			int nStep = ( GetColor( colors, i ) - GetColor( *pPrevColors, i ) + 3 ) % 3;
			pKnowns[i] &= ( nStep != 0 );
			pCodes[i] = ( pCodes[i] << 1 ) | ( nStep == 1 );
		}

		pPrevColors = &colors;
	}

	return true;
}


static int DecodeNibble( uint8_t nCodeword )
{
	for ( int i = 0; i < 16; i++ )
	{
		if ( k_hadamardCodewords[i] == nCodeword )
			return i;
	}

	return -1;
}


// LED index, or -1. Only exact codewords are accepted: a speculative decode is worse than none.
static int DecodePixel( uint32_t nCode, uint8_t nFirstColor )
{
	int nLedIdx = 0;
	for ( int i = 0; i < 4; i++ )
	{
		int nNibble = DecodeNibble( ( uint8_t )( nCode >> ( 24 - 8 * i ) ) );
		if ( nNibble < 0 )
			return -1;

		nLedIdx = ( nLedIdx << 4 ) | nNibble;
	}

	if ( nLedIdx >= k_nMaxLeds )
		return -1;

	// ledcodes.glsl starts each LED's color cycle at idx % 3.
	if ( nLedIdx % 3 != nFirstColor )
		return -1;

	return nLedIdx;
}


// Largest 8-connected same-index blob per LED index. Consumes *pLabels.
static void FindBlobs( std::vector< PointObservation_t > *pObservations, int *pnDuplicateIdxs, cv::Mat *pLabels )
{
	std::vector< PointObservation_t > best( k_nMaxLeds );
	std::vector< int > blobCounts( k_nMaxLeds, 0 );
	std::vector< cv::Point > stack;

	for ( int y0 = 0; y0 < pLabels->rows; y0++ )
	{
		for ( int x0 = 0; x0 < pLabels->cols; x0++ )
		{
			int nLedIdx = pLabels->at< int >( y0, x0 );
			if ( nLedIdx < 0 )
				continue;

			double flSumX = 0.0;
			double flSumY = 0.0;
			int nPixels = 0;

			pLabels->at< int >( y0, x0 ) = -1;
			stack.push_back( cv::Point( x0, y0 ) );
			while ( !stack.empty() )
			{
				cv::Point p = stack.back();
				stack.pop_back();

				flSumX += p.x;
				flSumY += p.y;
				nPixels++;

				for ( int dy = -1; dy <= 1; dy++ )
				{
					for ( int dx = -1; dx <= 1; dx++ )
					{
						cv::Point q( p.x + dx, p.y + dy );
						if ( q.x < 0 || q.y < 0 || q.x >= pLabels->cols || q.y >= pLabels->rows )
							continue;

						int &nNeighbor = pLabels->at< int >( q );
						if ( nNeighbor != nLedIdx )
							continue;

						nNeighbor = -1;
						stack.push_back( q );
					}
				}
			}

			if ( nPixels < k_nMinBlobPixels )
				continue;

			blobCounts[nLedIdx]++;

			PointObservation_t &obs = best[nLedIdx];
			if ( nPixels <= obs.nPixels )
				continue;

			obs.p = cv::Point2f( ( float )( flSumX / nPixels ), ( float )( flSumY / nPixels ) );
			obs.nLedIdx = nLedIdx;
			obs.nPixels = nPixels;
		}
	}

	*pnDuplicateIdxs = 0;
	for ( int i = 0; i < k_nMaxLeds; i++ )
	{
		if ( blobCounts[i] == 0 )
			continue;

		if ( blobCounts[i] > 1 )
		{
			( *pnDuplicateIdxs )++;
		}

		pObservations->push_back( best[i] );
	}
}


bool DecodeVideo( std::vector< PointObservation_t > *pObservations, const std::string &videoPath )
{
	std::string videoName = FileName( videoPath );

	StageClock_t::time_point scanStart = StageClock_t::now();
	std::vector< double > intensities;
	std::vector< ColorMap_t > colorMaps;
	cv::Size frameSize;
	double flFps = 0.0;
	if ( !ScanVideo( &intensities, &colorMaps, &frameSize, &flFps, videoPath ) )
	{
		printf( "%s: can't open video\n", videoName.c_str() );
		return false;
	}

	double flCycleFrames = ExpectedCycleFrames( flFps );
	std::vector< int > peaks = FindSyncPeaks( intensities, flCycleFrames );

	int nSyncStart = 0;
	int nSyncEnd = 0;
	if ( !FindSyncCycle( &nSyncStart, &nSyncEnd, peaks, flCycleFrames, videoName ) )
		return false;

	double flScanSeconds = SecondsSince( scanStart );

	StageClock_t::time_point labelStart = StageClock_t::now();
	cv::Mat code;
	cv::Mat known;
	cv::Mat firstColor;
	if ( !AccumulateCodes( &code, &known, &firstColor, colorMaps, frameSize, nSyncStart, nSyncEnd ) )
	{
		printf( "%s: video ended inside the code cycle\n", videoName.c_str() );
		return false;
	}

	colorMaps.clear();

	cv::Mat labels( code.size(), CV_32SC1, cv::Scalar( -1 ) );
	for ( int y = 0; y < code.rows; y++ )
	{
		const uint32_t *pCodeRow = code.ptr< uint32_t >( y );
		const uint8_t *pKnownRow = known.ptr< uint8_t >( y );
		const uint8_t *pFirstRow = firstColor.ptr< uint8_t >( y );
		int *pLabelRow = labels.ptr< int >( y );

		for ( int x = 0; x < code.cols; x++ )
		{
			if ( !pKnownRow[x] )
				continue;

			pLabelRow[x] = DecodePixel( pCodeRow[x], pFirstRow[x] );
		}
	}

	int nDuplicateIdxs = 0;
	FindBlobs( pObservations, &nDuplicateIdxs, &labels );

	printf( "%s: %d LEDs (sync frames %d-%d, %d indices with duplicate blobs; scan %d frames %.2f s, label %.2f s)\n",
		videoName.c_str(), ( int )pObservations->size(), nSyncStart, nSyncEnd, nDuplicateIdxs, ( int )intensities.size(),
		flScanSeconds, SecondsSince( labelStart ) );
	return true;
}
