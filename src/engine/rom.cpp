// USF Maker - N64 cartridge image loading.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "rom.h"

#include "zip.h"

#include <cstring>

namespace usfm {

void swap_words(uint8_t* data, size_t size)
{
    for (size_t i = 0; i + 3 < size; i += 4) {
        const uint8_t a = data[i], b = data[i + 1];
        data[i] = data[i + 3];
        data[i + 1] = data[i + 2];
        data[i + 2] = b;
        data[i + 3] = a;
    }
}

static std::string trim_ascii(const uint8_t* p, size_t n)
{
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && s[start] == ' ')
        ++start;
    s.erase(0, start);
    for (char& c : s)
        if ((unsigned char)c < 0x20)
            c = ' ';
    return s;
}

static const char* identify_cic(const uint8_t* rom_be)
{
    // Same test the emulator core uses: the 64 bit sum of the IPL3 words.
    uint64_t sum = 0;
    for (size_t i = 0x40; i < 0x1000; i += 4)
        sum += rd32be(rom_be + i);
    switch (sum) {
    case 0x000000D057C85244ULL: return "6102";
    case 0x000000D0027FDF31ULL:
    case 0x000000CFFB631223ULL: return "6101";
    case 0x000000D6497E414BULL: return "6103";
    case 0x0000011A49F60E96ULL: return "6105";
    case 0x000000D6D5BE5580ULL: return "6106";
    default: return "unknown";
    }
}

// Games whose cartridge has a 16 kbit EEPROM, by the two letter game id in
// the product code ("NFUE": id "FU").  Every other EEPROM game has the
// 4 kbit part, and the two answer differently when asked what they are.
static bool has_16k_eeprom(const std::string& game_code)
{
    static const char* const ids[] = {
        "B7", // Banjo-Tooie
        "CW", // Cruis'n World
        "CZ", // Custom Robo V2
        "D6", // Densha de Go! 64
        "DO", // Donkey Kong 64
        "D2", // Doraemon 2: Nobita to Hikari no Shinden
        "3D", // Doraemon 3: Nobita no Machi SOS!
        "MX", // Excitebike 64
        "FU", // Conker's Bad Fur Day
        "GC", // GT 64: Championship Edition
        "IM", // Ide Yosuke no Mahjong Juku
        "NB", // Kobe Bryant in NBA Courtside
        "MV", // Mario Party 3
        "M8", // Mario Tennis
        "EV", // Neon Genesis Evangelion
        "PP", // Parlor! Pro 64
        "UB", // PD Ultraman Battle Collection 64
        "PD", // Perfect Dark
        "RZ", // Ridge Racer 64
        "R7", // Robot Poncots 64
        "EP", // Star Wars Episode I: Racer
        "YS", // Yoshi's Story
    };
    if (game_code.size() != 4)
        return false;
    const std::string id = game_code.substr(1, 2);
    for (const char* k : ids)
        if (id == k)
            return true;
    return false;
}

bool Rom::load(const Path& path, std::string& err)
{
    Bytes data;
    if (!read_file(path, data, err, (size_t)256 << 20))
        return false;
    return load_from_memory(std::move(data), err);
}

static bool n64_magic(const uint8_t* p)
{
    const uint32_t v = rd32be(p);
    return v == 0x80371240u || v == 0x37804012u || v == 0x40123780u;
}

// Picks the cartridge image out of a zip archive: the first member that
// starts with the N64 header word, preferring the usual file extensions.
static bool rom_from_zip(const Bytes& zip, Bytes& out, std::string& err)
{
    std::vector<ZipEntry> entries;
    if (!zip_list(zip, entries, err))
        return false;
    int best = -1, best_rank = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        const ZipEntry& e = entries[i];
        if (e.is_dir() || e.size < 0x1000 || e.size > ((uint64_t)64 << 20))
            continue;
        const std::string ext = name_extension(e.name);
        int rank = 1;
        if (ext == "z64" || ext == "n64" || ext == "v64" || ext == "u64" || ext == "rom" || ext == "bin")
            rank = 2;
        if (rank <= best_rank)
            continue;
        Bytes head;
        std::string ignored;
        if (!zip_extract(zip, e, head, ignored, 0, 4) || head.size() < 4 || !n64_magic(head.data()))
            continue;
        best = (int)i;
        best_rank = rank;
        if (rank == 2)
            break;
    }
    if (best < 0) {
        err = "there is no N64 ROM inside this zip archive";
        return false;
    }
    return zip_extract(zip, entries[(size_t)best], out, err, (size_t)64 << 20);
}

bool Rom::load_from_memory(Bytes data, std::string& err)
{
    be_.clear();
    words_.clear();
    info_ = RomInfo();

    if (looks_like_zip(data)) {
        Bytes inner;
        if (!rom_from_zip(data, inner, err))
            return false;
        data = std::move(inner);
    }

    if (data.size() < 0x1000) {
        err = "the file is too small to be an N64 ROM";
        return false;
    }
    if (data.size() > ((size_t)64 << 20)) {
        err = "the file is larger than 64 MB, which no N64 cartridge can be";
        return false;
    }

    // Byte order from the first word (PI bus configuration, 0x80371240 on
    // every licensed cartridge).  Images that do not carry that value are
    // assumed to be big endian already.
    const uint8_t b0 = data[0], b1 = data[1], b2 = data[2], b3 = data[3];
    if (b0 == 0x80 && b1 == 0x37 && b2 == 0x12 && b3 == 0x40) {
        info_.source_format = "z64";
    } else if (b0 == 0x37 && b1 == 0x80 && b2 == 0x40 && b3 == 0x12) {
        info_.source_format = "v64";
        for (size_t i = 0; i + 1 < data.size(); i += 2)
            std::swap(data[i], data[i + 1]);
    } else if (b0 == 0x40 && b1 == 0x12 && b2 == 0x37 && b3 == 0x80) {
        info_.source_format = "n64";
        swap_words(data.data(), data.size());
    } else if (b0 == 0x80 && b1 == 0x37) {
        info_.source_format = "z64";
    } else {
        err = "this does not look like an N64 ROM (unknown header)";
        return false;
    }

    // keep the size a multiple of four
    while (data.size() & 3)
        data.push_back(0);

    be_ = std::move(data);
    info_.size = be_.size();
    info_.entry_point = rd32be(be_.data() + 0x08);
    info_.crc1 = rd32be(be_.data() + 0x10);
    info_.crc2 = rd32be(be_.data() + 0x14);
    info_.internal_name = trim_ascii(be_.data() + 0x20, 20);
    {
        const uint8_t* code = be_.data() + 0x3B;
        bool printable = true;
        for (int i = 0; i < 4; i++)
            if (code[i] < 0x20 || code[i] > 0x7E)
                printable = false;
        if (printable)
            info_.game_code.assign(reinterpret_cast<const char*>(code), 4);
    }
    info_.region = (char)be_[0x3E];
    switch (be_[0x3E]) {
    case 0x44: case 0x46: case 0x49: case 0x50: case 0x53: case 0x55: case 0x58: case 0x59:
        info_.pal = true;
        break;
    default:
        info_.pal = false;
    }
    info_.cic = identify_cic(be_.data());
    info_.eeprom_16k = has_16k_eeprom(info_.game_code);

    size_t padded = (size_t)1 << 20;
    while (padded < be_.size())
        padded *= 2;
    words_.assign(padded, 0);
    std::memcpy(words_.data(), be_.data(), be_.size());
    swap_words(words_.data(), words_.size());

    return true;
}

} // namespace usfm
