#pragma once

#include <string>
#include <vector>

#include "observationset.h"

// Timing from shaderrunner/ledcodes.glsl: slot = int( iTime * 59.94f ) / 2 % 35.
// Slots 0-1 :bright white (sync pulse).
// Slots 2-34: 32 transitions between R/G/B colors, with each transition being a codeword bit
constexpr double k_flCaptureFps = 59.94;
constexpr int k_nFramesPerSlot = 2;
constexpr int k_nSyncSlots = 2;
constexpr int k_nColorSlots = 33;
constexpr int k_nCycleSlots = k_nSyncSlots + k_nColorSlots;

// Extract one viewpoint's LED observations from a static video of the tree playing ledcodes.glsl
// Populates pObservations and returns true on success.
// If imageDir is not empty, also writes a debug visualization to <imageDir>/<video>.decode.png
bool DecodeVideo( std::vector< PointObservation_t > *pObservations, const std::string &videoPath, const std::string &imageDir );
