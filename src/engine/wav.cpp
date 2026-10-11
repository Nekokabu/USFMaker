// USF Maker - minimal WAV writer.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "wav.h"

#include <cstring>

namespace usfm {

bool write_wav(const Path& path, const int16_t* stereo, size_t frames, uint32_t rate, std::string& err)
{
    Bytes out;
    const uint32_t data_bytes = (uint32_t)(frames * 4);
    out.reserve(44 + data_bytes);
    auto tag = [&out](const char* t) { out.insert(out.end(), t, t + 4); };
    auto u16 = [&out](uint16_t v) { out.push_back((uint8_t)v); out.push_back((uint8_t)(v >> 8)); };
    tag("RIFF");
    put32le(out, 36 + data_bytes);
    tag("WAVE");
    tag("fmt ");
    put32le(out, 16);
    u16(1);  // PCM
    u16(2);  // channels
    put32le(out, rate);
    put32le(out, rate * 4);
    u16(4);  // block align
    u16(16); // bits
    tag("data");
    put32le(out, data_bytes);
    const size_t pos = out.size();
    out.resize(pos + data_bytes);
    if (data_bytes)
        std::memcpy(out.data() + pos, stereo, data_bytes);
    return write_file(path, out.data(), out.size(), err);
}

} // namespace usfm
