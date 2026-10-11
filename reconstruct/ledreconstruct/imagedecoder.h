#pragma once

#include <string>
#include <vector>

#include "observationset.h"

// Timing of shaderrunner/ledcodes.glsl: slot = int( iTime * 59.94f ) / 2 % 35.
// Slots 0-1 are white (sync), slots 2-34 are 33 R/G/B colors whose 32 transitions carry 4 Hadamard codewords.
constexpr double k_flCaptureFps = 59.94;
constexpr int k_nFramesPerSlot = 2;
constexpr int k_nSyncSlots = 2;
constexpr int k_nColorSlots = 33;
constexpr int k_nCycleSlots = k_nSyncSlots + k_nColorSlots;

// Decode one view's LED observations from a static video of ledcodes.glsl. Returns false (and prints why) if no
// complete code cycle was found. If imageDir is set, also writes <imageDir>/<video>.decode.png (see RenderDecodeImage).
bool DecodeVideo( std::vector< PointObservation_t > *pObservations, const std::string &videoPath, const std::string &imageDir );
