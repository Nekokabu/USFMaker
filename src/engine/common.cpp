// USF Maker - small shared helpers.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "common.h"

#include <cstdio>
#include <fstream>

namespace usfm {

bool read_file(const Path& path, Bytes& out, std::string& err, size_t max_size)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        err = "cannot open file: " + path_to_utf8(path);
        return false;
    }
    if (size > max_size) {
        err = "file is too large: " + path_to_utf8(path);
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open file: " + path_to_utf8(path);
        return false;
    }
    out.resize((size_t)size);
    if (size && !f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)size)) {
        err = "cannot read file: " + path_to_utf8(path);
        return false;
    }
    return true;
}

bool write_file(const Path& path, const void* data, size_t size, std::string& err)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot create file: " + path_to_utf8(path);
        return false;
    }
    if (size)
        f.write(static_cast<const char*>(data), (std::streamsize)size);
    f.close();
    if (!f) {
        err = "cannot write file: " + path_to_utf8(path);
        return false;
    }
    return true;
}

Path path_from_utf8(const std::string& s)
{
#if defined(__cpp_lib_char8_t)
    return Path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

std::string path_to_utf8(const Path& p)
{
#if defined(__cpp_lib_char8_t)
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

std::string sanitize_filename(const std::string& utf8)
{
    std::string out;
    out.reserve(utf8.size());
    for (unsigned char c : utf8) {
        switch (c) {
        case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
            out.push_back('_');
            break;
        default:
            out.push_back(c < 0x20 ? '_' : (char)c);
        }
    }
    // Windows does not like names that end in a dot or a space
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    if (out.empty())
        out = "_";
    return out;
}

std::string format_time_tag(uint64_t ms)
{
    char buf[64];
    const unsigned long long total_s = ms / 1000;
    std::snprintf(buf, sizeof(buf), "%llu:%02llu.%03llu", total_s / 60, total_s % 60, (unsigned long long)(ms % 1000));
    return buf;
}

std::string format_time_short(double seconds)
{
    if (seconds < 0)
        seconds = 0;
    const unsigned long long s = (unsigned long long)(seconds + 0.5);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu", s / 60, s % 60);
    return buf;
}

} // namespace usfm
