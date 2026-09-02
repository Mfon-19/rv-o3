#include "sim/loader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

std::vector<uint32_t> loadHexFile(const char *path) {
  std::ifstream f(path);
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  std::vector<uint32_t> words;
  std::string line;
  while (std::getline(f, line)) {
    for (const char *comment : {"#", "//"})
      line.resize(std::min(line.size(), line.find(comment)));
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) {
      char *end = nullptr;
      unsigned long v = strtoul(tok.c_str(), &end, 16);
      if (end == tok.c_str() || *end != '\0' || v > 0xFFFFFFFFul) {
        fprintf(stderr, "%s: bad hex word '%s'\n", path, tok.c_str());
        exit(1);
      }
      words.push_back((uint32_t)v);
    }
  }
  return words;
}

std::vector<uint8_t> loadBinFile(const char *path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}
