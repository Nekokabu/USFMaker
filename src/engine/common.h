// USF Maker - shared declarations for the engine.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace usfm {

using Bytes = std::vector<uint8_t>;
using Path = std::filesystem::path;

// Reads a whole file. Returns false (and fills err) on failure.
bool read_file(const Path& path, Bytes& out, std::string& err, size_t max_size = (size_t)512 << 20);
bool write_file(const Path& path, const void* data, size_t size, std::string& err);

// UTF-8 <-> filesystem path helpers that behave the same on Windows and elsewhere.
Path path_from_utf8(const std::string& s);
std::string path_to_utf8(const Path& p);

// Replaces characters that are not allowed in Windows file names.
std::string sanitize_filename(const std::string& utf8);

// Formats milliseconds as m:ss.mmm (the form PSF tags use).
std::string format_time_tag(uint64_t ms);
// Formats seconds as m:ss for display.
std::string format_time_short(double seconds);

inline uint32_t rd32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint32_t rd32le(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
inline void wr32le(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
inline void put32le(Bytes& b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 24)); }

} // namespace usfm
