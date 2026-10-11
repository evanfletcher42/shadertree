#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include <ctype.h>
#include <stdio.h>

#include <json/json.h>

#include "imagedecoder.h"
#include "ledconstants.h"
#include "observationset.h"

namespace fs = std::filesystem;


static std::vector< fs::path > ListFiles( const std::string &dirPath, const std::vector< std::string > &extensions )
{
	std::vector< fs::path > paths;
	std::error_code ec;
	for ( const fs::directory_entry &entry : fs::directory_iterator( dirPath, ec ) )
	{
		if ( !entry.is_regular_file() )
			continue;

		std::string ext = entry.path().extension().string();
		std::transform( ext.begin(), ext.end(), ext.begin(), []( unsigned char c ) { return ( char )tolower( c ); } );
		if ( std::find( extensions.begin(), extensions.end(), ext ) == extensions.end() )
			continue;

		paths.push_back( entry.path() );
	}

	std::sort( paths.begin(), paths.end() );
	return paths;
}


// Format: { "<led idx>": [ [ row, col ], pixels ], ... }
static bool ReadObservationsJson( std::vector< PointObservation_t > *pObservations, const fs::path &path )
{
	std::ifstream ifs( path );
	if ( !ifs )
		return false;

	Json::Value root;
	Json::CharReaderBuilder builder;
	std::string errors;
	if ( !Json::parseFromStream( builder, ifs, &root, &errors ) || !root.isObject() )
		return false;

	for ( const std::string &key : root.getMemberNames() )
	{
		const Json::Value &entry = root[key];

		PointObservation_t obs;
		obs.nLedIdx = std::stoi( key );
		if ( obs.nLedIdx < 0 || obs.nLedIdx >= k_nMaxLeds )
			continue;

		obs.p.x = entry[0][1].asFloat();
		obs.p.y = entry[0][0].asFloat();
		obs.nPixels = ( int )entry[1].asDouble();
		pObservations->push_back( obs );
	}

	std::sort( pObservations->begin(), pObservations->end(),
		[]( const PointObservation_t &a, const PointObservation_t &b ) { return a.nLedIdx < b.nLedIdx; } );
	return true;
}


static bool WriteObservationsJson( const fs::path &path, const std::vector< PointObservation_t > &observations )
{
	Json::Value root( Json::objectValue );
	for ( const PointObservation_t &obs : observations )
	{
		Json::Value center( Json::arrayValue );
		center.append( obs.p.y );
		center.append( obs.p.x );

		Json::Value entry( Json::arrayValue );
		entry.append( center );
		entry.append( obs.nPixels );

		root[std::to_string( obs.nLedIdx )] = entry;
	}

	std::ofstream ofs( path );
	if ( !ofs )
		return false;

	Json::StreamWriterBuilder builder;
	std::unique_ptr< Json::StreamWriter > pWriter( builder.newStreamWriter() );
	pWriter->write( root, &ofs );
	return true;
}


static bool LoadOrDecodeVideo( std::vector< PointObservation_t > *pObservations, const fs::path &videoPath, const std::string &dumpDir )
{
	if ( dumpDir.empty() )
		return DecodeVideo( pObservations, videoPath.string() );

	fs::path jsonPath = fs::path( dumpDir ) / ( videoPath.filename().string() + ".json" );

	std::error_code ec;
	bool bCacheFresh = fs::exists( jsonPath, ec ) && fs::last_write_time( jsonPath, ec ) >= fs::last_write_time( videoPath, ec );
	if ( bCacheFresh && ReadObservationsJson( pObservations, jsonPath ) )
	{
		printf( "%s: %d LEDs (cached)\n", videoPath.filename().string().c_str(), ( int )pObservations->size() );
		return true;
	}

	if ( !DecodeVideo( pObservations, videoPath.string() ) )
		return false;

	if ( !WriteObservationsJson( jsonPath, *pObservations ) )
	{
		printf( "Error: can't write %s\n", jsonPath.string().c_str() );
	}

	return true;
}


bool ObservationSet::LoadFromVideos( const std::string &videoDir, const std::string &dumpDir )
{
	std::vector< fs::path > paths = ListFiles( videoDir, { ".mov", ".mp4" } );
	if ( paths.empty() )
	{
		printf( "Error: no videos in %s\n", videoDir.c_str() );
		return false;
	}

	std::error_code ec;
	if ( !dumpDir.empty() )
	{
		fs::create_directories( dumpDir, ec );
	}

	int nVideos = ( int )paths.size();
	std::vector< std::vector< PointObservation_t > > views( nVideos );
	std::vector< char > decoded( nVideos, 0 );

	// One video per thread; each decode is single-threaded apart from the video decoder itself.
	std::atomic< int > nNext( 0 );
	auto worker = [&]()
	{
		for ( int i = nNext++; i < nVideos; i = nNext++ )
		{
			decoded[i] = LoadOrDecodeVideo( &views[i], paths[i], dumpDir );
		}
	};

	int nThreads = std::max( 1, std::min( ( int )std::thread::hardware_concurrency(), nVideos ) );
	std::vector< std::thread > threads;
	for ( int i = 0; i < nThreads; i++ )
	{
		threads.emplace_back( worker );
	}
	for ( std::thread &thread : threads )
	{
		thread.join();
	}

	for ( int i = 0; i < nVideos; i++ )
	{
		if ( !decoded[i] )
			continue;

		m_observations.push_back( std::move( views[i] ) );
	}

	printf( "Decoded %d of %d videos\n", ( int )m_observations.size(), nVideos );
	return !m_observations.empty();
}


bool ObservationSet::LoadFromJson( const std::string &jsonDir )
{
	for ( const fs::path &path : ListFiles( jsonDir, { ".json" } ) )
	{
		std::vector< PointObservation_t > observations;
		if ( !ReadObservationsJson( &observations, path ) )
		{
			printf( "%s: can't read, skipped\n", path.string().c_str() );
			continue;
		}

		printf( "%s: loaded %d observations\n", path.string().c_str(), ( int )observations.size() );
		m_observations.push_back( std::move( observations ) );
	}

	return !m_observations.empty();
}
