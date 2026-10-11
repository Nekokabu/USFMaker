// USF Maker - N64 cartridge image loading.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#include "common.h"

namespace usfm {

struct RomInfo {
    std::string internal_name;   // 20 character name from the header (trimmed, may be Shift-JIS)
    std::string game_code;       // 4 character product code, e.g. "NSME" (may be empty)
    char region = 0;             // country code byte
    bool pal = false;            // 50 Hz game
    uint32_t crc1 = 0, crc2 = 0;
    uint32_t entry_point = 0;
    std::string cic;             // "6102", "6105", ... or "unknown"
    std::string source_format;   // "z64", "v64" or "n64" - how the file was stored
    size_t size = 0;             // size of the image in bytes
    bool eeprom_16k = false;     // known to carry a 16 kbit EEPROM (most EEPROM games have 4 kbit)
};

class Rom {
public:
    // Loads a .z64 / .v64 / .n64 image (the byte order is detected from the
    // header).  The image may also sit inside a .zip archive.
    bool load(const Path& path, std::string& err);
    bool load_from_memory(Bytes data, std::string& err);

    const RomInfo& info() const { return info_; }

    // Image in native cartridge (big endian) order, exactly `info().size` bytes.
    const Bytes& big_endian() const { return be_; }

    // Image as the emulator core wants it: every 32 bit word in host order and
    // the buffer padded with zeros to a power of two (at least 1 MB).  This is
    // also the byte order ROM data is stored in inside USF files.
    const Bytes& words() const { return words_; }

    bool empty() const { return be_.empty(); }

private:
    Bytes be_;
    Bytes words_;
    RomInfo info_;
};

// Swaps between big endian and "32 bit little endian words" layout in place.
void swap_words(uint8_t* data, size_t size);

} // namespace usfm
