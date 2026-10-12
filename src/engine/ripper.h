// USF Maker - the automatic ripper.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
//
// How a ROM becomes a USF set without a human in the loop:
//
//  1. Explore.  The cartridge is booted in the emulator core with no video.
//     A scripted "player" presses buttons to get through title screens and
//     menus.  Whenever the sound output goes from silence to sound, the save
//     state taken just before that moment becomes a candidate song, and also
//     a branching point for further exploration.
//  2. Finalize.  Each candidate is loaded exactly the way a USF player would
//     load it (no controller, stock emulation settings) and rendered.  If it
//     really plays, is not something we already have, and is long enough to
//     be music, it is analysed for its loop point or ending, and the parts of
//     the cartridge it touched while playing are recorded.
//  3. Write.  The union of the touched cartridge data goes into one .usflib;
//     every song becomes a .miniusf with its save state and tags.
#pragma once

#include "audio.h"
#include "common.h"
#include "machine.h"
#include "rom.h"
#include "usffile.h"

#include <atomic>
#include <memory>

namespace usfm {

struct RipOptions {
    // --- emulation settings; they end up as tags in the files ---
    bool enable_compare = true;
    bool enable_fifo_full = true;
    uint32_t rdram_size = 0x800000;       // 0x400000 = no Expansion Pak

    // --- exploration ---
    bool press_buttons = true;            // false: only listen to what the game does on its own
    double search_seconds = 300;          // wall clock time to spend exploring (0 = until cancelled)
    double idle_boot_seconds = 100;       // how long to watch the untouched game after power-on
    double episode_seconds = 40;          // emulated time per exploration run
    int threads = 0;                      // 0 = one per CPU core (at most 8)
    uint64_t seed = 0x5EED;
    int max_songs = 200;
    bool save_memory = true;              // give the game blank EEPROM / SRAM / FlashRAM while exploring
    int eeprom_kbit = 0;                  // 4 or 16; 0 = decide by the game's product code

    // --- song analysis ---
    double max_render_seconds = 480;      // how much of a song to render when looking for its loop
    double default_length_seconds = 180;  // used when neither a loop nor an ending is found
    double fade_seconds = 10;             // fade for looping songs
    int loop_count = 2;                   // how many times a looping song plays before the fade
    double min_song_seconds = 1.5;        // shorter sounds are treated as sound effects
};

enum class SongKind {
    Looping,    // loop found: length = intro + loop_count * loop
    Ending,     // the music stops by itself
    Unknown,    // no loop or ending found within max_render_seconds
};

enum class SongOrigin {
    BootIdle,   // heard without touching the controller
    Explored,   // heard while the automatic player was pressing buttons
    Imported,   // made from a save state the user supplied
    SoundTest,  // played by walking through the game's sound test screen
};

struct SongInfo {
    int id = 0;
    std::string title;
    SongKind kind = SongKind::Unknown;
    SongOrigin origin = SongOrigin::BootIdle;
    double length_seconds = 0;
    double fade_seconds = 0;
    double intro_seconds = 0;
    double loop_seconds = 0;
    uint32_t sample_rate = 0;
    double found_at_seconds = 0;          // emulated time since power-on when it was first heard
    size_t state_bytes = 0;
    bool hle_ok = true;                   // false: uses audio microcode the fast (HLE) path does not know
    bool stalls = false;                  // the game stops producing sound after the tagged length
    int order = 0;                        // position in the list (sound test entries come in menu order)
};

struct RipProgress {
    double elapsed_seconds = 0;           // wall clock
    double budget_seconds = 0;
    double explored_seconds = 0;          // emulated time covered by exploration so far
    int candidates = 0;                   // song starts noticed
    int duplicates = 0;
    int rejected = 0;                     // silent / too short / did not play
    int songs = 0;
    int pending = 0;                      // candidates waiting for analysis
    bool exploring = false;
    bool finished = false;
    // sound test walk (run_sound_test)
    int st_phase = 0;                     // 0 not running, 1 finding out the controls, 2 walking the list
    int st_entry = 0;                     // entry being tried (1 = the one selected in the save state)
    std::string st_keys;                  // controls in use, e.g. "D-Pad Right, then A"
};

struct SoundTestResult {
    bool controls_found = false;
    bool cancelled = false;
    std::string keys;                     // "<select>, then <play>"
    int entries = 0;                      // entries tried
    int sounds = 0;                       // entries that produced a new sound
};

struct SetTags {
    std::string game;
    std::string artist;
    std::string year;
    std::string copyright;
    std::string usfby;
    std::string comment;
};

struct WriteReport {
    std::vector<Path> files;
    size_t lib_bytes = 0;
    size_t rom_bytes = 0;                 // cartridge data stored in the library
    size_t total_bytes = 0;
};

class RipListener {
public:
    virtual ~RipListener() = default;
    // All of these are called from worker threads.
    virtual void on_log(const std::string& utf8) { (void)utf8; }
    virtual void on_progress(const RipProgress& p) { (void)p; }
    virtual void on_song_added(const SongInfo& song) { (void)song; }
    virtual void on_song_removed(int id) { (void)id; }
};

class Ripper {
public:
    Ripper();
    ~Ripper();
    Ripper(const Ripper&) = delete;
    Ripper& operator=(const Ripper&) = delete;

    void set_rom(std::shared_ptr<const Rom> rom);
    void set_options(const RipOptions& opt);
    const RipOptions& options() const;
    void set_listener(RipListener* listener);

    // Explores the game.  Blocks until the time budget is used up or cancel()
    // is called; run it on its own thread.  Songs found so far stay available
    // afterwards and a later run() continues to add to them.
    void run();
    void cancel();
    bool running() const;

    RipProgress progress() const;
    std::vector<SongInfo> songs() const;
    bool rename_song(int id, const std::string& title);
    bool remove_song(int id);
    void clear_songs();

    // Makes a song out of a Project64 save state taken while the wanted music
    // was starting.  Runs the same checks and analysis as an automatic find.
    bool add_from_state(const Bytes& pj64_state, const std::string& title, int* new_id, std::string& err);

    // "Sound test" mode.  `pj64_state` is a save state taken on the game's
    // sound test / music player screen.  The controls are found out by
    // trying (which button moves to the next entry, which one plays it),
    // then the list is walked entry by entry and everything that starts to
    // play becomes a song, in menu order.  Blocks until the end of the list
    // (it wraps around or stops moving) or cancel(); run it on its own
    // thread like run().  Returns false with `err` set when nothing could be
    // done; result.controls_found tells whether the screen reacted at all.
    bool run_sound_test(const Bytes& pj64_state, SoundTestResult& result, std::string& err);

    // Sets `m` up to play song `id` the way a USF player would.
    bool open_player(int id, Machine& m, std::string& err) const;

    // Writes <game>.usflib and one .miniusf per listed song into `dir`.
    bool write_set(const Path& dir, const SetTags& tags, const std::vector<int>& ids, bool full_rom,
                   WriteReport& report, std::string& err) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace usfm
