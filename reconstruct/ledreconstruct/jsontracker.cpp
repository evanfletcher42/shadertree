#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <fstream>

#include <json/json.h>

#include "windows.h"

//#include "pointtracker.h"
#include "jsontracker.h"


PointTracker::PointTracker()
{
}

PointTracker::~PointTracker()
{
}

bool PointTracker::LoadBlobsFromJson(std::string folderPath)
{
    std::vector< std::string > paths = findJsonPaths(folderPath);

    int nFrameIdx = 0;

    for (const std::string& path : paths)
    {
        Json::Value v;
        std::ifstream ifs(path);

        if (!ifs)
            continue;

        ifs >> v;

        std::vector< PointObservation_t > obsThisFrame;

        // Parse json into (made-up) PointObservation_t
        const Json::Value::Members keys = v.getMemberNames();

        for (const std::string ledStr : keys)
        {
            PointObservation_t obs;

			obs.nLedIdx = std::stoi(ledStr);

            if (obs.nLedIdx < 0 || obs.nLedIdx >= k_nMaxLeds)
            {
                // Bad decode
                continue;
            }

            // TBD: Convention - confirm
            obs.p.x = v[ledStr][0][1].asFloat();
            obs.p.y = v[ledStr][0][0].asFloat();

            obs.nFrameIdx = nFrameIdx;
            obs.status = PointObservation_t::eLabeled;

            obsThisFrame.push_back(obs);
        }

        m_observations.push_back(obsThisFrame);

		printf("%s: loaded %zu observations\n", path.c_str(), obsThisFrame.size());

        nFrameIdx++;
    }

	return true;
}

std::vector<std::string> PointTracker::findJsonPaths(std::string folderPath)
{
    std::vector<std::string> result;

    if (folderPath.empty()) {
        return result;
    }

    // Build search pattern: e.g. "C:\path\to\folder\*.json"
    std::string searchPath = folderPath;
    char last = searchPath.back();
    if (last != '\\' && last != '/') {
        searchPath += '\\';
    }
    searchPath += "*.json";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return result; // no matches or folder not found
    }

    do {
        // Skip directories, only regular files
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string fullPath = folderPath;
            char last2 = fullPath.back();
            if (last2 != '\\' && last2 != '/') {
                fullPath += '\\';
            }
            fullPath += findData.cFileName;
            result.push_back(fullPath);
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
    return result;
}
