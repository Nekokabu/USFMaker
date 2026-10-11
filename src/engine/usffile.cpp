// USF Maker - reading and writing USF / miniUSF / USFlib files.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "usffile.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace usfm {

static const uint32_t kSectionMagic = 0x34365253u; // "SR64"

static void push_run(std::vector<SparseBlock>& out, uint64_t start, uint64_t end, uint32_t merge_gap)
{
    if (end <= start)
        return;
    if (!out.empty()) {
        SparseBlock& last = out.back();
        const uint64_t last_end = (uint64_t)last.offset + last.length;
        if (start <= last_end + merge_gap) {
            last.length = (uint32_t)(end - last.offset);
            return;
        }
    }
    SparseBlock b;
    b.offset = (uint32_t)start;
    b.length = (uint32_t)(end - start);
    out.push_back(b);
}

std::vector<SparseBlock> blocks_from_coverage(const uint8_t* bitmap, size_t bitmap_bytes, size_t data_size, uint32_t merge_gap)
{
    std::vector<SparseBlock> out;
    if (!bitmap)
        return out;
    const size_t words = std::min(data_size / 4, bitmap_bytes * 8);
    size_t w = 0;
    while (w < words) {
        // skip whole empty bytes quickly
        if ((w & 7) == 0 && bitmap[w >> 3] == 0) {
            w += 8;
            continue;
        }
        if (!(bitmap[w >> 3] & (1u << (w & 7)))) {
            ++w;
            continue;
        }
        const size_t start = w;
        while (w < words && (bitmap[w >> 3] & (1u << (w & 7))))
            ++w;
        push_run(out, (uint64_t)start * 4, (uint64_t)w * 4, merge_gap);
    }
    return out;
}

std::vector<SparseBlock> blocks_nonzero(const uint8_t* data, size_t size, uint32_t merge_gap)
{
    std::vector<SparseBlock> out;
    size_t i = 0;
    const size_t words = size / 4;
    while (i < words) {
        uint32_t v;
        std::memcpy(&v, data + i * 4, 4);
        if (v == 0) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < words) {
            std::memcpy(&v, data + i * 4, 4);
            if (v == 0)
                break;
            ++i;
        }
        push_run(out, (uint64_t)start * 4, (uint64_t)i * 4, merge_gap);
    }
    // trailing bytes that do not fill a word
    if (size & 3) {
        bool nz = false;
        for (size_t k = words * 4; k < size; k++)
            if (data[k])
                nz = true;
        if (nz)
            push_run(out, words * 4, size, merge_gap);
    }
    return out;
}

std::vector<SparseBlock> blocks_diff(const uint8_t* data, const uint8_t* base, size_t size, uint32_t merge_gap)
{
    std::vector<SparseBlock> out;
    size_t i = 0;
    const size_t words = size / 4;
    while (i < words) {
        if (std::memcmp(data + i * 4, base + i * 4, 4) == 0) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < words && std::memcmp(data + i * 4, base + i * 4, 4) != 0)
            ++i;
        push_run(out, (uint64_t)start * 4, (uint64_t)i * 4, merge_gap);
    }
    if ((size & 3) && std::memcmp(data + words * 4, base + words * 4, size & 3) != 0)
        push_run(out, words * 4, size, merge_gap);
    return out;
}

size_t blocks_total(const std::vector<SparseBlock>& blocks)
{
    size_t n = 0;
    for (const SparseBlock& b : blocks)
        n += b.length;
    return n;
}

static void append_section(Bytes& out, const uint8_t* data, const std::vector<SparseBlock>& blocks)
{
    put32le(out, kSectionMagic);
    for (const SparseBlock& b : blocks) {
        if (!b.length)
            continue;
        put32le(out, b.length);
        put32le(out, b.offset);
        out.insert(out.end(), data + b.offset, data + b.offset + b.length);
    }
    put32le(out, 0);
}

Bytes build_usf_reserved(const uint8_t* rom_words, const std::vector<SparseBlock>& rom_blocks,
                         const uint8_t* state, const std::vector<SparseBlock>& state_blocks)
{
    Bytes out;
    out.reserve(16 + blocks_total(rom_blocks) + blocks_total(state_blocks) + 8 * (rom_blocks.size() + state_blocks.size()));
    append_section(out, rom_words, rom_blocks);
    append_section(out, state, state_blocks);
    return out;
}

Bytes build_usf_file(const Bytes& reserved, const TagList& tags)
{
    Bytes out;
    out.reserve(16 + reserved.size() + 256);
    out.push_back('P');
    out.push_back('S');
    out.push_back('F');
    out.push_back(0x21);
    put32le(out, (uint32_t)reserved.size());
    put32le(out, 0); // no compressed program
    put32le(out, 0); // its CRC
    out.insert(out.end(), reserved.begin(), reserved.end());

    if (!tags.empty()) {
        static const char marker[] = "[TAG]";
        out.insert(out.end(), marker, marker + 5);
        for (const auto& kv : tags) {
            // a value with line breaks is written as repeated lines
            size_t pos = 0;
            const std::string& v = kv.second;
            do {
                size_t nl = v.find('\n', pos);
                if (nl == std::string::npos)
                    nl = v.size();
                out.insert(out.end(), kv.first.begin(), kv.first.end());
                out.push_back('=');
                out.insert(out.end(), v.begin() + (std::ptrdiff_t)pos, v.begin() + (std::ptrdiff_t)nl);
                out.push_back('\n');
                pos = nl + 1;
            } while (pos <= v.size() && pos != 0 && pos - 1 < v.size());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

static bool iequals(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return i == a.size() && b[i] == 0;
}

std::string find_tag(const TagList& tags, const char* name)
{
    for (const auto& kv : tags)
        if (iequals(kv.first, name))
            return kv.second;
    return std::string();
}

bool parse_time_tag(const std::string& text, uint64_t& ms)
{
    // [[hours:]minutes:]seconds[.decimals]
    uint64_t parts[3] = { 0, 0, 0 };
    int count = 0;
    uint64_t frac = 0, frac_div = 1;
    bool in_frac = false, any = false;
    uint64_t cur = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            any = true;
            if (in_frac) {
                if (frac_div < 1000) {
                    frac = frac * 10 + (uint64_t)(c - '0');
                    frac_div *= 10;
                }
            } else {
                cur = cur * 10 + (uint64_t)(c - '0');
            }
        } else if (c == ':' && !in_frac) {
            if (count >= 2)
                return false;
            parts[count++] = cur;
            cur = 0;
        } else if ((c == '.' || c == ',') && !in_frac) {
            in_frac = true;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            continue;
        } else {
            return false;
        }
    }
    if (!any)
        return false;
    parts[count++] = cur;
    uint64_t seconds = 0;
    for (int i = 0; i < count; i++)
        seconds = seconds * 60 + parts[i];
    ms = seconds * 1000 + (frac * 1000) / frac_div;
    return true;
}

bool parse_psf(const Bytes& file, Bytes& reserved, TagList& tags, std::string& err)
{
    reserved.clear();
    tags.clear();
    if (file.size() < 16 || file[0] != 'P' || file[1] != 'S' || file[2] != 'F') {
        err = "not a PSF container";
        return false;
    }
    if (file[3] != 0x21) {
        err = "this PSF file is not a USF (wrong version byte)";
        return false;
    }
    const uint64_t reserved_size = rd32le(file.data() + 4);
    const uint64_t program_size = rd32le(file.data() + 8);
    if (16 + reserved_size + program_size > file.size()) {
        err = "file is truncated";
        return false;
    }
    reserved.assign(file.begin() + 16, file.begin() + 16 + (std::ptrdiff_t)reserved_size);

    size_t pos = (size_t)(16 + reserved_size + program_size);
    if (pos + 5 <= file.size() && std::memcmp(file.data() + pos, "[TAG]", 5) == 0) {
        pos += 5;
        std::string text(file.begin() + (std::ptrdiff_t)pos, file.end());
        size_t p = 0;
        while (p < text.size()) {
            size_t nl = text.find('\n', p);
            if (nl == std::string::npos)
                nl = text.size();
            std::string line = text.substr(p, nl - p);
            p = nl + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                while (!s.empty() && (unsigned char)s.back() <= 0x20)
                    s.pop_back();
                size_t b = 0;
                while (b < s.size() && (unsigned char)s[b] <= 0x20)
                    ++b;
                return s.substr(b);
            };
            std::string name = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));
            if (name.empty())
                continue;
            bool merged = false;
            for (auto& kv : tags) {
                if (iequals(kv.first, name.c_str())) {
                    kv.second += "\n" + value;
                    merged = true;
                    break;
                }
            }
            if (!merged)
                tags.emplace_back(name, value);
        }
    }
    return true;
}

static bool apply_section(UsfImage& img, const Bytes& reserved, std::string& err)
{
    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= reserved.size(); };
    auto get = [&]() {
        const uint32_t v = rd32le(reserved.data() + pos);
        pos += 4;
        return v;
    };

    if (reserved.empty())
        return true;
    if (!need(4)) {
        err = "reserved area is truncated";
        return false;
    }
    if (get() == kSectionMagic) {
        if (!need(4)) { err = "ROM section is truncated"; return false; }
        uint32_t len = get();
        while (len) {
            if (!need(4)) { err = "ROM section is truncated"; return false; }
            const uint32_t start = get();
            if ((uint64_t)start + len > ((uint64_t)64 << 20) || !need(len)) {
                err = "ROM section is damaged";
                return false;
            }
            if ((size_t)start + len > img.rom_words.size()) {
                size_t n = img.rom_words.empty() ? ((size_t)1 << 20) : img.rom_words.size();
                while ((size_t)start + len > n)
                    n *= 2;
                img.rom_words.resize(n, 0);
            }
            std::memcpy(img.rom_words.data() + start, reserved.data() + pos, len);
            pos += len;
            if (!need(4)) { err = "ROM section is truncated"; return false; }
            len = get();
        }
    }

    if (!need(4))
        return true; // a file may carry only a ROM section
    if (get() == kSectionMagic) {
        if (img.state.empty())
            img.state.assign(0x80275C, 0);
        if (!need(4)) { err = "save state section is truncated"; return false; }
        uint32_t len = get();
        while (len) {
            if (!need(4)) { err = "save state section is truncated"; return false; }
            const uint32_t start = get();
            if ((uint64_t)start + len > img.state.size() || !need(len)) {
                err = "save state section is damaged";
                return false;
            }
            std::memcpy(img.state.data() + start, reserved.data() + pos, len);
            pos += len;
            if (!need(4)) { err = "save state section is truncated"; return false; }
            len = get();
        }
    }
    return true;
}

static bool load_recursive(const Path& path, UsfImage& img, int depth, bool top)
{
    if (depth > 8) {
        img.error = "_lib chain is too deep";
        return false;
    }
    Bytes file;
    if (!read_file(path, file, img.error))
        return false;
    Bytes reserved;
    TagList tags;
    if (!parse_psf(file, reserved, tags, img.error))
        return false;

    // _lib first, then this file, then _lib2.._lib9
    const std::string lib = find_tag(tags, "_lib");
    if (!lib.empty()) {
        if (!load_recursive(path.parent_path() / path_from_utf8(lib), img, depth + 1, false))
            return false;
    }
    if (!apply_section(img, reserved, img.error))
        return false;
    for (int n = 2; n <= 9; n++) {
        const std::string name = "_lib" + std::to_string(n);
        const std::string libn = find_tag(tags, name.c_str());
        if (libn.empty())
            break;
        if (!load_recursive(path.parent_path() / path_from_utf8(libn), img, depth + 1, false))
            return false;
    }

    if (!find_tag(tags, "_enablecompare").empty())
        img.enable_compare = true;
    if (!find_tag(tags, "_enablefifofull").empty())
        img.enable_fifo_full = true;
    if (top)
        img.tags = tags;
    return true;
}

bool load_usf(const Path& path, UsfImage& out)
{
    out = UsfImage();
    if (!load_recursive(path, out, 0, true))
        return false;
    if (out.state.size() < 8 || rd32le(out.state.data()) != 0x23D8A6C8u) {
        out.error = "the file does not contain a Project64 save state";
        return false;
    }
    if (rd32le(out.state.data() + 4) == 0x400000)
        out.state.resize(0x40275C);
    if (out.rom_words.empty())
        out.rom_words.assign((size_t)1 << 20, 0);
    return true;
}

} // namespace usfm
