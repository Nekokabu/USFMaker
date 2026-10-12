// USF Maker - command line front end.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "engine/ripper.h"
#include "engine/statefile.h"
#include "engine/wav.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace usfm;

namespace {

class ConsoleListener : public RipListener {
public:
    bool verbose = false;
    void on_log(const std::string& s) override
    {
        if (!verbose && (s.compare(0, 8, "explore:") == 0 || s.compare(0, 11, "sound test:") == 0))
            return;
        std::lock_guard<std::mutex> lk(mu_);
        std::printf("  %s\n", s.c_str());
        std::fflush(stdout);
    }
    void on_progress(const RipProgress& p) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int sec = (int)p.elapsed_seconds;
        if (p.st_phase == 2 && !p.finished) {
            if (p.st_entry == last_entry_)
                return;
            last_entry_ = p.st_entry;
            std::printf("[%4ds] sound test entry %d (%s): songs %d, waiting %d\n", sec, p.st_entry, p.st_keys.c_str(), p.songs, p.pending);
            std::fflush(stdout);
            return;
        }
        if (p.st_phase == 1 && !p.finished) {
            if (last_entry_ != -2)
                std::printf("finding out how the sound test screen is operated...\n");
            last_entry_ = -2;
            std::fflush(stdout);
            return;
        }
        if (sec / 10 == last_tick_ && !p.finished)
            return;
        last_tick_ = sec / 10;
        std::printf("[%4ds] songs %d, candidates %d (duplicates %d, rejected %d, waiting %d), explored %.0f s of game time\n",
                    sec, p.songs, p.candidates, p.duplicates, p.rejected, p.pending, p.explored_seconds);
        std::fflush(stdout);
    }
    void on_song_added(const SongInfo& s) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        const char* kind = s.kind == SongKind::Looping ? "loop" : s.kind == SongKind::Ending ? "ends" : "unknown";
        std::printf("+ %-12s %s (+%.0fs fade)  %-7s", s.title.c_str(), format_time_short(s.length_seconds).c_str(), s.fade_seconds, kind);
        if (s.kind == SongKind::Looping)
            std::printf(" intro %.2fs loop %.2fs", s.intro_seconds, s.loop_seconds);
        std::printf("  [%u Hz, heard at %.0fs%s%s]\n", s.sample_rate, s.found_at_seconds,
                    s.hle_ok ? "" : ", needs LLE audio", s.stalls ? ", stalls" : "");
        std::fflush(stdout);
    }
    void on_song_removed(int id) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::printf("- song %d replaced by a better capture\n", id);
    }

private:
    std::mutex mu_;
    int last_tick_ = -1;
    int last_entry_ = -1;
};

class WavSink : public AudioSink {
public:
    std::vector<int16_t> pcm;
    uint32_t rate = 0;
    void on_audio(const int16_t* s, size_t frames, uint32_t r) override
    {
        rate = r;
        pcm.insert(pcm.end(), s, s + frames * 2);
    }
};

void usage()
{
    std::printf(
        "USF Maker (command line)\n"
        "\n"
        "  usfmaker-cli <rom> [options]             find the music in a ROM and write a USF set\n"
        "  usfmaker-cli --render <usf> <wav> [sec]  play a .usf/.miniusf into a WAV file\n"
        "\n"
        "The ROM may be a .z64, .n64 or .v64 image, plain or inside a .zip archive.\n"
        "\n"
        "options:\n"
        "  -o <folder>        where to write the set (default: <rom name> next to the ROM)\n"
        "  --time <sec>       how long to search, in seconds of real time (default 300)\n"
        "  --no-buttons       do not press any buttons, only listen to the untouched game\n"
        "  --state <file>     also make a song from this Project64 save state (.pj or\n"
        "                     .pj.zip; repeatable; with --time 0 nothing else is searched)\n"
        "  --sound-test <file> a save state taken on the game's sound test screen: finds\n"
        "                     out the controls and plays every entry in turn (the\n"
        "                     game is then only explored too if --time is given)\n"
        "  --game <name>      game name for the tags and file names\n"
        "  --artist <name>    artist tag\n"
        "  --threads <n>      worker threads (default: one per core, at most 8)\n"
        "  --full-rom         store the whole ROM in the .usflib instead of only what is read\n"
        "  --4mb              emulate a console without Expansion Pak\n"
        "  --eeprom <4|16>    size of the cartridge EEPROM in kbit (default: by game)\n"
        "  --no-save-memory   explore without EEPROM / SRAM / FlashRAM in the cartridge\n"
        "  --loops <n>        loops before the fade (default 2)\n"
        "  --fade <sec>       fade length for looping songs (default 10)\n"
        "  --seed <n>         change the sequence of automatic button presses\n"
        "  --wav              also write a WAV preview of every song\n"
        "  -v                 verbose\n");
}

int render_usf(const std::string& in, const std::string& out, double seconds)
{
    UsfImage img;
    if (!load_usf(path_from_utf8(in), img)) {
        std::printf("error: %s\n", img.error.c_str());
        return 1;
    }
    uint64_t len_ms = 0, fade_ms = 0;
    parse_time_tag(find_tag(img.tags, "length"), len_ms);
    parse_time_tag(find_tag(img.tags, "fade"), fade_ms);
    if (seconds <= 0)
        seconds = len_ms ? (double)(len_ms + fade_ms) / 1000.0 : 180.0;

    MachineConfig cfg;
    cfg.enable_compare = img.enable_compare;
    cfg.enable_fifo_full = img.enable_fifo_full;
    Machine m;
    std::string err;
    if (!m.load_state(img.rom_words, img.state, cfg, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    WavSink sink;
    m.set_audio_sink(&sink);
    double idle_since = 0;
    size_t last = 0;
    while (sink.rate == 0 || sink.pcm.size() / 2 < (size_t)(seconds * sink.rate)) {
        const int r = m.run(kStopAudio | kStopVI);
        if (r & kStopError) {
            std::printf("emulation stopped: %s\n", m.error().c_str());
            break;
        }
        if (sink.pcm.size() != last) {
            last = sink.pcm.size();
            idle_since = m.seconds();
        } else if (m.seconds() - idle_since > 10.0) {
            std::printf("no audio for 10 seconds, giving up\n");
            break;
        }
    }
    if (len_ms && sink.rate) {
        // apply the fade the tags ask for
        const size_t start = (size_t)((double)len_ms / 1000.0 * sink.rate);
        const size_t n = (size_t)((double)fade_ms / 1000.0 * sink.rate);
        for (size_t i = start; i < sink.pcm.size() / 2; i++) {
            const double g = (n && i < start + n) ? 1.0 - (double)(i - start) / (double)n : 0.0;
            sink.pcm[i * 2] = (int16_t)(sink.pcm[i * 2] * g);
            sink.pcm[i * 2 + 1] = (int16_t)(sink.pcm[i * 2 + 1] * g);
        }
    }
    if (!write_wav(path_from_utf8(out), sink.pcm.data(), sink.pcm.size() / 2, sink.rate, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s: %.1f s at %u Hz\n", out.c_str(), sink.rate ? (double)(sink.pcm.size() / 2) / sink.rate : 0.0, sink.rate);
    return 0;
}

int run(std::vector<std::string> args)
{
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--render") {
        if (args.size() < 3) {
            usage();
            return 2;
        }
        return render_usf(args[1], args[2], args.size() > 3 ? std::atof(args[3].c_str()) : 0);
    }

    std::string rom_path, out_dir;
    std::vector<std::string> states, sound_tests;
    bool time_given = false;
    RipOptions opt;
    SetTags tags;
    bool full_rom = false, verbose = false, want_wav = false;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                std::printf("missing value after %s\n", a.c_str());
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "-o") out_dir = next();
        else if (a == "--time") { opt.search_seconds = std::atof(next().c_str()); time_given = true; }
        else if (a == "--sound-test") sound_tests.push_back(next());
        else if (a == "--no-buttons") opt.press_buttons = false;
        else if (a == "--state") states.push_back(next());
        else if (a == "--game") tags.game = next();
        else if (a == "--artist") tags.artist = next();
        else if (a == "--threads") opt.threads = std::atoi(next().c_str());
        else if (a == "--full-rom") full_rom = true;
        else if (a == "--4mb") opt.rdram_size = 0x400000;
        else if (a == "--eeprom") opt.eeprom_kbit = std::atoi(next().c_str()) >= 16 ? 16 : 4;
        else if (a == "--no-save-memory") opt.save_memory = false;
        else if (a == "--loops") opt.loop_count = std::atoi(next().c_str());
        else if (a == "--fade") opt.fade_seconds = std::atof(next().c_str());
        else if (a == "--seed") opt.seed = std::strtoull(next().c_str(), nullptr, 0);
        else if (a == "--idle") opt.idle_boot_seconds = std::atof(next().c_str());
        else if (a == "--max-render") opt.max_render_seconds = std::atof(next().c_str());
        else if (a == "--wav") want_wav = true;
        else if (a == "-v") verbose = true;
        else if (!a.empty() && a[0] == '-') {
            std::printf("unknown option %s\n", a.c_str());
            return 2;
        } else if (rom_path.empty()) rom_path = a;
        else {
            std::printf("unexpected argument %s\n", a.c_str());
            return 2;
        }
    }
    if (rom_path.empty()) {
        usage();
        return 2;
    }

    auto rom = std::make_shared<Rom>();
    std::string err;
    if (!rom->load(path_from_utf8(rom_path), err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    const Path rom_file = path_from_utf8(rom_path);
    if (tags.game.empty())
        tags.game = path_to_utf8(rom_file.stem());
    const Path out = out_dir.empty() ? rom_file.parent_path() / path_from_utf8(sanitize_filename(tags.game) + " (USF)") : path_from_utf8(out_dir);

    ConsoleListener listener;
    listener.verbose = verbose;
    Ripper ripper;
    ripper.set_rom(rom);
    ripper.set_options(opt);
    ripper.set_listener(&listener);

    for (const std::string& sp : states) {
        Bytes st;
        std::string title = state_title_from_path(path_from_utf8(sp));
        if (!load_pj64_state(path_from_utf8(sp), st, err)) {
            std::printf("state %s: %s\n", sp.c_str(), err.c_str());
            continue;
        }
        if (!ripper.add_from_state(st, title, nullptr, err))
            std::printf("state %s: %s\n", sp.c_str(), err.c_str());
    }

    for (const std::string& sp : sound_tests) {
        Bytes st;
        if (!load_pj64_state(path_from_utf8(sp), st, err)) {
            std::printf("state %s: %s\n", sp.c_str(), err.c_str());
            continue;
        }
        SoundTestResult res;
        if (ripper.run_sound_test(st, res, err))
            std::printf("sound test %s: operated with %s; %d entries tried, %d gave a new sound\n", sp.c_str(), res.keys.c_str(), res.entries, res.sounds);
        else if (!res.cancelled) {
            // not a sound test after all: use it the ordinary way
            std::printf("sound test %s: %s; adding it as an ordinary save state\n", sp.c_str(), err.c_str());
            if (!ripper.add_from_state(st, state_title_from_path(path_from_utf8(sp)), nullptr, err))
                std::printf("state %s: %s\n", sp.c_str(), err.c_str());
        }
    }

    // With a sound test state the game is only explored as well when asked to.
    const bool explore = sound_tests.empty() ? (opt.search_seconds > 0 || states.empty()) : (time_given && opt.search_seconds > 0);
    if (explore)
        ripper.run();

    const std::vector<SongInfo> songs = ripper.songs();
    if (songs.empty()) {
        std::printf("no music found\n");
        return 1;
    }
    std::vector<int> ids;
    for (const SongInfo& s : songs)
        ids.push_back(s.id);
    WriteReport rep;
    if (!ripper.write_set(out, tags, ids, full_rom, rep, err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %u files to %s (%.1f MB total; %.1f MB of the %.1f MB ROM kept)\n", (unsigned)rep.files.size(), path_to_utf8(out).c_str(),
                (double)rep.total_bytes / 1048576.0, (double)rep.rom_bytes / 1048576.0, (double)rom->info().size / 1048576.0);

    if (want_wav) {
        for (size_t i = 1; i < rep.files.size(); i++) {
            const std::string in = path_to_utf8(rep.files[i]);
            Path w = rep.files[i];
            w.replace_extension(".wav");
            render_usf(in, path_to_utf8(w), 0);
        }
    }
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], n, nullptr, nullptr);
        args.push_back(s);
    }
    return run(args);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++)
        args.push_back(argv[i]);
    return run(args);
}
#endif
