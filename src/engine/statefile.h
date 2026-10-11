// USF Maker - importing emulator save states.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#include "common.h"

namespace usfm {

// Loads a Project64 save state and returns it in the classic uncompressed
// "Project64 1.x" layout that USF files are built from.  Accepted input:
//   - plain .pj files of any Project64 version (1.6 ... 4.x),
//   - the .pj.zip files Project64 writes by default.
// `note` (optional) receives a short description of what was converted.
bool load_pj64_state(const Path& path, Bytes& out, std::string& err, std::string* note = nullptr);
bool load_pj64_state_from_memory(const Bytes& file, Bytes& out, std::string& err, std::string* note = nullptr);

// A song title suggestion from a save state's file name: "Zelda.pj3.zip"
// gives "Zelda".
std::string state_title_from_path(const Path& path);

// Tells what kind of file the user handed us (drag and drop accepts both
// ROMs and save states, and either may be zipped).
enum class FileKind { Unknown, Rom, SaveState };
FileKind sniff_file(const Path& path);

} // namespace usfm
