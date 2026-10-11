// USF Maker - minimal WAV writer.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#include "common.h"

namespace usfm {

// 16 bit stereo PCM.
bool write_wav(const Path& path, const int16_t* stereo, size_t frames, uint32_t rate, std::string& err);

} // namespace usfm
