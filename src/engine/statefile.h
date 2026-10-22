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

// Aleck64: a USF player has none of the arcade board, so a game that reads
// its inputs through the TLB (at physical 0xC0800000+) takes a TLB miss
// there, and some games stop running when that happens.  This rewrites the
// state's TLB entries for the board's I/O pages so that they point at a
// spare page of RDRAM holding "nothing pressed" (`in0`: the first input word
// with the DIP switches as set, everything else all ones).  The SDRAM is
// left alone.  Returns the number of TLB pages remapped (0 = nothing to do
// or no spare page).
int a64_remap_board_io(Bytes& state, uint32_t in0);

} // namespace usfm
