// USF Maker - reading .zip archives (zipped ROMs, zipped Project64 save states).
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
//
// A small self-contained reader: "stored" and "deflate" entries, no ZIP64,
// no encryption.  That covers what ROM sets and emulators produce.
#pragma once

#include "common.h"

namespace usfm {

struct ZipEntry {
    std::string name;            // as stored in the archive (bytes; usually UTF-8 or the OEM code page)
    uint32_t method = 0;         // 0 = stored, 8 = deflate
    uint32_t flags = 0;
    uint32_t crc32 = 0;
    uint64_t packed_size = 0;
    uint64_t size = 0;
    uint64_t header_offset = 0;  // of the local file header
    bool is_dir() const { return !name.empty() && (name.back() == '/' || name.back() == '\\'); }
};

// True when `file` starts like a zip archive.
bool looks_like_zip(const Bytes& file);

// Lists the archive's entries.
bool zip_list(const Bytes& file, std::vector<ZipEntry>& out, std::string& err);

// Unpacks one entry.  `max_size` protects against absurd archives.  With
// `first_bytes` > 0 only (at least) that many leading bytes are produced and
// no checksum is verified - used to look at a file's header cheaply.
bool zip_extract(const Bytes& file, const ZipEntry& entry, Bytes& out, std::string& err,
                 size_t max_size = (size_t)256 << 20, size_t first_bytes = 0);

// Raw DEFLATE (RFC 1951) decoder.  Stops with an error if the output would
// grow beyond `max_out`; if `stop_after` > 0 it returns successfully as soon
// as at least that many bytes are available.
bool inflate_raw(const uint8_t* src, size_t src_size, Bytes& out, std::string& err,
                 size_t max_out, size_t stop_after = 0, size_t size_hint = 0);

uint32_t crc32_of(const uint8_t* data, size_t size, uint32_t crc = 0);

// Lower-cased extension of an archive member or file name, without the dot.
std::string name_extension(const std::string& name);

} // namespace usfm
