#include <algorithm>
#include <filesystem>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "imagedecoder.h"
#include "ledconstants.h"
#include "stagetimes.h"

// Sync flash params

// Width of the moving average window used for sync-flash extraction
static constexpr int k_nSyncWindowFrames = 5;

// How bright a sync flash needs to be wrt. the rest of the sequence
static constexpr double k_flSyncPeakFrac = 0.5;

// Minimum separation between sync flashes, wrt cycle period
static constexpr double k_flSyncPeakSeparationFrac = 0.4;

// Maximum deviation of sync period vs expected cycle period
static constexpr double k_flSyncGapTolerance = 0.05;

// Warning thresholds for bad sync and/or camera fps disagreement with LED sequence
static constexpr double k_flFpsTolerance = 0.01;
static constexpr double k_flCycleToleranceFrames = 1.5;

// Max duration to scan for sync frames
static constexpr double k_flMaxScanCycles = 2.5;

// Minimum blob area in pixels for observations
static constexpr int k_nMinBlobPixels = 9;

// (8, 4, 4)_2 augmented Hadamard code
// maps 4 input bits -> 8 output bits
static constexpr uint8_t k_hadamardCodewords[16] =
{
    0b00000000, 0b01010101, 0b00110011, 0b01100110,
    0b00001111, 0b01011010, 0b00111100, 0b01101001,
    0b11111111, 0b10101010, 0b11001100, 0b10011001,
    0b11110000, 0b10100101, 0b11000011, 0b10010110,
};

enum LedColor_t
{
	eRed = 0,
	eGreen = 1,
	eBlue = 2,
};

// Per-pixel labels for decoding failures
static constexpr int k_nLabelErasure = -1;
static constexpr int k_nLabelBadCodeword = -2;
static constexpr int k_nLabelBadIndex = -3;


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


static uint8_t ClassifyColor( const uint8_t *pBgr )
{
	uint8_t b = pBgr[0];
	uint8_t g = pBgr[1];
	uint8_t r = pBgr[2];

	if ( r >= g && r >= b )
		return eRed;

	return ( g >= b ) ? eGreen : eBlue;
}


// Color labels for a frame. Packed for memory reduction.
// Labels are 2 bits per pixel / 4 pixels per byte, row major addressing
// See GetColor(...) for unpacking
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

// Unpacks ColorMap_t at the specified pixel index
static uint8_t GetColor( const ColorMap_t &colors, size_t i )
{
	return ( colors[i >> 2] >> ( ( i & 3 ) * 2 ) ) & 3;
}


// Scans through a video:
// - Extracts per frame average intensity (for sync extraction)
// - Performs per-pixel color classification
static bool ScanVideo( std::vector< double > *pIntensities, std::vector< ColorMap_t > *pColorMaps, cv::Size *pFrameSize, double *pflFps, cv::Mat *pBrightest, const std::string &videoPath )
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
	double flBrightest = -1.0;
	while ( ( int )pIntensities->size() < nMaxFrames && cap.read( frame ) )
	{
		cv::Scalar mean = cv::mean( frame );
		double flIntensity = ( mean[0] + mean[1] + mean[2] ) / 3.0;
		pIntensities->push_back( flIntensity );
		if ( pBrightest && flIntensity > flBrightest )
		{
			flBrightest = flIntensity;
			cv::cvtColor( frame, *pBrightest, cv::COLOR_BGR2GRAY );
		}

		pColorMaps->push_back( ClassifyFrame( frame ) );
		*pFrameSize = frame.size();
	}

	*pflFps = flFps;
	return true;
}


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


// Accumulates per-pixel codewords via inspecting colors over one cycle:
// - Transitions R->G->B->R are 1
// - Transitions R->B->G->R are 0
// - Missing changes count as erasures
// Note - camera is rolling shutter & LED updates are async; exact timing of transitions can vary
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
	std::vector< uint8_t > prevColors( nPixels );

	for ( int k = 0; k < k_nColorSlots; k++ )
	{
		size_t nFrame = ( size_t )( nSyncStart + flSlotFrames * ( k + 1.5 ) + 0.5 );
		if ( nFrame + 1 >= colorMaps.size() )
			return false;

		const ColorMap_t &before = colorMaps[nFrame - 1];
		const ColorMap_t &center = colorMaps[nFrame];
		const ColorMap_t &after = colorMaps[nFrame + 1];
		if ( k == 0 )
		{
			for ( size_t i = 0; i < nPixels; i++ )
			{
				pFirsts[i] = prevColors[i] = GetColor( center, i );
			}

			continue;
		}

		for ( size_t i = 0; i < nPixels; i++ )
		{
			uint8_t nPrev = prevColors[i];
			uint8_t nColor = GetColor( before, i );
			if ( nColor == nPrev )
			{
				nColor = GetColor( center, i );
			}
			if ( nColor == nPrev )
			{
				nColor = GetColor( after, i );
			}

			int nStep = ( nColor - nPrev + 3 ) % 3;
			pKnowns[i] &= ( nStep != 0 );
			pCodes[i] = ( pCodes[i] << 1 ) | ( nStep == 1 );
			prevColors[i] = nColor;
		}
	}

	return true;
}


static int DecodeNibble( uint8_t nCodeword )
{
	// Exact matches only.
	// We could try to recover lost bits, but for this application, corrupted codewords usually mean there's some kind 
	// of occlusion or other problem that makes the LED detection in image space unreliable for reconstruction. 
	// We're probably leaving some detections on the floor, but I'm fine with that. 
	for ( int i = 0; i < 16; i++ )
	{
		if ( k_hadamardCodewords[i] == nCodeword )
			return i;
	}

	return -1;
}


static int DecodePixel( uint32_t nCode, uint8_t nFirstColor )
{
	int nLedIdx = 0;
	for ( int i = 0; i < 4; i++ )
	{
		int nNibble = DecodeNibble( ( uint8_t )( nCode >> ( 24 - 8 * i ) ) );
		if ( nNibble < 0 )
			return k_nLabelBadCodeword;

		nLedIdx = ( nLedIdx << 4 ) | nNibble;
	}

	if ( nLedIdx >= k_nMaxLeds )
		return k_nLabelBadIndex;

	// As an additional check, confirm 
	if ( nLedIdx % 3 != nFirstColor )
		return k_nLabelBadIndex;

	return nLedIdx;
}


// Connected components - Finds 8-connected blobs of pixels with the same decoded LED index. 
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

			// Keep largest observation only
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

		// Count LEDs with duplicate detections (for logging)
		if ( blobCounts[i] > 1 )
		{
			( *pnDuplicateIdxs )++;
		}

		pObservations->push_back( best[i] );
	}
}


// Decoding debug image
static void RenderDecodeImage( const cv::Mat &labels, const cv::Mat &brightest, const std::vector< PointObservation_t > &observations, const std::string &path )
{
	// LED colors
	// Hue separated by golden ratio s.t. neighboring LEDs on the same string have clear contrast
	cv::Mat ledColors( 1, k_nMaxLeds, CV_8UC3 );
	for ( int i = 0; i < k_nMaxLeds; i++ )
	{
		double flHue = fmod( i * 0.6180339887, 1.0 );
		ledColors.at< cv::Vec3b >( i ) = cv::Vec3b( ( uint8_t )( flHue * 180.0 ), 255, 255 );
	}
	cv::cvtColor( ledColors, ledColors, cv::COLOR_HSV2BGR );

	cv::Mat image( labels.size(), CV_8UC3 );
	for ( int y = 0; y < labels.rows; y++ )
	{
		const int *pLabelRow = labels.ptr< int >( y );
		const uint8_t *pGrayRow = brightest.ptr< uint8_t >( y );
		cv::Vec3b *pOutRow = image.ptr< cv::Vec3b >( y );

		for ( int x = 0; x < labels.cols; x++ )
		{
			int nLabel = pLabelRow[x];
			if ( nLabel >= 0 )
			{
				pOutRow[x] = ledColors.at< cv::Vec3b >( nLabel );
				continue;
			}

			uint8_t nGray = pGrayRow[x] / 2;
			pOutRow[x] = cv::Vec3b( nGray, nGray, nGray );
			if ( nLabel == k_nLabelBadCodeword )
			{
				// Red tint on corrupted codewords
				pOutRow[x][2] += 96;
			}
			else if ( nLabel == k_nLabelBadIndex )
			{
				// Blue tint on "bad index" (too large or bad color phase)
				pOutRow[x][0] += 96;
			}
		}
	}

	// Draw extracted point observations
	for ( const PointObservation_t &obs : observations )
	{
		int nRadius = std::max( 2, ( int )( sqrt( obs.nPixels / CV_PI ) + 0.5 ) );
		cv::circle( image, obs.p, nRadius, cv::Scalar( 255, 255, 255 ), 1, cv::LINE_AA );

		std::string text = std::to_string( obs.nLedIdx );
		cv::Point textPos( ( int )obs.p.x + nRadius + 2, ( int )obs.p.y + 4 );
		cv::putText( image, text, textPos, cv::FONT_HERSHEY_PLAIN, 0.8, cv::Scalar( 0, 0, 0 ), 3, cv::LINE_AA );
		cv::putText( image, text, textPos, cv::FONT_HERSHEY_PLAIN, 0.8, cv::Scalar( 255, 255, 255 ), 1, cv::LINE_AA );
	}

	if ( !cv::imwrite( path, image ) )
	{
		printf( "Error: can't write %s\n", path.c_str() );
	}
}


bool DecodeVideo( std::vector< PointObservation_t > *pObservations, const std::string &videoPath, const std::string &imageDir )
{
	std::string videoName = FileName( videoPath );

	StageClock_t::time_point scanStart = StageClock_t::now();
	std::vector< double > intensities;
	std::vector< ColorMap_t > colorMaps;
	cv::Size frameSize;
	double flFps = 0.0;
	cv::Mat brightest;
	if ( !ScanVideo( &intensities, &colorMaps, &frameSize, &flFps, imageDir.empty() ? nullptr : &brightest, videoPath ) )
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

	cv::Mat labels( code.size(), CV_32SC1, cv::Scalar( k_nLabelErasure ) );
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

	cv::Mat imageLabels;
	if ( !imageDir.empty() )
	{
		imageLabels = labels.clone();
	}

	int nDuplicateIdxs = 0;
	FindBlobs( pObservations, &nDuplicateIdxs, &labels );

	printf( "%s: %d LEDs (sync frames %d-%d, %d indices with duplicate blobs; scan %d frames %.2f s, label %.2f s)\n",
		videoName.c_str(), ( int )pObservations->size(), nSyncStart, nSyncEnd, nDuplicateIdxs, ( int )intensities.size(),
		flScanSeconds, SecondsSince( labelStart ) );

	if ( !imageDir.empty() )
	{
		RenderDecodeImage( imageLabels, brightest, *pObservations, ( std::filesystem::path( imageDir ) / ( videoName + ".decode.png" ) ).string() );
	}

	return true;
}
