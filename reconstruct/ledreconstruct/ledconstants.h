#pragma once

// Devices x strands x LEDs per strand, device-major addresses.
constexpr int k_nLedsPerStrand = 215;
constexpr int k_nStrandsPerDevice = 32;
constexpr int k_nDevices = 2;
constexpr int k_nMaxLeds = k_nLedsPerStrand * k_nStrandsPerDevice * k_nDevices;
