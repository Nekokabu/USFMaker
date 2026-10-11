// USF Maker - reading .zip archives.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "zip.h"

#include <cstring>

namespace usfm {

// ----------------------------------------------------------------------------
// CRC-32 (the zip one, polynomial 0xEDB88320)
// ----------------------------------------------------------------------------

namespace {

struct CrcTable {
    uint32_t t[256];
    CrcTable()
    {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};

} // namespace

uint32_t crc32_of(const uint8_t* data, size_t size, uint32_t crc)
{
    static const CrcTable table;
    uint32_t c = ~crc;
    for (size_t i = 0; i < size; i++)
        c = table.t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ----------------------------------------------------------------------------
// DEFLATE
// ----------------------------------------------------------------------------

namespace {

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int cnt = 0;
    bool overrun = false;

    void refill()
    {
        while (cnt <= 56 && p < end) {
            buf |= (uint64_t)*p++ << cnt;
            cnt += 8;
        }
    }
    // Missing bits at the very end of the input read as zero; drop() notices
    // when more bits are consumed than the input really had.
    uint32_t peek(int n)
    {
        if (cnt < n)
            refill();
        return (uint32_t)(buf & ((1ull << n) - 1));
    }
    void drop(int n)
    {
        if (n > cnt) {
            overrun = true;
            buf = 0;
            cnt = 0;
            return;
        }
        buf >>= n;
        cnt -= n;
    }
    uint32_t get(int n)
    {
        const uint32_t v = peek(n);
        drop(n);
        return v;
    }
    void align()
    {
        const int r = cnt & 7;
        buf >>= r;
        cnt -= r;
    }
};

const int kFastBits = 10;

struct Huffman {
    uint16_t count[16];              // number of codes of each length
    uint16_t symbol[288];            // symbols ordered by code
    uint16_t fast[1 << kFastBits];   // (length << 9) | symbol for short codes, 0 = use the slow path

    // Returns false for an over-subscribed set of code lengths.
    bool build(const uint8_t* lengths, int n)
    {
        std::memset(count, 0, sizeof(count));
        std::memset(fast, 0, sizeof(fast));
        for (int i = 0; i < n; i++)
            count[lengths[i]]++;
        count[0] = 0;

        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0)
                return false;
        }

        uint16_t offs[16];
        uint16_t next_code[16];
        offs[1] = 0;
        next_code[0] = 0;
        uint32_t code = 0;
        for (int len = 1; len < 16; len++) {
            if (len > 1)
                offs[len] = (uint16_t)(offs[len - 1] + count[len - 1]);
            code = (code + (len > 1 ? count[len - 1] : 0)) << 1;
            next_code[len] = (uint16_t)code;
        }
        for (int i = 0; i < n; i++) {
            const int len = lengths[i];
            if (!len)
                continue;
            symbol[offs[len]++] = (uint16_t)i;
            const uint32_t c = next_code[len]++;
            if (len <= kFastBits) {
                // codes are sent most significant bit first, the bit reader is LSB first
                uint32_t rev = 0;
                for (int b = 0; b < len; b++)
                    rev |= ((c >> b) & 1u) << (len - 1 - b);
                const uint16_t e = (uint16_t)((len << 9) | i);
                for (uint32_t k = rev; k < (1u << kFastBits); k += 1u << len)
                    fast[k] = e;
            }
        }
        return true;
    }

    // Returns the symbol or -1.
    int decode(Bits& in) const
    {
        const uint32_t look = in.peek(15);
        const uint16_t e = fast[look & ((1u << kFastBits) - 1)];
        if (e) {
            in.drop(e >> 9);
            return e & 0x1FF;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)((look >> (len - 1)) & 1u);
            const int c = count[len];
            if (code - c < first) {
                in.drop(len);
                return symbol[index + (code - first)];
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

struct Tables {
    Huffman lit;
    Huffman dist;
};

} // namespace

bool inflate_raw(const uint8_t* src, size_t src_size, Bytes& out, std::string& err,
                 size_t max_out, size_t stop_after, size_t size_hint)
{
    out.clear();
    if (size_hint && size_hint <= max_out)
        out.reserve(size_hint);

    Bits in{ src, src + src_size };
    Tables tb;

    for (;;) {
        const uint32_t final_block = in.get(1);
        const uint32_t type = in.get(2);
        if (in.overrun) {
            err = "compressed data ends too early";
            return false;
        }

        if (type == 0) {
            in.align();
            const uint32_t len = in.get(16);
            const uint32_t nlen = in.get(16);
            if (in.overrun || (len ^ nlen) != 0xFFFFu) {
                err = "compressed data is damaged";
                return false;
            }
            if (out.size() + len > max_out) {
                err = "the unpacked file would be too large";
                return false;
            }
            uint32_t left = len;
            while (left && in.cnt >= 8) {
                out.push_back((uint8_t)in.get(8));
                left--;
            }
            if (left) {
                // the bit buffer is empty now, the rest comes straight from the input
                if ((size_t)(in.end - in.p) < left) {
                    err = "compressed data ends too early";
                    return false;
                }
                out.insert(out.end(), in.p, in.p + left);
                in.p += left;
            }
        } else if (type == 1 || type == 2) {
            if (type == 1) {
                uint8_t l[288 + 30];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                for (i = 0; i < 30; i++) l[288 + i] = 5;
                tb.lit.build(l, 288);
                tb.dist.build(l + 288, 30);
            } else {
                const int nlen = (int)in.get(5) + 257;
                const int ndist = (int)in.get(5) + 1;
                const int ncode = (int)in.get(4) + 4;
                if (in.overrun || nlen > 286 || ndist > 30) {
                    err = "compressed data is damaged";
                    return false;
                }
                static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint8_t l[288 + 32];
                std::memset(l, 0, sizeof(l));
                for (int i = 0; i < ncode; i++)
                    l[order[i]] = (uint8_t)in.get(3);
                Huffman& lencode = tb.dist;   // borrowed, rebuilt below
                if (!lencode.build(l, 19)) {
                    err = "compressed data is damaged";
                    return false;
                }
                std::memset(l, 0, sizeof(l));
                int idx = 0;
                while (idx < nlen + ndist) {
                    const int sym = lencode.decode(in);
                    if (sym < 0 || in.overrun) {
                        err = "compressed data is damaged";
                        return false;
                    }
                    if (sym < 16) {
                        l[idx++] = (uint8_t)sym;
                        continue;
                    }
                    uint8_t prev = 0;
                    int rep;
                    if (sym == 16) {
                        if (idx == 0) {
                            err = "compressed data is damaged";
                            return false;
                        }
                        prev = l[idx - 1];
                        rep = 3 + (int)in.get(2);
                    } else if (sym == 17) {
                        rep = 3 + (int)in.get(3);
                    } else {
                        rep = 11 + (int)in.get(7);
                    }
                    if (idx + rep > nlen + ndist) {
                        err = "compressed data is damaged";
                        return false;
                    }
                    while (rep--)
                        l[idx++] = prev;
                }
                if (l[256] == 0) {
                    err = "compressed data is damaged";
                    return false;
                }
                uint8_t dl[32];
                std::memcpy(dl, l + nlen, (size_t)ndist);
                if (!tb.lit.build(l, nlen) || !tb.dist.build(dl, ndist)) {
                    err = "compressed data is damaged";
                    return false;
                }
            }

            for (;;) {
                int sym = tb.lit.decode(in);
                if (sym < 0 || in.overrun) {
                    err = in.overrun ? "compressed data ends too early" : "compressed data is damaged";
                    return false;
                }
                if (sym < 256) {
                    if (out.size() >= max_out) {
                        err = "the unpacked file would be too large";
                        return false;
                    }
                    out.push_back((uint8_t)sym);
                    continue;
                }
                if (sym == 256)
                    break;
                sym -= 257;
                if (sym >= 29) {
                    err = "compressed data is damaged";
                    return false;
                }
                const size_t len = kLenBase[sym] + in.get(kLenExtra[sym]);
                const int dsym = tb.dist.decode(in);
                if (dsym < 0 || dsym >= 30) {
                    err = "compressed data is damaged";
                    return false;
                }
                const size_t dist = kDistBase[dsym] + in.get(kDistExtra[dsym]);
                if (in.overrun) {
                    err = "compressed data ends too early";
                    return false;
                }
                if (dist > out.size()) {
                    err = "compressed data is damaged";
                    return false;
                }
                if (out.size() + len > max_out) {
                    err = "the unpacked file would be too large";
                    return false;
                }
                const size_t at = out.size();
                out.resize(at + len);
                uint8_t* d = out.data() + at;
                const uint8_t* s = d - dist;
                if (dist >= len) {
                    std::memcpy(d, s, len);
                } else {
                    for (size_t i = 0; i < len; i++)
                        d[i] = s[i];
                }
                if (stop_after && out.size() >= stop_after)
                    return true;
            }
        } else {
            err = "compressed data is damaged";
            return false;
        }

        if (stop_after && out.size() >= stop_after)
            return true;
        if (final_block)
            break;
    }
    return true;
}

// ----------------------------------------------------------------------------
// ZIP container
// ----------------------------------------------------------------------------

namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

} // namespace

bool looks_like_zip(const Bytes& file)
{
    return file.size() >= 22 && file[0] == 'P' && file[1] == 'K' &&
           ((file[2] == 3 && file[3] == 4) || (file[2] == 5 && file[3] == 6));
}

bool zip_list(const Bytes& file, std::vector<ZipEntry>& out, std::string& err)
{
    out.clear();
    const size_t n = file.size();
    if (n < 22) {
        err = "not a zip archive";
        return false;
    }
    // End of central directory record: within the last 64 KB + 22 bytes.
    size_t eocd = (size_t)-1;
    const size_t lowest = n > (size_t)(0xFFFF + 22) ? n - (0xFFFF + 22) : 0;
    for (size_t i = n - 22 + 1; i-- > lowest;) {
        if (rd32le(&file[i]) == 0x06054B50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == (size_t)-1) {
        err = "not a zip archive (or it is damaged)";
        return false;
    }
    const uint32_t count = rd16(&file[eocd + 10]);
    const uint32_t cd_size = rd32le(&file[eocd + 12]);
    const uint32_t cd_offset = rd32le(&file[eocd + 16]);
    if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu) {
        err = "ZIP64 archives are not supported";
        return false;
    }
    if ((uint64_t)cd_offset + cd_size > n) {
        err = "the zip archive is damaged";
        return false;
    }
    size_t p = cd_offset;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > n || rd32le(&file[p]) != 0x02014B50u) {
            err = "the zip archive is damaged";
            return false;
        }
        ZipEntry e;
        e.flags = rd16(&file[p + 8]);
        e.method = rd16(&file[p + 10]);
        e.crc32 = rd32le(&file[p + 16]);
        e.packed_size = rd32le(&file[p + 20]);
        e.size = rd32le(&file[p + 24]);
        const size_t name_len = rd16(&file[p + 28]);
        const size_t extra_len = rd16(&file[p + 30]);
        const size_t comment_len = rd16(&file[p + 32]);
        e.header_offset = rd32le(&file[p + 42]);
        if (p + 46 + name_len + extra_len + comment_len > n) {
            err = "the zip archive is damaged";
            return false;
        }
        e.name.assign((const char*)&file[p + 46], name_len);
        p += 46 + name_len + extra_len + comment_len;
        out.push_back(std::move(e));
    }
    return true;
}

bool zip_extract(const Bytes& file, const ZipEntry& e, Bytes& out, std::string& err,
                 size_t max_size, size_t first_bytes)
{
    out.clear();
    const size_t n = file.size();
    if (e.flags & 1) {
        err = "the file inside the zip archive is password protected";
        return false;
    }
    if (e.packed_size == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu) {
        err = "ZIP64 archives are not supported";
        return false;
    }
    if (e.header_offset + 30 > n || rd32le(&file[(size_t)e.header_offset]) != 0x04034B50u) {
        err = "the zip archive is damaged";
        return false;
    }
    const size_t h = (size_t)e.header_offset;
    const size_t data = h + 30 + rd16(&file[h + 26]) + rd16(&file[h + 28]);
    if (data > n || e.packed_size > n - data) {
        err = "the zip archive is damaged";
        return false;
    }
    if (!first_bytes && e.size > max_size) {
        err = "the file inside the zip archive is too large";
        return false;
    }
    const uint8_t* src = &file[0] + data;
    const size_t packed = (size_t)e.packed_size;

    if (e.method == 0) {
        size_t take = packed;
        if (first_bytes && take > first_bytes)
            take = first_bytes;
        out.assign(src, src + take);
    } else if (e.method == 8) {
        const size_t limit = first_bytes ? first_bytes + 0x10000 : (size_t)e.size;
        if (!inflate_raw(src, packed, out, err, limit, first_bytes, first_bytes ? 0 : (size_t)e.size))
            return false;
    } else {
        err = "the zip archive uses a compression method that is not supported (only deflate is)";
        return false;
    }
    if (first_bytes)
        return true;
    if (out.size() != e.size || crc32_of(out.data(), out.size()) != e.crc32) {
        err = "the file inside the zip archive is damaged (checksum mismatch)";
        return false;
    }
    return true;
}

std::string name_extension(const std::string& name)
{
    const size_t slash = name.find_last_of("/\\");
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return std::string();
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return ext;
}

} // namespace usfm
