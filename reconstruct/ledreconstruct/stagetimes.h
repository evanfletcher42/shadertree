#pragma once

#include <chrono>
#include <map>
#include <stdio.h>
#include <string>

using StageClock_t = std::chrono::steady_clock;

inline double SecondsSince( StageClock_t::time_point start )
{
	return std::chrono::duration< double >( StageClock_t::now() - start ).count();
}

// Wall time per named stage, summed over calls.
class CStageTimes
{
public:
	void Add( const std::string &name, double flSeconds );
	void Print( const char *pTitle ) const;

private:
	std::map< std::string, double > m_seconds;
};

inline void CStageTimes::Add( const std::string &name, double flSeconds )
{
	m_seconds[name] += flSeconds;
}

inline void CStageTimes::Print( const char *pTitle ) const
{
	printf( "%s stage times:\n", pTitle );
	for ( const std::pair< const std::string, double > &stage : m_seconds )
	{
		printf( "  %-24s %8.2f s\n", stage.first.c_str(), stage.second );
	}
}

class CScopedStageTimer
{
public:
	CScopedStageTimer( CStageTimes *pTimes, const char *pName );
	~CScopedStageTimer();

private:
	CStageTimes *m_pTimes;
	const char *m_pName;
	StageClock_t::time_point m_start;
};

inline CScopedStageTimer::CScopedStageTimer( CStageTimes *pTimes, const char *pName )
	: m_pTimes( pTimes )
	, m_pName( pName )
	, m_start( StageClock_t::now() )
{
}

inline CScopedStageTimer::~CScopedStageTimer()
{
	m_pTimes->Add( m_pName, SecondsSince( m_start ) );
}
