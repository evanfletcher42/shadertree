#pragma once

// Describe the physical tree and hardware addressing here. 
constexpr int k_nLedsPerStrand = 215;
constexpr int k_nStrandsPerDevice = 32;
constexpr int k_nDevices = 2;
constexpr int k_nMaxLeds = k_nLedsPerStrand * k_nStrandsPerDevice * k_nDevices;
