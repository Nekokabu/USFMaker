// USF Maker - importing emulator save states.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "statefile.h"

#include "zip.h"

#include <cstring>

namespace usfm {

// Project64 has used two layouts for the main part of a save state:
//
//   0x23D8A6C8  the original one (1.x, 2.x, 3.0): 32 bit program counter,
//               32 bit CP0 registers, 20 byte TLB entries.  USF files embed
//               exactly this layout.
//   0x25EF3FAC  current development builds: 64 bit program counter, 64 bit
//               CP0 registers, 40 byte TLB entries.  Everything else is the
//               same, so it converts without loss for 32 bit games.
static const uint32_t kPj64Magic = 0x23D8A6C8u;
static const uint32_t kPj64Magic64 = 0x25EF3FACu;

static const size_t kOldHeader = 0x75C;
static const size_t kNewHeader = 0xA60;

static bool ram_size_ok(uint32_t ram)
{
    return ram == 0x400000 || ram == 0x800000;
}

static bool accept_classic(const Bytes& file, Bytes& out, std::string& err)
{
    const uint32_t ram = rd32le(file.data() + 4);
    if (!ram_size_ok(ram)) {
        err = "the save state has an unsupported RDRAM size";
        return false;
    }
    const size_t need = kOldHeader + ram + 0x2000;
    if (file.size() < need) {
        err = "the save state is truncated";
        return false;
    }
    out.assign(file.begin(), file.begin() + (std::ptrdiff_t)need);
    // The first byte of each TLB entry is a C++ bool followed by padding
    // that Project64 never initialises; clear it so that equal machine
    // states give equal files.
    for (int i = 0; i < 32; i++) {
        uint8_t* e = out.data() + 0x49C + (size_t)i * 20;
        const uint8_t defined = e[0] ? 1 : 0;
        wr32le(e, defined);
    }
    return true;
}

static bool convert_64(const Bytes& file, Bytes& out, std::string& err)
{
    const uint32_t ram = rd32le(file.data() + 4);
    if (!ram_size_ok(ram)) {
        err = "the save state has an unsupported RDRAM size";
        return false;
    }
    const size_t need = kNewHeader + ram + 0x2000;
    if (file.size() < need) {
        err = "the save state is truncated";
        return false;
    }
    const uint8_t* s = file.data();
    out.assign(kOldHeader + ram + 0x2000, 0);
    uint8_t* d = out.data();

    wr32le(d, kPj64Magic);
    std::memcpy(d + 4, s + 4, 4 + 0x40 + 4);           // RDRAM size, ROM header, VI timer
    std::memcpy(d + 0x4C, s + 0x4C, 4);                // program counter (low half)
    std::memcpy(d + 0x50, s + 0x54, 256 + 256);        // GPR, FPR
    for (int i = 0; i < 32; i++)                       // CP0 (low halves)
        std::memcpy(d + 0x250 + i * 4, s + 0x254 + i * 8, 4);
    // FPCR, HI, LO and the RDRAM/SP/DPC/MI/VI/AI/PI/RI/SI register blocks
    std::memcpy(d + 0x2D0, s + 0x354, 128 + 16 + 316);
    for (int i = 0; i < 32; i++) {                     // TLB
        const uint8_t* se = s + 0x520 + (size_t)i * 40;
        uint8_t* de = d + 0x49C + (size_t)i * 20;
        wr32le(de, se[0] ? 1u : 0u);
        std::memcpy(de + 4, se + 8, 4);                // PageMask
        std::memcpy(de + 8, se + 16, 4);               // EntryHi
        std::memcpy(de + 12, se + 24, 4);              // EntryLo0
        std::memcpy(de + 16, se + 32, 4);              // EntryLo1
    }
    std::memcpy(d + 0x71C, s + 0xA20, 0x40);           // PIF RAM
    std::memcpy(d + kOldHeader, s + kNewHeader, (size_t)ram + 0x2000);
    return true;
}

static bool accept_plain(const Bytes& file, Bytes& out, std::string& err, std::string* note)
{
    if (file.size() >= 8) {
        const uint32_t magic = rd32le(file.data());
        if (magic == kPj64Magic)
            return accept_classic(file, out, err);
        if (magic == kPj64Magic64) {
            if (note)
                *note = "converted from the newer Project64 save state layout";
            return convert_64(file, out, err);
        }
    }
    err = "this is not a Project64 save state";
    return false;
}

bool load_pj64_state_from_memory(const Bytes& file, Bytes& out, std::string& err, std::string* note)
{
    if (note)
        note->clear();
    if (!looks_like_zip(file))
        return accept_plain(file, out, err, note);

    std::vector<ZipEntry> entries;
    if (!zip_list(file, entries, err))
        return false;
    for (const ZipEntry& e : entries) {
        // smallest possible state: header + 4 MB + SP memory
        if (e.is_dir() || e.size < kOldHeader + 0x400000 + 0x2000 || e.size > ((uint64_t)32 << 20))
            continue;
        Bytes head;
        std::string ignored;
        if (!zip_extract(file, e, head, ignored, 0, 4) || head.size() < 4)
            continue;
        const uint32_t magic = rd32le(head.data());
        if (magic != kPj64Magic && magic != kPj64Magic64)
            continue;
        Bytes inner;
        if (!zip_extract(file, e, inner, err, (size_t)32 << 20))
            return false;
        return accept_plain(inner, out, err, note);
    }
    err = "there is no Project64 save state inside this zip archive";
    return false;
}

bool load_pj64_state(const Path& path, Bytes& out, std::string& err, std::string* note)
{
    Bytes file;
    if (!read_file(path, file, err, (size_t)64 << 20))
        return false;
    return load_pj64_state_from_memory(file, out, err, note);
}

static FileKind kind_of_header(const uint8_t* p)
{
    const uint32_t le = rd32le(p);
    if (le == kPj64Magic || le == kPj64Magic64)
        return FileKind::SaveState;
    const uint32_t be = rd32be(p);
    if (be == 0x80371240u || be == 0x37804012u || be == 0x40123780u || (be >> 16) == 0x8037u)
        return FileKind::Rom;
    return FileKind::Unknown;
}

FileKind sniff_file(const Path& path)
{
    Bytes file;
    std::string err;
    if (!read_file(path, file, err, (size_t)256 << 20) || file.size() < 4)
        return FileKind::Unknown;
    if (!looks_like_zip(file))
        return kind_of_header(file.data());
    std::vector<ZipEntry> entries;
    if (!zip_list(file, entries, err))
        return FileKind::Unknown;
    for (const ZipEntry& e : entries) {
        Bytes head;
        if (e.is_dir() || e.size < 0x1000 || !zip_extract(file, e, head, err, 0, 4) || head.size() < 4)
            continue;
        const FileKind k = kind_of_header(head.data());
        if (k != FileKind::Unknown)
            return k;
    }
    return FileKind::Unknown;
}

std::string state_title_from_path(const Path& path)
{
    std::string name = path_to_utf8(path.filename());
    for (int pass = 0; pass < 2; pass++) {
        const std::string ext = name_extension(name);
        const bool strip = ext == "zip" || ext == "st" || (ext.size() >= 2 && ext.size() <= 4 && ext.compare(0, 2, "pj") == 0) ||
                           (ext.size() == 3 && ext.compare(0, 2, "st") == 0);
        if (!strip)
            break;
        name.resize(name.size() - ext.size() - 1);
    }
    return name;
}

// ---- Aleck64 ----------------------------------------------------------
int a64_remap_board_io(Bytes& state, uint32_t in0)
{
    const size_t kTlb = 0x49C, kRdram = 0x75C;
    if (state.size() < kRdram || rd32le(state.data()) != 0x23D8A6C8u)
        return 0;
    const uint32_t rdram_size = rd32le(state.data() + 4);
    if (rdram_size < 0x400000 || state.size() < kRdram + rdram_size)
        return 0;
    // which TLB pages (4 KB ones only) point at the board's I/O space?
    std::vector<size_t> hits;
    for (int i = 0; i < 32; i++) {
        const uint8_t* e = state.data() + kTlb + (size_t)i * 20;
        const uint32_t mask = rd32le(e + 4);
        if (mask != 0)
            continue;
        for (int half = 0; half < 2; half++) {
            const uint32_t lo = rd32le(e + 12 + half * 4);
            if (!(lo & 2))
                continue;   // not valid
            const uint32_t phys = (uint32_t)(((uint64_t)((lo & 0x3FFFFFC0u) >> 6) << 12) & 0xFFFFFFFFu);
            if (phys >= 0xC0800000u && phys < 0xC1000000u)
                hits.push_back(kTlb + (size_t)i * 20 + 12 + half * 4);
        }
    }
    if (hits.empty())
        return 0;
    // a page of RDRAM nobody uses: the highest one that is all zero
    uint32_t page = 0;
    for (uint32_t a = rdram_size - 0x1000; a >= 0x200000; a -= 0x1000) {
        const uint8_t* p = state.data() + kRdram + a;
        bool zero = true;
        for (size_t k = 0; k < 0x1000 && zero; k++)
            zero = p[k] == 0;
        if (zero) {
            page = a;
            break;
        }
    }
    if (!page)
        return 0;
    uint8_t* p = state.data() + kRdram + page;
    std::memset(p, 0xFF, 0x1000);
    wr32le(p, in0);
    for (size_t at : hits) {
        const uint32_t lo = rd32le(state.data() + at);
        wr32le(state.data() + at, (lo & 0x3Fu) | ((page >> 12) << 6));
    }
    return (int)hits.size();
}

} // namespace usfm
