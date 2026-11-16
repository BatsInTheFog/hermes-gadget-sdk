// Builds small wisp_face.bin files for tests, the way the Wisp project's tools/pack_face.py does.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "hg/wisp_face.hpp"

namespace wisp_test {


inline void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v);
  b[at + 1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void add16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}

// A part as tools/pack_face.py writes it: full size w x h, pixels row-major (0 = black).
struct TestPart {
  std::string name;
  int w, h;
  std::vector<uint16_t> px;
};

// Builds a version 1 face file the way tools/pack_face.py does (one span per lit run).
inline std::vector<uint8_t> make_file(const std::vector<TestPart>& parts, const std::string& meta) {
  const size_t parts_off = 48, meta_off = parts_off + 48 * parts.size();
  size_t data_off = (meta_off + meta.size() + 3) & ~size_t{3};
  std::vector<uint8_t> f(data_off, 0);
  std::memcpy(f.data(), "WISPFACE", 8);
  put16(f, 8, 1);
  put16(f, 10, 48);
  put16(f, 12, 368);
  put16(f, 14, 448);
  put16(f, 16, static_cast<uint16_t>(parts.size()));
  put16(f, 18, 24);
  put32(f, 20, static_cast<uint32_t>(parts_off));
  put32(f, 24, static_cast<uint32_t>(meta_off));
  put32(f, 28, static_cast<uint32_t>(meta.size()));
  put32(f, 32, static_cast<uint32_t>(data_off));
  std::memcpy(f.data() + meta_off, meta.data(), meta.size());
  for (size_t i = 0; i < parts.size(); ++i) {
    const TestPart& p = parts[i];
    int x0 = p.w, y0 = p.h, x1 = -1, y1 = -1;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x)
        if (p.px[y * p.w + x]) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    const size_t e = parts_off + 48 * i;
    std::memcpy(f.data() + e, p.name.data(), p.name.size());
    put16(f, e + 24, static_cast<uint16_t>(p.w));
    put16(f, e + 26, static_cast<uint16_t>(p.h));
    if (x1 < 0) continue;  // nothing lit: no block
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<uint8_t> rows;
    std::vector<uint32_t> offsets;
    for (int y = y0; y <= y1; ++y) {
      offsets.push_back(static_cast<uint32_t>(4 * ch + rows.size()));
      std::vector<std::pair<int, int>> spans;
      for (int x = 0; x < cw; ++x) {
        if (!p.px[y * p.w + x0 + x]) continue;
        if (!spans.empty() && spans.back().first + spans.back().second == x) ++spans.back().second;
        else spans.push_back({x, 1});
      }
      add16(rows, static_cast<uint16_t>(spans.size()));
      for (auto [sx, n] : spans) {
        add16(rows, static_cast<uint16_t>(sx));
        add16(rows, static_cast<uint16_t>(n));
        for (int k = 0; k < n; ++k) add16(rows, p.px[y * p.w + x0 + sx + k]);
      }
    }
    std::vector<uint8_t> block(4 * ch);
    for (int r = 0; r < ch; ++r) put32(block, 4 * r, offsets[r]);
    block.insert(block.end(), rows.begin(), rows.end());
    while (block.size() % 4) block.push_back(0);
    put16(f, e + 28, static_cast<uint16_t>(x0));
    put16(f, e + 30, static_cast<uint16_t>(y0));
    put16(f, e + 32, static_cast<uint16_t>(cw));
    put16(f, e + 34, static_cast<uint16_t>(ch));
    put32(f, e + 36, static_cast<uint32_t>(f.size()));
    put32(f, e + 40, static_cast<uint32_t>(block.size()));
    f.insert(f.end(), block.begin(), block.end());
  }
  put32(f, 36, static_cast<uint32_t>(f.size()));
  put32(f, 40, hg::crc32(f.data() + 48, f.size() - 48));
  return f;
}

inline TestPart dot_part(const char* name, int w, int h, int x, int y, uint16_t colour) {
  TestPart p{name, w, h, std::vector<uint16_t>(static_cast<size_t>(w) * h, 0)};
  p.px[y * w + x] = colour;
  return p;
}

}  // namespace wisp_test
