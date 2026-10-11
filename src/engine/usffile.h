// USF Maker - reading and writing USF / miniUSF / USFlib files.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
//
// A USF file is a PSF container (version byte 0x21) whose "reserved" area
// holds two sparse images, each introduced by the marker "SR64":
//
//     "SR64"  { u32 length, u32 offset, bytes[length] }*  u32 0     <- cartridge
//     "SR64"  { u32 length, u32 offset, bytes[length] }*  u32 0     <- Project64 save state
//
// A player loads the file named by the "_lib" tag first and then lays the
// file itself on top, so a set normally keeps what is common to all songs
// in one .usflib and only the differences in each .miniusf.
#pragma once

#include "common.h"

#include <utility>

namespace usfm {

using TagList = std::vector<std::pair<std::string, std::string>>;

struct SparseBlock {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// Turns a coverage bitmap (one bit per 32 bit word) into a block list.
// Runs separated by fewer than `merge_gap` bytes are joined.
std::vector<SparseBlock> blocks_from_coverage(const uint8_t* bitmap, size_t bitmap_bytes, size_t data_size, uint32_t merge_gap);

// Blocks covering every non-zero part of `data`.
std::vector<SparseBlock> blocks_nonzero(const uint8_t* data, size_t size, uint32_t merge_gap);

// Blocks covering everything in `data` that differs from `base` (same size).
std::vector<SparseBlock> blocks_diff(const uint8_t* data, const uint8_t* base, size_t size, uint32_t merge_gap);

size_t blocks_total(const std::vector<SparseBlock>& blocks);

// Builds the reserved area.  Either block list may be empty.
Bytes build_usf_reserved(const uint8_t* rom_words, const std::vector<SparseBlock>& rom_blocks,
                         const uint8_t* state, const std::vector<SparseBlock>& state_blocks);

// Wraps a reserved area and tags into a complete file image.
Bytes build_usf_file(const Bytes& reserved, const TagList& tags);

// ---------------------------------------------------------------------------

struct UsfImage {
    Bytes rom_words;          // sparse cartridge data laid out flat, power of two size
    Bytes state;              // Project64 save state
    TagList tags;             // tags of the top level file
    bool enable_compare = false;
    bool enable_fifo_full = false;
    std::string error;
};

// Loads a .usf/.miniusf together with its _lib chain, the way a player would.
bool load_usf(const Path& path, UsfImage& out);

// Parses one file without following _lib.
bool parse_psf(const Bytes& file, Bytes& reserved, TagList& tags, std::string& err);

// Returns the value of a tag (case-insensitive name) or an empty string.
std::string find_tag(const TagList& tags, const char* name);

// Parses a PSF time tag ("90", "1:30", "1:30.5") into milliseconds.
bool parse_time_tag(const std::string& text, uint64_t& ms);

} // namespace usfm
