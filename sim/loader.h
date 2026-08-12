// Program-image loaders.

#pragma once

#include <cstdint>
#include <vector>

// Text format: whitespace-separated 32-bit hex words, laid out sequentially
// from address 0. '#' and '//' start a comment that runs to end of line.
std::vector<uint32_t> loadHexFile(const char *path);

// Raw little-endian flat binary, loaded at address 0
std::vector<uint8_t> loadBinFile(const char *path);
