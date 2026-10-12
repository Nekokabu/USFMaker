// USF Maker - the automatic ripper.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "ripper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "../lazyusf2/usf/usfx.h"

namespace usfm {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// --------------------------------------------------------------------------
// Save states are 8 MB each and we keep many of them around; most of a
// state is zeros, so store only the non-zero runs.
// --------------------------------------------------------------------------
struct PackedState {
    uint32_t size = 0;
    std::vector<SparseBlock> blocks;
    Bytes data;

    static PackedState pack(const Bytes& raw)
    {
        PackedState p;
        p.size = (uint32_t)raw.size();
        p.blocks = blocks_nonzero(raw.data(), raw.size(), 16);
        p.data.reserve(blocks_total(p.blocks));
        for (const SparseBlock& b : p.blocks)
            p.data.insert(p.data.end(), raw.begin() + b.offset, raw.begin() + b.offset + b.length);
        return p;
    }

    Bytes unpack() const
    {
        Bytes raw(size, 0);
        size_t pos = 0;
        for (const SparseBlock& b : blocks) {
            std::memcpy(raw.data() + b.offset, data.data() + pos, b.length);
            pos += b.length;
        }
        return raw;
    }

    size_t bytes() const { return data.size() + blocks.size() * sizeof(SparseBlock); }
    bool empty() const { return size == 0; }

    // FNV-1a over the contents; equal states give equal hashes.
    uint64_t hash() const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const uint8_t* p, size_t n) {
            for (size_t i = 0; i < n; i++) {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        };
        for (const SparseBlock& b : blocks) {
            mix(reinterpret_cast<const uint8_t*>(&b.offset), 4);
            mix(reinterpret_cast<const uint8_t*>(&b.length), 4);
        }
        // sampling every 16th byte is plenty to tell machine states apart
        for (size_t i = 0; i < data.size(); i += 16) {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    }
};

// --------------------------------------------------------------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    // uniform in [lo, hi]
    int range(int lo, int hi) { return lo + (int)(next() % (uint64_t)(hi - lo + 1)); }
    double unit() { return (double)(next() >> 11) / 9007199254740992.0; }
};

// --------------------------------------------------------------------------
// The automatic player.  It can not see the screen, so it does not try to be
// clever: each "style" is a plausible way of pressing buttons that tends to
// move a game forward (skip a logo, confirm a menu, walk around).  Which
// style is useful where is found out by trying; see Impl::pick_node().
// --------------------------------------------------------------------------
enum BotStyle { kBotIdle = 0, kBotStart, kBotConfirm, kBotMenu, kBotPlay, kBotMash, kBotStyleCount };

const char* bot_style_name(int s)
{
    switch (s) {
    case kBotIdle: return "idle";
    case kBotStart: return "start";
    case kBotConfirm: return "confirm";
    case kBotMenu: return "menu";
    case kBotPlay: return "play";
    case kBotMash: return "mash";
    default: return "?";
    }
}

class InputBot {
public:
    InputBot(uint64_t seed, int style, int frames_per_second)
        : rng_(seed), style_(style), fps_(frames_per_second > 0 ? frames_per_second : 60)
    {
        wait_ = rng_.range(fps_ / 4, fps_);
    }

    void set_style(int style)
    {
        style_ = style;
        hold_ = 0;
        cur_ = 0;
        held_dir_ = 0;
        dir_left_ = 0;
    }

    // called once per video frame
    uint32_t next()
    {
        if (style_ == kBotIdle)
            return 0;

        uint32_t out = 0;

        // a direction that is held down for a while (walking around)
        if (style_ == kBotPlay || style_ == kBotMash) {
            if (dir_left_ <= 0) {
                held_dir_ = rng_.range(0, 8);
                dir_left_ = rng_.range(fps_ / 2, fps_ * 3);
            }
            --dir_left_;
            out |= direction(held_dir_, true);
        }

        if (hold_ > 0) {
            --hold_;
            return out | cur_;
        }
        cur_ = 0;
        if (wait_ > 0) {
            --wait_;
            return out;
        }

        // time for the next tap
        switch (style_) {
        case kBotStart:
            cur_ = kBtnStart;
            wait_ = rng_.range(fps_ * 3 / 2, fps_ * 3);
            break;
        case kBotConfirm:
            cur_ = (rng_.range(0, 9) < 8) ? kBtnA : kBtnStart;
            wait_ = rng_.range(fps_ / 2, fps_ * 3 / 2);
            break;
        case kBotMenu: {
            const int r = rng_.range(0, 99);
            if (r < 40) cur_ = kBtnA;
            else if (r < 48) cur_ = kBtnB;
            else if (r < 56) cur_ = kBtnStart;
            else cur_ = direction(1 + (rng_.range(0, 3) * 2), false); // up, right, down, left
            wait_ = rng_.range(fps_ / 3, fps_);
            break;
        }
        case kBotPlay: {
            static const uint32_t buttons[] = { kBtnA, kBtnA, kBtnB, kBtnZ, kBtnR, kBtnCUp, kBtnCDown, kBtnCLeft, kBtnCRight, kBtnStart };
            cur_ = buttons[rng_.range(0, 9)];
            wait_ = rng_.range(fps_ / 6, fps_);
            break;
        }
        case kBotMash:
        default: {
            static const uint32_t buttons[] = { kBtnA, kBtnB, kBtnZ, kBtnStart, kBtnL, kBtnR, kBtnCUp, kBtnCDown, kBtnCLeft, kBtnCRight };
            cur_ = buttons[rng_.range(0, 9)];
            if (rng_.range(0, 3) == 0)
                cur_ |= buttons[rng_.range(0, 9)];
            wait_ = rng_.range(2, fps_ / 4);
            break;
        }
        }
        hold_ = rng_.range(3, 6);
        return out | cur_;
    }

private:
    // 0 = neutral, 1..8 = eight directions starting at "up", clockwise
    static uint32_t direction(int d, bool analog_only)
    {
        static const int dx[9] = { 0, 0, 1, 1, 1, 0, -1, -1, -1 };
        static const int dy[9] = { 0, 1, 1, 0, -1, -1, -1, 0, 1 };
        if (d <= 0 || d > 8)
            return 0;
        uint32_t b = stick(dx[d] * 80, dy[d] * 80);
        if (!analog_only) {
            if (dy[d] > 0) b |= kBtnDUp;
            if (dy[d] < 0) b |= kBtnDDown;
            if (dx[d] > 0) b |= kBtnDRight;
            if (dx[d] < 0) b |= kBtnDLeft;
        }
        return b;
    }

    Rng rng_;
    int style_;
    int fps_;
    int wait_ = 0, hold_ = 0;
    uint32_t cur_ = 0;
    int held_dir_ = 0, dir_left_ = 0;
};

// --------------------------------------------------------------------------
struct Node {
    int id = 0;
    bool root = false;            // power-on, has no state
    PackedState state;
    std::shared_ptr<const Bytes> cart;   // cartridge save memory at that moment (null: blank)
    double game_time = 0;         // emulated seconds since power-on
    int depth = 0;
    int visits = 0;
    double novelty = 0;           // cartridge bytes first seen on the way here
    double yield = 0;             // new bytes / songs found by runs that started here
    int crashes = 0;
};

struct Candidate {
    PackedState state;
    std::shared_ptr<const Bytes> cart;
    double game_time = 0;
    SongOrigin origin = SongOrigin::BootIdle;
    std::string title;            // given by the user or by the sound test walk; empty: number it
    int chain = 0;                // how many "what comes next" steps led here
    int order = -1;               // position in the song list; -1: append
};

// "Keep listening from this state": used when a song is followed by more
// sound after a pause, to find out what that is.
struct ListenJob {
    PackedState state;
    std::shared_ptr<const Bytes> cart;
    double game_time = 0;
    double seconds = 0;
    SongOrigin origin = SongOrigin::BootIdle;
    int chain = 0;
};

struct SongRec {
    SongInfo info;
    PackedState state;
    std::vector<SparseBlock> rom_blocks;   // cartridge data read while playing
    FeatureTrack features;
    double first_sound = 0;
    double probe_seconds = 10;
    bool auto_title = true;                // still called "Track NN" after the order it was found in
};

struct InProgress {
    FeatureTrack features;
    double first_sound = 0;
    double probe_seconds = 10;
};

// Collects everything a running machine tells us.
class Listener : public AudioSink {
public:
    FeatureExtractor fx;
    SilenceMeter meter;
    bool want_features = true;
    void on_audio(const int16_t* stereo, size_t frames, uint32_t rate) override
    {
        meter.feed(stereo, frames, rate);
        if (want_features)
            fx.feed(stereo, frames, rate);
    }
};

// True if the pause `end` describes is there again one loop period later.
bool pause_recurs(const FeatureTrack& t, const EndResult& end, double period)
{
    if (!end.found || end.resume_seconds < 0 || period <= 0)
        return false;
    const size_t a = (size_t)((end.end_seconds + period) * kFeatRate);
    const size_t b = (size_t)((end.resume_seconds + period) * kFeatRate);
    if (b <= a || b > t.frames())
        return false;   // not rendered that far, or nothing to compare
    size_t silent = 0;
    for (size_t i = a; i < b; i++)
        silent += t.silent(i) ? 1u : 0u;
    return silent * 10 >= (b - a) * 7;
}

// A newly found piece counts as "already have it" when it starts somewhere
// inside the part of an existing song that will actually be played.  For a
// song that ends, that stops a little before its end: whatever starts after
// the closing silence is a different piece.
double dup_limit(const SongInfo& s)
{
    if (s.kind == SongKind::Ending)
        return std::max(0.0, s.length_seconds - 1.5);
    return s.length_seconds + s.fade_seconds;
}

size_t popcount_new(const uint8_t* local, uint8_t* global, size_t bytes)
{
    // counts bits set in local but not yet in global, and merges them
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t fresh = (uint8_t)(local[i] & ~global[i]);
        if (fresh) {
            global[i] |= fresh;
            while (fresh) {
                fresh &= (uint8_t)(fresh - 1);
                ++n;
            }
        }
    }
    return n;
}

// ---- sound test walk ------------------------------------------------------
struct StKey {
    uint32_t buttons;
    const char* name;
};

// How a sound test screen is operated: one key moves to the next entry and
// one starts it.  play.buttons == 0 means that moving to an entry plays it.
struct StScheme {
    StKey change;
    StKey play;
};

struct StStep {
    bool reacted = false;      // the key press changed what is heard
    bool captured = false;     // ... and a save state was found that plays the new sound by itself
    Bytes state;
    FeatureTrack print;        // the first seconds of the new sound
};

class PcmSink : public AudioSink {
public:
    std::vector<int16_t> pcm;  // interleaved
    uint32_t rate = 0;
    void on_audio(const int16_t* stereo, size_t frames, uint32_t r) override
    {
        rate = r;
        pcm.insert(pcm.end(), stereo, stereo + frames * 2);
    }
    size_t frames() const { return pcm.size() / 2; }
};

const int kStHold = 5;             // video frames a key is held
const int kStSettle = 24;          // frames to let a cursor movement finish
const double kStWindow = 2.0;      // seconds to wait for a key press to become audible
const double kStListen = 3.0;      // seconds of the new sound to record
const double kStSame = 0.80;       // similarity above which two entries are the same sound
const int kStMaxEntries = 600;

const double kOnsetGap = 0.25;        // silence after which new sound is looked at as a possible new song
const double kEndGap = 0.6;           // silence that ends a song (unless it turns out to be part of a loop)
const double kSilentSnapEvery = 1.0 / 15;
const double kCheckpointEvery = 10.0;

} // namespace

// ==========================================================================

struct Ripper::Impl {
    std::shared_ptr<const Rom> rom;
    RipOptions opt;
    RipListener* listener = nullptr;

    mutable std::mutex mu;
    std::condition_variable cv;

    std::vector<std::shared_ptr<SongRec>> songs;
    std::vector<std::shared_ptr<Node>> nodes;
    std::deque<Candidate> queue;
    std::deque<ListenJob> listen_queue;
    std::vector<std::shared_ptr<InProgress>> in_progress;
    std::vector<uint64_t> seen_states;     // hashes of every candidate state so far
    Bytes global_cover;

    std::atomic<bool> cancel{ false };
    std::atomic<bool> is_running{ false };
    int active = 0;
    bool stop_workers = false;
    bool idle_boot_done = false;
    bool idle_boot_taken = false;
    bool st_mode = false;          // a sound test walk is running: workers only analyse what it queues
    bool st_walking = false;       // ... and it may still queue more
    int next_song_id = 1;
    int next_node_id = 1;
    uint64_t job_counter = 0;
    Clock::time_point t_start;
    RipProgress prog;

    // ---------------------------------------------------------------- utils
    void log(const std::string& s)
    {
        if (listener)
            listener->on_log(s);
    }

    void logf(const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
    ;

    MachineConfig capture_config() const
    {
        MachineConfig c;
        c.enable_compare = opt.enable_compare;
        c.enable_fifo_full = opt.enable_fifo_full;
        c.rdram_size = opt.rdram_size;
        // Capture with the real microcode: save states made that way play
        // correctly in both HLE and LLE players, while the reverse is not
        // true for every game.
        c.hle_audio = false;
        c.controller = true;
        c.fix_code_mirrors = true;
        c.track_rom = true;
        // A new cartridge's blank save memory.  Players have none, but they
        // start from a save state taken after the game has done its checks.
        c.cart_saves = opt.save_memory;
        c.eeprom_16k = opt.eeprom_kbit == 16 || (opt.eeprom_kbit == 0 && rom && rom->info().eeprom_16k);
        return c;
    }

    MachineConfig player_config() const
    {
        MachineConfig c;
        c.enable_compare = opt.enable_compare;
        c.enable_fifo_full = opt.enable_fifo_full;
        c.rdram_size = opt.rdram_size;
        c.hle_audio = true;      // what players do by default
        c.controller = false;
        c.fix_code_mirrors = false;
        c.track_rom = true;
        return c;
    }

    bool budget_over() const
    {
        return opt.search_seconds > 0 && seconds_since(t_start) >= opt.search_seconds;
    }

    // ---------------------------------------------------------------- nodes
    std::shared_ptr<Node> add_node_locked(const Bytes& raw, double game_time, int depth, double novelty,
                                          std::shared_ptr<const Bytes> cart)
    {
        auto n = std::make_shared<Node>();
        n->id = next_node_id++;
        n->state = PackedState::pack(raw);
        n->cart = std::move(cart);
        n->game_time = game_time;
        n->depth = depth;
        n->novelty = novelty;
        nodes.push_back(n);

        // keep memory bounded: drop the least promising visited nodes
        size_t total = 0;
        for (const auto& k : nodes)
            total += k->state.bytes();
        const size_t limit = (size_t)320 << 20;
        while (total > limit && nodes.size() > 8) {
            size_t worst = 0;
            double worst_w = 1e300;
            for (size_t i = 0; i < nodes.size(); i++) {
                if (nodes[i]->root || nodes[i] == n)
                    continue;
                const double w = weight(*nodes[i]);
                if (w < worst_w) {
                    worst_w = w;
                    worst = i;
                }
            }
            if (worst_w == 1e300)
                break;
            total -= nodes[worst]->state.bytes();
            nodes.erase(nodes.begin() + (std::ptrdiff_t)worst);
        }
        return n;
    }

    static double weight(const Node& n)
    {
        // New territory (cartridge data nobody had read before) is the best
        // sign that a different part of the game was reached.
        const double base = 32.0 * 1024;
        const double avg_yield = n.visits ? n.yield / n.visits : n.novelty;
        double w = (base + n.novelty * 0.5 + avg_yield) / ((1.0 + n.visits) * (1.0 + n.visits));
        w /= (1.0 + 2.0 * n.crashes);
        if (n.root)
            w = std::max(w, base * 4 / (1.0 + n.visits));
        return w;
    }

    std::shared_ptr<Node> pick_node_locked(Rng& rng)
    {
        double total = 0;
        for (const auto& n : nodes)
            total += weight(*n);
        double r = rng.unit() * total;
        for (const auto& n : nodes) {
            r -= weight(*n);
            if (r <= 0)
                return n;
        }
        return nodes.empty() ? nullptr : nodes.back();
    }

    // ------------------------------------------------------------- explore
    // listen != nullptr: no buttons, just wait and hear what the game does next
    void explore(std::shared_ptr<Node> node, uint64_t seed, bool idle_boot, const ListenJob* listen = nullptr);
    // ------------------------------------------------------------ finalize
    enum class Verdict { Added, Duplicate, Rejected, Cancelled, Failed };
    Verdict finalize(const Candidate& cand, int* new_id, std::string* why);

    void worker(int index);
    void emit_progress(bool finished);

    // sound test walk
    MachineConfig st_config() const
    {
        MachineConfig c = capture_config();
        c.track_rom = false;
        return c;
    }
    bool st_frames(Machine& m, int frames, uint32_t buttons);
    bool st_advance(const Bytes& cursor, uint32_t key, Bytes& next);
    bool st_step(const StScheme& scheme, Bytes& cursor, int index, StStep& out);
    int st_probe(const StScheme& scheme, const Bytes& root);
};

void Ripper::Impl::logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(buf);
}

void Ripper::Impl::emit_progress(bool finished)
{
    RipProgress p;
    {
        std::lock_guard<std::mutex> lk(mu);
        prog.elapsed_seconds = seconds_since(t_start);
        prog.budget_seconds = opt.search_seconds;
        prog.songs = (int)songs.size();
        prog.pending = (int)queue.size();
        prog.exploring = !finished && !budget_over() && !cancel.load();
        prog.finished = finished;
        p = prog;
    }
    if (listener)
        listener->on_progress(p);
}

// --------------------------------------------------------------------------
// One exploration run: start at a node, press buttons for a while, listen.
// --------------------------------------------------------------------------
void Ripper::Impl::explore(std::shared_ptr<Node> node, uint64_t seed, bool idle_boot, const ListenJob* listen)
{
    Machine m;
    Listener ls;
    ls.want_features = false;
    std::string err;

    const MachineConfig cfg = capture_config();
    bool ok;
    if (node->root) {
        ok = m.power_on(rom->words(), cfg, err);
    } else {
        const Bytes raw = node->state.unpack();
        ok = m.load_state(rom->words(), raw, cfg, err);
        if (ok && node->cart)
            m.import_cart_save(*node->cart);
    }
    if (!ok) {
        logf("explore: %s", err.c_str());
        std::lock_guard<std::mutex> lk(mu);
        node->visits++;
        node->crashes++;
        return;
    }
    m.set_audio_sink(&ls);

    Rng rng(seed);
    const int fps = (int)m.vi_rate();

    // Plan: which button style to use when.  An idle stretch after each
    // active one lets whatever music started settle without key clicks.
    struct Phase { double until; int style; };
    std::vector<Phase> plan;
    double limit;
    if (idle_boot) {
        limit = opt.idle_boot_seconds;
        plan.push_back({ limit, kBotIdle });
    } else if (listen) {
        limit = listen->seconds;
        plan.push_back({ limit, kBotIdle });
    } else {
        // a cold boot needs time to get to anything interesting
        limit = opt.episode_seconds + (node->root ? 20.0 : 0.0);
        double t = 0;
        if (node->root) {
            // let the very first logo appear before pressing anything
            t = 2.0 + rng.unit() * 6.0;
            plan.push_back({ t, kBotIdle });
        }
        while (t < limit) {
            static const int styles[] = { kBotStart, kBotStart, kBotConfirm, kBotConfirm, kBotConfirm, kBotMenu, kBotMenu, kBotMenu, kBotPlay, kBotPlay, kBotMash };
            const int style = styles[rng.range(0, (int)(sizeof(styles) / sizeof(styles[0])) - 1)];
            t += 2.0 + rng.unit() * 8.0;
            plan.push_back({ t, style });
            t += 3.0 + rng.unit() * 6.0;
            plan.push_back({ t, kBotIdle });
        }
    }
    size_t phase = 0;
    InputBot bot(rng.next(), plan[0].style, fps);
    std::string styles_used = bot_style_name(plan[0].style);

    Bytes snap_silent;         // latest state taken while the output was silent
    std::shared_ptr<const Bytes> cart_silent;   // ... and the save memory that goes with it
    double snap_silent_t = -1;
    auto grab_cart = [&m]() -> std::shared_ptr<const Bytes> {
        Bytes b;
        m.export_cart_save(b);
        if (b.empty())
            return nullptr;
        return std::make_shared<const Bytes>(std::move(b));
    };
    Bytes snap_tmp;
    double last_checkpoint = 0;
    double silence_len = 1e9;  // length of the silence we are in (or just left)
    bool in_silence = true;
    bool heard_anything = false;
    uint64_t last_audio_samples = 0;
    int found_here = 0;
    size_t new_bits_total = 0;
    size_t new_bits_since_checkpoint = 0;
    const double t_base = node->game_time;
    bool crashed = false;
    double t = 0;

    auto merge_coverage = [&]() {
        size_t bytes = 0;
        const uint8_t* cov = m.rom_coverage(&bytes);
        if (!cov)
            return;
        std::lock_guard<std::mutex> lk(mu);
        if (global_cover.size() < bytes)
            global_cover.resize(bytes, 0);
        const size_t fresh = popcount_new(cov, global_cover.data(), bytes);
        new_bits_total += fresh;
        new_bits_since_checkpoint += fresh;
    };

    while (!cancel.load(std::memory_order_relaxed)) {
        const int r = m.run(kStopVI | kStopAudio);
        if (r & kStopError) {
            logf("explore: the game stopped (%s) after %.0f s", m.error().c_str(), t_base + t);
            crashed = true;
            break;
        }

        if (ls.meter.total_samples() != last_audio_samples) {
            last_audio_samples = ls.meter.total_samples();
            const bool silent = ls.meter.silent_now();
            if (silent) {
                in_silence = true;
                silence_len = ls.meter.silence_seconds();
            } else if (in_silence) {
                // ---- sound just started ----
                in_silence = false;
                const bool gap_ok = !heard_anything || silence_len >= kOnsetGap;
                // A run that starts from a "just before the music" state
                // hears that same music start again right away; skip it.
                const bool own_start = !node->root && m.seconds() < 2.0;
                if (gap_ok && !own_start && !snap_silent.empty() && (m.seconds() - snap_silent_t) < 1.5) {
                    Candidate c;
                    c.state = PackedState::pack(snap_silent);
                    c.cart = cart_silent;
                    c.game_time = t_base + snap_silent_t;
                    c.origin = idle_boot ? SongOrigin::BootIdle : listen ? listen->origin : SongOrigin::Explored;
                    c.chain = listen ? listen->chain + 1 : 0;
                    merge_coverage();
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        prog.candidates++;
                        // Emulation is deterministic, so runs that have not
                        // diverged yet produce the very same state again.
                        const uint64_t h = c.state.hash();
                        if (std::find(seen_states.begin(), seen_states.end(), h) != seen_states.end()) {
                            prog.duplicates++;
                        } else {
                            seen_states.push_back(h);
                            queue.push_back(std::move(c));
                            // where a new piece of music starts is also a
                            // good place to branch the exploration from
                            add_node_locked(snap_silent, t_base + snap_silent_t, node->depth + 1, (double)new_bits_since_checkpoint * 4, cart_silent);
                            new_bits_since_checkpoint = 0;
                            ++found_here;
                        }
                    }
                    cv.notify_all();
                }
                heard_anything = true;
            }
        }

        if (r & kStopVI) {
            t = m.seconds();

            while (phase + 1 < plan.size() && t >= plan[phase].until) {
                ++phase;
                bot.set_style(plan[phase].style);
                if (plan[phase].style != kBotIdle) {
                    styles_used += ",";
                    styles_used += bot_style_name(plan[phase].style);
                }
            }
            m.set_buttons(opt.press_buttons ? bot.next() : 0);

            // While it is quiet keep a fresh save state at hand: the next
            // sound to start will want one from just before it.
            if (in_silence && (t - snap_silent_t) >= kSilentSnapEvery && m.busy() == 0) {
                if (m.snapshot(snap_silent)) {
                    snap_silent_t = t;
                    cart_silent = grab_cart();
                }
            }

            if ((t - last_checkpoint) >= kCheckpointEvery && m.busy() == 0 && m.snapshot(snap_tmp)) {
                last_checkpoint = t;
                merge_coverage();
                std::shared_ptr<const Bytes> cart_now = grab_cart();
                std::lock_guard<std::mutex> lk(mu);
                add_node_locked(snap_tmp, t_base + t, node->depth + 1, (double)new_bits_since_checkpoint * 4, std::move(cart_now));
                new_bits_since_checkpoint = 0;
                prog.explored_seconds += kCheckpointEvery;
            }

            if (t >= limit)
                break;
        }
    }

    merge_coverage();
    {
        std::lock_guard<std::mutex> lk(mu);
        node->visits++;
        node->yield += (double)new_bits_total * 4 + 256.0 * 1024 * found_here;
        if (crashed)
            node->crashes++;
    }
    logf("explore: node %d (%.0f s) [%s] ran %.0f s, %d song start(s), %u KB of new cartridge data",
         node->id, node->game_time, listen ? "listen" : styles_used.c_str(), t, found_here, (unsigned)(new_bits_total * 4 / 1024));
}

// --------------------------------------------------------------------------
// Load a candidate like a player would, and decide what it is.
// --------------------------------------------------------------------------
Ripper::Impl::Verdict Ripper::Impl::finalize(const Candidate& cand, int* new_id, std::string* why)
{
    auto fail = [&](Verdict v, const char* reason) {
        if (why)
            *why = reason;
        return v;
    };

    const Bytes state = cand.state.unpack();
    Machine m;
    Listener ls;
    std::string err;
    if (!m.load_state(rom->words(), state, player_config(), err)) {
        if (why)
            *why = err;
        return Verdict::Failed;
    }
    m.set_audio_sink(&ls);

    enum class Stop { Ok, Error, Stalled, Cancelled };
    // Renders until `seconds` of audio exist in total.
    auto render_until = [&](double seconds) -> Stop {
        double last_audio_at = m.seconds();
        uint64_t last_samples = ls.meter.total_samples();
        while (ls.meter.total_seconds() < seconds) {
            if (cancel.load(std::memory_order_relaxed))
                return Stop::Cancelled;
            const int r = m.run(kStopAudio | kStopVI);
            if (r & kStopError)
                return Stop::Error;
            if (ls.meter.total_samples() != last_samples) {
                last_samples = ls.meter.total_samples();
                last_audio_at = m.seconds();
            } else if (m.seconds() - last_audio_at > 4.0) {
                return Stop::Stalled; // the game no longer feeds the DAC
            }
        }
        return Stop::Ok;
    };

    // ---- 1. does it play at all? -------------------------------------
    Stop st = render_until(12.0);
    if (st == Stop::Cancelled)
        return Verdict::Cancelled;
    const FeatureTrack& tr = ls.fx.track();
    double first = first_sound(tr);
    if (first < 0 && st == Stop::Ok) {
        st = render_until(20.0);
        if (st == Stop::Cancelled)
            return Verdict::Cancelled;
        first = first_sound(tr);
    }
    if (first < 0)
        return fail(Verdict::Rejected, st == Stop::Ok ? "stays silent when played back" : "does not run when played back");
    if (first > 8.0)
        return fail(Verdict::Rejected, "music starts too late after the save state");

    // long enough to judge, and to compare with what we have
    if (st == Stop::Ok) {
        st = render_until(first + 12.0);
        if (st == Stop::Cancelled)
            return Verdict::Cancelled;
    }

    // how long does the first uninterrupted stretch of sound last?
    double probe_seconds = 10.0;
    {
        const size_t f0 = (size_t)(first * kFeatRate);
        size_t i = f0, quiet = 0, last_loud = f0;
        for (; i < tr.frames(); i++) {
            if (tr.silent(i)) {
                if (++quiet >= (size_t)(kEndGap * kFeatRate))
                    break;
            } else {
                quiet = 0;
                last_loud = i;
            }
        }
        const double stretch = (double)(last_loud + 1 - f0) / kFeatRate;
        if (stretch < probe_seconds)
            probe_seconds = stretch;
    }
    if (sound_total(tr) < opt.min_song_seconds || probe_seconds < std::min(1.0, opt.min_song_seconds))
        return fail(Verdict::Rejected, "too short to be music");

    // ---- 2. do we have it already? -----------------------------------
    std::shared_ptr<InProgress> mine;
    if (cand.origin != SongOrigin::Imported) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& s : songs) {
            const MatchResult mr = find_in(tr, first, probe_seconds, s->features, dup_limit(s->info));
            if (mr.matched)
                return fail(Verdict::Duplicate, "already have this one");
        }
        for (const auto& p : in_progress) {
            const MatchResult mr = find_in(tr, first, probe_seconds, p->features, 3.0);
            if (mr.matched)
                return fail(Verdict::Duplicate, "already being analysed");
        }
        mine = std::make_shared<InProgress>();
        mine->features = tr;
        mine->first_sound = first;
        mine->probe_seconds = probe_seconds;
        in_progress.push_back(mine);
    }
    struct Unregister {
        Impl* impl;
        std::shared_ptr<InProgress> p;
        ~Unregister()
        {
            if (!p)
                return;
            std::lock_guard<std::mutex> lk(impl->mu);
            auto& v = impl->in_progress;
            v.erase(std::remove(v.begin(), v.end(), p), v.end());
        }
    } unregister{ this, mine };

    // ---- 3. how long is it? ------------------------------------------
    SongInfo info;
    info.kind = SongKind::Unknown;
    info.origin = cand.origin;
    info.found_at_seconds = cand.game_time;
    info.sample_rate = ls.meter.rate();

    // Where does this piece of music end?  Three things can happen:
    //  - it loops: the audio repeats with a fixed period from some point on;
    //  - it stops: silence follows (or the game stops feeding the DAC);
    //  - something else starts after a short pause (a jingle followed by the
    //    actual tune, a medley).  That pause ends *this* song; what follows
    //    is picked up as a song of its own by the exploration.
    // A pause can also be part of a looping tune, so a gap only counts as
    // the end when no loop is found that contains it.
    // Rendering is the expensive part, so look at a short stretch first:
    // jingles and other short pieces are settled after half a minute.
    double target = std::min(std::max(30.0, first + 20.0), opt.max_render_seconds);
    bool stalled = (st != Stop::Ok);
    double stall_at = stalled ? ls.meter.total_seconds() : 0;
    for (;;) {
        if (!stalled) {
            st = render_until(target);
            if (st == Stop::Cancelled)
                return Verdict::Cancelled;
            if (st != Stop::Ok) {
                stalled = true;
                stall_at = ls.meter.total_seconds();
            }
        }
        const EndResult end = find_end(tr, kEndGap);
        const bool really_over = end.found && (end.resume_seconds < 0) && (end.silence_seconds >= 5.0 || stalled);
        if (really_over) {
            info.kind = SongKind::Ending;
            info.length_seconds = end.end_seconds + 0.5;
            info.fade_seconds = 0;
            break;
        }
        if (stalled) {
            if (end.found) {
                info.kind = SongKind::Ending;
                info.length_seconds = end.end_seconds + 0.5;
                info.fade_seconds = 0;
            }
            break;
        }
        const LoopResult loop = find_loop(tr);
        // A pause before the looping part normally separates two pieces (a
        // jingle, then the tune).  But when the same pause comes back one
        // loop later it is part of the tune itself, and what precedes it is
        // simply the tune's first pass.
        const bool pause_first = end.found && end.end_seconds < loop.intro_seconds + 0.5;
        if (loop.found && (!pause_first || pause_recurs(tr, end, loop.loop_seconds))) {
            info.kind = SongKind::Looping;
            info.intro_seconds = loop.intro_seconds;
            info.loop_seconds = loop.loop_seconds;
            int loops = std::max(1, opt.loop_count);
            // very long loops: once through is plenty
            while (loops > 1 && loop.intro_seconds + loop.loop_seconds * loops > 600.0)
                --loops;
            info.length_seconds = loop.intro_seconds + loop.loop_seconds * loops;
            info.fade_seconds = opt.fade_seconds;
            break;
        }
        if (loop.found && end.found) {
            // the loop belongs to whatever starts after the pause
            info.kind = SongKind::Ending;
            info.length_seconds = end.end_seconds + 0.5;
            info.fade_seconds = 0;
            break;
        }
        if (target >= opt.max_render_seconds) {
            if (end.found) {
                info.kind = SongKind::Ending;
                info.length_seconds = end.end_seconds + 0.5;
                info.fade_seconds = 0;
            }
            break;
        }
        if (loop.pending && loop.need_seconds > target)
            target = std::min(opt.max_render_seconds, std::max(loop.need_seconds, target + 30.0));
        else if (target < 150.0)
            target = std::min(opt.max_render_seconds, 150.0);
        else
            target = std::min(opt.max_render_seconds, target * 2.0);
    }

    if (info.kind == SongKind::Unknown) {
        if (stalled) {
            // The game stopped feeding the DAC: either the music simply ran
            // out or the game locked up.  Either way that is where it ends,
            // and a player must not be asked for more than there is.
            info.kind = SongKind::Ending;
            info.length_seconds = std::max(0.0, stall_at - 0.3);
            info.fade_seconds = 0;
            info.stalls = true;
            if (info.length_seconds - first < opt.min_song_seconds)
                return fail(Verdict::Rejected, "the game stops shortly after the save state is loaded");
        } else {
            info.length_seconds = opt.default_length_seconds;
            info.fade_seconds = opt.fade_seconds;
        }
    } else if (stalled && info.length_seconds + info.fade_seconds > stall_at - 0.3) {
        info.length_seconds = std::max(0.0, std::min(info.length_seconds, stall_at - 0.3));
        info.fade_seconds = 0;
        info.stalls = true;
    }
    if (info.kind == SongKind::Ending && info.length_seconds - first < std::min(1.0, opt.min_song_seconds))
        return fail(Verdict::Rejected, "too short to be music");

    // ---- 4. make sure the cartridge coverage spans the whole playing time
    if (!stalled) {
        st = render_until(info.length_seconds + info.fade_seconds + 3.0);
        if (st == Stop::Cancelled)
            return Verdict::Cancelled;
        if (st != Stop::Ok && ls.meter.total_seconds() < info.length_seconds + info.fade_seconds) {
            info.stalls = true;
        }
    }

    auto rec = std::make_shared<SongRec>();
    {
        size_t bytes = 0;
        const uint8_t* cov = m.rom_coverage(&bytes);
        rec->rom_blocks = blocks_from_coverage(cov, bytes, rom->words().size(), 256);
    }
    info.hle_ok = m.audio_hle_ok() != 0;
    rec->state = cand.state;
    info.state_bytes = rec->state.bytes();
    rec->features = tr;
    rec->first_sound = first;
    rec->probe_seconds = probe_seconds;

    // Something else starts after this song: have an exploration run wait
    // for it (pressing nothing), so that it gets its own save state.
    double follow_at = -1;
    if (cand.origin != SongOrigin::Imported && cand.origin != SongOrigin::SoundTest && cand.chain < 16) {
        const EndResult end = find_end(tr, kEndGap);
        if (end.found && end.resume_seconds >= 0 && info.kind == SongKind::Ending && end.resume_seconds < opt.max_render_seconds)
            follow_at = end.resume_seconds;
    }

    // ---- 5. commit ---------------------------------------------------
    std::vector<int> removed;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (cand.origin != SongOrigin::Imported) {
            // Someone else may have finished the same song in the meantime.
            for (const auto& s : songs) {
                const MatchResult mr = find_in(tr, first, probe_seconds, s->features, dup_limit(s->info));
                if (mr.matched)
                    return fail(Verdict::Duplicate, "already have this one");
            }
            // An earlier find that turns out to start in the middle of this
            // song is the lesser copy; replace it.
            for (size_t i = 0; i < songs.size();) {
                const auto& s = songs[i];
                const MatchResult mr = find_in(s->features, s->first_sound, s->probe_seconds, tr, dup_limit(info));
                if (mr.matched && mr.offset_seconds > first + 1.5 && s->info.origin != SongOrigin::Imported) {
                    removed.push_back(s->info.id);
                    songs.erase(songs.begin() + (std::ptrdiff_t)i);
                } else {
                    ++i;
                }
            }
        }
        info.id = next_song_id++;
        info.order = cand.order >= 0 ? cand.order : 1000000 + info.id;
        rec->auto_title = cand.title.empty();
        if (!cand.title.empty()) {
            info.title = cand.title;
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Track %02d", info.id);
            info.title = buf;
        }
        rec->info = info;
        songs.push_back(rec);
        if (follow_at >= 0) {
            ListenJob j;
            j.state = cand.state;
            j.cart = cand.cart;
            j.game_time = cand.game_time;
            j.seconds = follow_at + 20.0;
            j.origin = cand.origin;
            j.chain = cand.chain;
            listen_queue.push_back(std::move(j));
        }
    }
    if (new_id)
        *new_id = info.id;
    if (listener) {
        for (int id : removed)
            listener->on_song_removed(id);
        listener->on_song_added(info);
    }
    return Verdict::Added;
}

// --------------------------------------------------------------------------
void Ripper::Impl::worker(int index)
{
    Rng rng(opt.seed * 0x9E3779B97F4A7C15ull + (uint64_t)index * 0xD1B54A32D192ED03ull + 1);
    for (;;) {
        Candidate cand;
        ListenJob listen;
        std::shared_ptr<Node> node;
        bool do_finalize = false, idle_boot = false, do_listen = false;
        uint64_t seed = 0;
        {
            std::unique_lock<std::mutex> lk(mu);
            for (;;) {
                if (stop_workers)
                    return;
                if (cancel.load()) {
                    queue.clear();
                    listen_queue.clear();
                }
                if (!queue.empty()) {
                    cand = std::move(queue.front());
                    queue.pop_front();
                    do_finalize = true;
                    break;
                }
                const bool may_explore = !st_mode && !cancel.load() && !budget_over() && (int)songs.size() < opt.max_songs;
                if (!may_explore)
                    listen_queue.clear();
                if (!listen_queue.empty()) {
                    listen = std::move(listen_queue.front());
                    listen_queue.pop_front();
                    do_listen = true;
                    break;
                }
                if (may_explore && !idle_boot_taken) {
                    // exactly one run watches the untouched game
                    idle_boot_taken = true;
                    idle_boot = true;
                    node = nodes.front();
                    break;
                }
                if (may_explore && opt.press_buttons) {
                    node = pick_node_locked(rng);
                    if (node)
                        break;
                }
                if (active == 0 && !(st_mode && st_walking)) {
                    // nothing queued, nothing running, nothing left to start
                    stop_workers = true;
                    cv.notify_all();
                    return;
                }
                cv.wait_for(lk, std::chrono::milliseconds(100));
            }
            ++active;
            seed = opt.seed + (++job_counter) * 0x2545F4914F6CDD1Dull;
        }

        if (do_finalize) {
            std::string why;
            const Verdict v = finalize(cand, nullptr, &why);
            std::lock_guard<std::mutex> lk(mu);
            if (v == Verdict::Duplicate)
                prog.duplicates++;
            else if (v == Verdict::Rejected || v == Verdict::Failed)
                prog.rejected++;
        } else if (do_listen) {
            auto tmp = std::make_shared<Node>();
            tmp->id = -1;
            tmp->state = listen.state;
            tmp->cart = listen.cart;
            tmp->game_time = listen.game_time;
            explore(tmp, seed, false, &listen);
        } else {
            explore(node, seed, idle_boot);
            if (idle_boot) {
                std::lock_guard<std::mutex> lk(mu);
                idle_boot_done = true;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mu);
            --active;
        }
        cv.notify_all();
    }
}

// ==========================================================================

Ripper::Ripper() : impl_(new Impl) {}

Ripper::~Ripper()
{
    cancel();
}

void Ripper::set_rom(std::shared_ptr<const Rom> rom)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->rom = std::move(rom);
    impl_->songs.clear();
    impl_->nodes.clear();
    impl_->queue.clear();
    impl_->listen_queue.clear();
    impl_->seen_states.clear();
    impl_->global_cover.clear();
    impl_->next_song_id = 1;
    impl_->next_node_id = 1;
    impl_->idle_boot_done = false;
    impl_->idle_boot_taken = false;
    impl_->prog = RipProgress();
}

void Ripper::set_options(const RipOptions& opt)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->opt = opt;
}

const RipOptions& Ripper::options() const
{
    return impl_->opt;
}

void Ripper::set_listener(RipListener* listener)
{
    impl_->listener = listener;
}

void Ripper::cancel()
{
    impl_->cancel.store(true);
    impl_->cv.notify_all();
}

bool Ripper::running() const
{
    return impl_->is_running.load();
}

RipProgress Ripper::progress() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    RipProgress p = impl_->prog;
    p.songs = (int)impl_->songs.size();
    p.pending = (int)impl_->queue.size();
    return p;
}

// --------------------------------------------------------------------------
// Sound test walk.
//
// The save state sits on a screen where one key selects the next tune and
// another one plays it.  Which keys those are is found by trying.  Whether a
// key press did anything is decided by running the same state twice, once
// with and once without the press: emulation is deterministic, so the two
// recordings are identical sample for sample up to the moment the press
// becomes audible.  That works no matter what is playing at the time, where
// "wait for silence, then for sound" would not.
// --------------------------------------------------------------------------
// A save state can not hold an audio buffer that is queued behind the one
// being played.  The exploration avoids that by only taking states while it
// is quiet; here music may be playing all the time, and dropping a sixtieth
// of a second of the *previous* tune is harmless.
static bool st_can_snapshot(Machine& m)
{
    return (m.busy() & ~kBusyAIFull) == 0;
}

bool Ripper::Impl::st_frames(Machine& m, int frames, uint32_t buttons)
{
    for (int i = 0; i < frames; i++) {
        if (cancel.load(std::memory_order_relaxed))
            return false;
        m.set_buttons(buttons);
        if (m.run(kStopVI) & kStopError)
            return false;
    }
    return true;
}

// Presses `key` once and returns the machine after things have settled.
bool Ripper::Impl::st_advance(const Bytes& cursor, uint32_t key, Bytes& next)
{
    Machine m;
    std::string err;
    if (!m.load_state(rom->words(), cursor, st_config(), err))
        return false;
    if (!st_frames(m, 2, 0) || !st_frames(m, kStHold, key) || !st_frames(m, kStSettle, 0))
        return false;
    for (int i = 0; i < 120; i++) {
        if (st_can_snapshot(m) && m.snapshot(next))
            return true;
        if (!st_frames(m, 1, 0))
            return false;
    }
    return false;
}

// Tries entry number `index` (0 = the one selected in `cursor`).  `cursor`
// is moved on to that entry.  Returns false when the emulation failed or the
// run was cancelled.
bool Ripper::Impl::st_step(const StScheme& scheme, Bytes& cursor, int index, StStep& out)
{
    out = StStep();
    const bool select_plays = scheme.play.buttons == 0;
    const uint32_t key = select_plays ? scheme.change.buttons : scheme.play.buttons;
    const MachineConfig cfg = st_config();
    std::string err;

    if (!select_plays && index > 0) {
        Bytes next;
        if (!st_advance(cursor, scheme.change.buttons, next))
            return false;
        cursor.swap(next);
    }

    // What is heard if nothing is pressed.
    PcmSink quiet;
    {
        Machine m;
        if (!m.load_state(rom->words(), cursor, cfg, err))
            return false;
        m.set_audio_sink(&quiet);
        while (m.seconds() < kStWindow + 1.6)
            if (!st_frames(m, 1, 0))
                return false;
    }

    // The same again with the key pressed.
    PcmSink loud;
    Machine m;
    if (!m.load_state(rom->words(), cursor, cfg, err))
        return false;
    m.set_audio_sink(&loud);

    // candidates for the save state: the last few from before the change
    // (a ring, newest at pre_at) and the first two after it
    const int kPre = 4;
    Bytes pre[kPre], post[2], settled;
    size_t pre_pos[kPre] = { 0, 0, 0, 0 }, post_pos[2] = { 0, 0 };
    int n_pre = 0, pre_at = -1, n_post = 0;
    bool have_settled = false;
    bool diverged = false;
    size_t cmp = 0;        // samples compared so far
    size_t td = 0;         // frame at which the recordings part
    double t_div = 0;
    for (int frame = 0;; frame++) {
        if (!st_frames(m, 1, (frame >= 2 && frame < 2 + kStHold) ? key : 0))
            return false;
        if (!diverged) {
            const size_t n = std::min(loud.pcm.size(), quiet.pcm.size());
            for (; cmp < n; cmp++) {
                if (loud.pcm[cmp] != quiet.pcm[cmp]) {
                    diverged = true;
                    break;
                }
            }
            // a game that only feeds the DAC while something plays
            if (!diverged && cmp >= quiet.pcm.size()) {
                for (; cmp < loud.pcm.size(); cmp++) {
                    if (loud.pcm[cmp] > 64 || loud.pcm[cmp] < -64) {
                        diverged = true;
                        break;
                    }
                }
            }
            if (diverged) {
                td = cmp / 2;
                t_div = m.seconds();
            }
        }
        if (!diverged) {
            const int slot = (pre_at + 1) % kPre;
            if (st_can_snapshot(m) && m.snapshot(pre[slot])) {
                pre_at = slot;
                pre_pos[slot] = loud.frames();
                if (n_pre < kPre)
                    ++n_pre;
            }
            if (m.seconds() >= kStWindow)
                break;
        } else {
            if (n_post < 2 && st_can_snapshot(m) && m.snapshot(post[n_post])) {
                post_pos[n_post] = loud.frames();
                ++n_post;
            }
            const size_t have = loud.frames();
            if (select_plays && !have_settled && loud.rate && have >= td + loud.rate / 2 && st_can_snapshot(m) && m.snapshot(settled))
                have_settled = true;
            if ((loud.rate && have >= td + (size_t)(kStListen * loud.rate)) || m.seconds() > t_div + kStListen + 2.0)
                break;
        }
    }
    if (select_plays) {
        // the next entry is selected from wherever this press left the game
        for (int i = 0; !have_settled && i < 120; i++) {
            if (st_can_snapshot(m) && m.snapshot(settled))
                have_settled = true;
            else if (!st_frames(m, 1, 0))
                return false;
        }
        if (!have_settled)
            return false;
        cursor.swap(settled);
    }
    m.shutdown();
    if (!diverged || !loud.rate)
        return true;

    // A beep for moving the cursor also changes what is heard, but only for
    // a moment: afterwards the two recordings are the same again.  A tune
    // that was started keeps them apart.
    {
        const size_t a = td + loud.rate * 3 / 10;
        const size_t b = std::min(std::min(quiet.frames(), loud.frames()), td + loud.rate * 3 / 2);
        if (b > a + loud.rate / 2) {
            const size_t block = std::max<size_t>(1, loud.rate / 100);
            size_t total = 0, differing = 0;
            for (size_t i = a; i + block <= b; i += block) {
                ++total;
                if (std::memcmp(loud.pcm.data() + i * 2, quiet.pcm.data() + i * 2, block * 2 * sizeof(int16_t)) != 0)
                    ++differing;
            }
            FeatureExtractor fa, fb;
            fa.feed(loud.pcm.data() + a * 2, b - a, loud.rate);
            fb.feed(quiet.pcm.data() + a * 2, b - a, loud.rate);
            if (differing * 2 < total || similarity(fa.track(), fb.track(), 1.2) >= 0.90)
                return true;
        }
    }
    out.reacted = true;

    {
        FeatureExtractor fx;
        fx.feed(loud.pcm.data() + td * 2, loud.frames() - td, loud.rate);
        out.print = fx.track();
    }

    // A state is only of use if it goes on to play the new sound with nobody
    // touching the controller.  One from shortly before the change is best
    // (nothing of the tune is cut off), but it may predate the moment the
    // game noticed the key, in which case it just keeps playing the old
    // sound; the ones after the change are the fallback.  A restored game
    // does not reproduce the recording sample for sample (the audio hardware
    // starts out idle), so the comparison is by ear: what the state plays
    // from the point of the change on has to sound like the new sound, and
    // more so than like what was playing before.
    FeatureTrack old_print;
    if (quiet.frames() > td + loud.rate / 2) {
        FeatureExtractor fx;
        fx.feed(quiet.pcm.data() + td * 2, quiet.frames() - td, loud.rate);
        old_print = fx.track();
    }
    auto plays_alone = [&](const Bytes& snap, size_t pos) -> int {
        const size_t lead = td > pos ? td - pos : 0;
        const size_t need = lead + loud.rate + loud.rate / 4;
        Machine v;
        PcmSink vs;
        std::string e;
        if (!v.load_state(rom->words(), snap, cfg, e))
            return 0;
        v.set_audio_sink(&vs);
        while (vs.frames() < need && v.seconds() < 8.0)
            if (!st_frames(v, 1, 0))
                return -1;
        if (vs.frames() < lead + loud.rate / 2)
            return 0;
        FeatureExtractor fx;
        fx.feed(vs.pcm.data() + lead * 2, vs.frames() - lead, vs.rate);
        const double like_new = similarity(fx.track(), out.print, 1.0);
        const double like_old = old_print.frames() ? similarity(fx.track(), old_print, 1.0) : 0.0;
        return (like_new >= 0.70 && (like_new >= like_old + 0.10 || like_new >= 0.95)) ? 1 : 0;
    };
    // order of preference: before the change with a little room to spare
    // (so that a dropped audio buffer still belongs to the old sound), then
    // right before the change, then after it
    std::vector<std::pair<const Bytes*, size_t>> tries;
    const size_t room = loud.rate / 20;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n_pre; i++) {
            const int slot = ((pre_at - i) % kPre + kPre) % kPre;
            const bool roomy = pre_pos[slot] + room <= td;
            if (roomy == (pass == 0))
                tries.emplace_back(&pre[slot], pre_pos[slot]);
        }
    }
    for (int i = 0; i < n_post; i++)
        tries.emplace_back(&post[i], post_pos[i]);
    for (const auto& t : tries) {
        const int r = plays_alone(*t.first, t.second);
        if (r < 0)
            return false;
        if (r == 1) {
            out.state = *t.first;
            out.captured = true;
            break;
        }
    }
    return true;
}

// How many different sounds the first few entries give with these keys.
int Ripper::Impl::st_probe(const StScheme& scheme, const Bytes& root)
{
    Bytes cur = root;
    std::vector<FeatureTrack> prints;
    int distinct = 0, misses = 0;
    for (int k = 0; k < 4; k++) {
        StStep st;
        if (!st_step(scheme, cur, k, st))
            break;
        if (!st.captured) {
            if (++misses >= 2 && distinct == 0)
                break;
            continue;
        }
        bool seen = false;
        for (const FeatureTrack& p : prints)
            if (similarity(st.print, p, 2.5) >= kStSame)
                seen = true;
        if (!seen)
            ++distinct;
        prints.push_back(std::move(st.print));
    }
    return distinct;
}

bool Ripper::run_sound_test(const Bytes& pj64_state, SoundTestResult& result, std::string& err)
{
    Impl& d = *impl_;
    result = SoundTestResult();
    if (!d.rom || d.rom->empty()) {
        err = "no ROM loaded";
        return false;
    }
    if (pj64_state.size() < 8 || rd32le(pj64_state.data()) != 0x23D8A6C8u) {
        err = "not a Project64 save state";
        return false;
    }
    {
        Machine m;
        if (!m.load_state(d.rom->words(), pj64_state, d.st_config(), err))
            return false;
    }
    if (d.is_running.exchange(true)) {
        err = "busy";
        return false;
    }

    int threads = d.opt.threads;
    if (threads <= 0) {
        threads = (int)std::thread::hardware_concurrency();
        if (threads <= 0)
            threads = 2;
        if (threads > 8)
            threads = 8;
    }

    const StKey right = { kBtnDRight, "D-Pad Right" }, down = { kBtnDDown, "D-Pad Down" };
    const StKey left = { kBtnDLeft, "D-Pad Left" }, up = { kBtnDUp, "D-Pad Up" };
    const StKey s_right = { stick(80, 0), "Stick Right" }, s_down = { stick(0, -80), "Stick Down" };
    const StKey key_a = { kBtnA, "A" }, key_b = { kBtnB, "B" }, key_start = { kBtnStart, "START" }, key_z = { kBtnZ, "Z" };
    const StKey key_r = { kBtnR, "R" }, c_right = { kBtnCRight, "C Right" }, c_down = { kBtnCDown, "C Down" };
    const StKey none = { 0, "" };
    // most likely first: with equal results the earlier one is used
    const std::vector<StScheme> schemes = {
        { right, key_a },   { down, key_a },     { s_right, key_a },   { s_down, key_a },
        { right, none },    { down, none },      { s_right, none },    { s_down, none },
        { right, key_start }, { down, key_start }, { s_right, key_start }, { s_down, key_start },
        { key_r, key_a },   { c_right, key_a },  { c_down, key_a },    { key_r, none },
        { right, key_b },   { down, key_b },     { right, key_z },     { down, key_z },
        { key_a, none },    { left, key_a },     { up, key_a },
    };

    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.cancel.store(false);
        d.t_start = Clock::now();
        d.prog.st_phase = 1;
        d.prog.st_entry = 0;
        d.prog.st_keys.clear();
    }
    d.emit_progress(false);

    auto finish = [&d]() {
        {
            std::lock_guard<std::mutex> lk(d.mu);
            d.prog.st_phase = 0;
            d.st_mode = false;
            d.st_walking = false;
        }
        d.is_running.store(false);
        d.emit_progress(true);
    };

    // ---- 1. which keys work this screen? --------------------------------
    std::vector<int> score(schemes.size(), 0);
    {
        std::atomic<size_t> next{ 0 };
        std::vector<std::thread> pool;
        const int n = std::min<int>(threads, (int)schemes.size());
        for (int t = 0; t < n; t++) {
            pool.emplace_back([&] {
                for (;;) {
                    const size_t i = next.fetch_add(1);
                    if (i >= schemes.size() || d.cancel.load())
                        break;
                    score[i] = d.st_probe(schemes[i], pj64_state);
                    d.logf("sound test: %s%s%s -> %d different sound(s)", schemes[i].change.name,
                           schemes[i].play.buttons ? " + " : "", schemes[i].play.name, score[i]);
                }
            });
        }
        for (std::thread& t : pool)
            t.join();
    }
    if (d.cancel.load()) {
        finish();
        result.cancelled = true;
        err = "cancelled";
        return false;
    }
    size_t best = 0;
    for (size_t i = 1; i < schemes.size(); i++)
        if (score[i] > score[best])
            best = i;
    if (score[best] < 2) {
        finish();
        err = "the screen in this save state does not react like a sound test: no key was found that moves "
              "to another tune and plays it";
        return false;
    }
    const StScheme scheme = schemes[best];
    result.controls_found = true;
    result.keys = scheme.change.name;
    if (scheme.play.buttons) {
        result.keys += ", then ";
        result.keys += scheme.play.name;
    }
    d.logf("sound test: using %s", result.keys.c_str());

    // ---- 2. walk the list ------------------------------------------------
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.st_mode = true;
        d.st_walking = true;
        d.stop_workers = false;
        d.active = 0;
        d.prog.st_phase = 2;
        d.prog.st_keys = result.keys;
    }
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; i++)
        pool.emplace_back([&d, i] { d.worker(i); });

    Bytes cur = pj64_state;
    std::vector<FeatureTrack> prints;   // one per different sound, in order
    int misses = 0, repeats = 0;
    for (int k = 0; k < kStMaxEntries && !d.cancel.load(); k++) {
        {
            std::lock_guard<std::mutex> lk(d.mu);
            d.prog.st_entry = k + 1;
        }
        d.emit_progress(false);

        StStep st;
        if (!d.st_step(scheme, cur, k, st))
            break;
        result.entries = k + 1;
        if (!st.captured) {
            // an empty slot; a few of those in a row and the list is over
            if (++misses >= 8)
                break;
            continue;
        }
        misses = 0;
        if (!prints.empty() && similarity(st.print, prints.back(), 2.5) >= kStSame) {
            // the selection no longer moves: end of a list that does not wrap
            if (++repeats >= 3)
                break;
            continue;
        }
        repeats = 0;
        prints.push_back(st.print);
        const size_t n = prints.size();
        if (n >= 4 && similarity(prints[n - 1], prints[1], 2.5) >= kStSame && similarity(prints[n - 2], prints[0], 2.5) >= kStSame)
            break;   // been round once

        Candidate c;
        c.state = PackedState::pack(st.state);
        c.origin = SongOrigin::SoundTest;
        c.order = k;
        char title[32];
        std::snprintf(title, sizeof(title), "Track %02d", k + 1);
        c.title = title;
        ++result.sounds;
        {
            std::lock_guard<std::mutex> lk(d.mu);
            d.prog.candidates++;
            d.queue.push_back(std::move(c));
        }
        d.cv.notify_all();
    }

    // ---- 3. let the analysis of what was queued finish ---------------------
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.st_walking = false;
    }
    d.cv.notify_all();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(d.mu);
            if (d.cv.wait_for(lk, std::chrono::milliseconds(300), [&d] { return d.stop_workers; }))
                break;
        }
        d.emit_progress(false);
    }
    for (std::thread& t : pool)
        t.join();
    result.cancelled = d.cancel.load();
    finish();
    return true;
}

void Ripper::run()
{
    Impl& d = *impl_;
    if (!d.rom || d.rom->empty())
        return;
    if (d.is_running.exchange(true))
        return;

    int threads = d.opt.threads;
    if (threads <= 0) {
        threads = (int)std::thread::hardware_concurrency();
        if (threads <= 0)
            threads = 2;
        if (threads > 8)
            threads = 8;
    }

    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.cancel.store(false);
        d.stop_workers = false;
        d.active = 0;
        d.t_start = Clock::now();
        if (d.nodes.empty()) {
            auto root = std::make_shared<Node>();
            root->id = 0;
            root->root = true;
            d.nodes.push_back(root);
        }
        const size_t cover_bytes = d.rom->words().size() / 32 + 1;
        if (d.global_cover.size() < cover_bytes)
            d.global_cover.resize(cover_bytes, 0);
    }

    const RomInfo& ri = d.rom->info();
    d.logf("ROM: \"%s\" %s, %u KB, CIC %s, %s", ri.internal_name.c_str(), ri.game_code.c_str(), (unsigned)(ri.size / 1024),
           ri.cic.c_str(), ri.pal ? "PAL" : "NTSC");
    d.logf("exploring with %d thread(s)%s", threads, d.opt.press_buttons ? "" : ", without pressing buttons");

    std::vector<std::thread> pool;
    for (int i = 0; i < threads; i++)
        pool.emplace_back([&d, i] { d.worker(i); });

    // progress ticker
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(d.mu);
            if (d.cv.wait_for(lk, std::chrono::milliseconds(300), [&d] { return d.stop_workers; }))
                break;
        }
        d.emit_progress(false);
    }
    for (std::thread& t : pool)
        t.join();

    d.is_running.store(false);
    d.emit_progress(true);
}

std::vector<SongInfo> Ripper::songs() const
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<SongInfo> out;
    out.reserve(impl_->songs.size());
    for (const auto& s : impl_->songs)
        out.push_back(s->info);
    std::stable_sort(out.begin(), out.end(), [](const SongInfo& a, const SongInfo& b) { return a.order < b.order; });
    return out;
}

bool Ripper::rename_song(int id, const std::string& title)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto& s : impl_->songs) {
        if (s->info.id == id) {
            s->info.title = title;
            s->auto_title = false;
            return true;
        }
    }
    return false;
}

bool Ripper::remove_song(int id)
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& v = impl_->songs;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i]->info.id == id) {
            v.erase(v.begin() + (std::ptrdiff_t)i);
            return true;
        }
    }
    return false;
}

void Ripper::clear_songs()
{
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->songs.clear();
}

bool Ripper::add_from_state(const Bytes& pj64_state, const std::string& title, int* new_id, std::string& err)
{
    Impl& d = *impl_;
    if (!d.rom || d.rom->empty()) {
        err = "no ROM loaded";
        return false;
    }
    if (pj64_state.size() < 8 || rd32le(pj64_state.data()) != 0x23D8A6C8u) {
        err = "not a Project64 save state";
        return false;
    }
    Candidate c;
    c.state = PackedState::pack(pj64_state);
    c.origin = SongOrigin::Imported;
    c.title = title;
    d.cancel.store(false);
    std::string why;
    const Impl::Verdict v = d.finalize(c, new_id, &why);
    if (v == Impl::Verdict::Added)
        return true;
    err = why.empty() ? "could not use this save state" : why;
    return false;
}

bool Ripper::open_player(int id, Machine& m, std::string& err) const
{
    Impl& d = *impl_;
    Bytes state;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        for (const auto& s : d.songs) {
            if (s->info.id == id) {
                state = s->state.unpack();
                break;
            }
        }
    }
    if (state.empty()) {
        err = "no such song";
        return false;
    }
    MachineConfig cfg = d.player_config();
    cfg.track_rom = false;
    return m.load_state(d.rom->words(), state, cfg, err);
}

// --------------------------------------------------------------------------
bool Ripper::write_set(const Path& dir, const SetTags& tags, const std::vector<int>& ids, bool full_rom,
                       WriteReport& report, std::string& err) const
{
    Impl& d = *impl_;
    report = WriteReport();
    if (!d.rom || d.rom->empty()) {
        err = "no ROM loaded";
        return false;
    }

    std::vector<std::shared_ptr<SongRec>> list;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        for (int id : ids) {
            for (const auto& s : d.songs) {
                if (s->info.id == id) {
                    list.push_back(s);
                    break;
                }
            }
        }
    }
    if (list.empty()) {
        err = "no songs selected";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        err = "cannot create folder: " + path_to_utf8(dir);
        return false;
    }

    const Bytes& rom_words = d.rom->words();
    const std::string game = tags.game.empty() ? d.rom->info().internal_name : tags.game;
    const std::string base_name = sanitize_filename(game.empty() ? std::string("game") : game);
    const std::string lib_name = base_name + ".usflib";

    // ---- cartridge data for the library ---------------------------------
    std::vector<SparseBlock> rom_blocks;
    if (full_rom) {
        rom_blocks = blocks_nonzero(rom_words.data(), std::min(rom_words.size(), (d.rom->info().size + 3) & ~(size_t)3), 64);
    } else {
        Bytes cover(rom_words.size() / 32 + 1, 0);
        auto mark = [&cover](uint32_t offset, uint32_t length) {
            for (uint32_t w = offset / 4; w < (offset + length + 3) / 4; w++)
                if ((w >> 3) < cover.size())
                    cover[w >> 3] |= (uint8_t)(1u << (w & 7));
        };
        mark(0, 0x40); // the header identifies the game and tells players PAL from NTSC
        for (const auto& s : list)
            for (const SparseBlock& b : s->rom_blocks)
                mark(b.offset, b.length);
        rom_blocks = blocks_from_coverage(cover.data(), cover.size(), rom_words.size(), 256);
    }

    // ---- the common base -------------------------------------------------
    // The library carries one complete save state and every song only stores
    // what differs from it, so the best base is the state the others differ
    // least from.  Comparing all pairs gets slow for big sets; a spread-out
    // sample of candidates and of songs to compare them with is enough.
    size_t base_index = 0;
    if (list.size() > 2) {
        auto spread = [&list](size_t want) {
            std::vector<size_t> v;
            const size_t n = list.size();
            if (n <= want) {
                for (size_t i = 0; i < n; i++)
                    v.push_back(i);
            } else {
                for (size_t k = 0; k < want; k++)
                    v.push_back(k * (n - 1) / (want - 1));
            }
            return v;
        };
        const std::vector<size_t> cands = spread(6), probes = spread(8);
        std::vector<Bytes> probe_states;
        for (size_t i : probes)
            probe_states.push_back(list[i]->state.unpack());
        uint64_t best = ~(uint64_t)0;
        for (size_t c : cands) {
            const Bytes cs = list[c]->state.unpack();
            uint64_t total = 0;
            for (size_t k = 0; k < probes.size(); k++) {
                const Bytes& ps = probe_states[k];
                if (probes[k] == c || ps.size() != cs.size())
                    continue;
                const size_t words = cs.size() / 8;
                for (size_t w = 0; w < words; w++)
                    total += std::memcmp(cs.data() + w * 8, ps.data() + w * 8, 8) != 0 ? 8u : 0u;
            }
            if (total < best) {
                best = total;
                base_index = c;
            }
        }
    }
    const Bytes base_state = list[base_index]->state.unpack();
    const std::vector<SparseBlock> base_blocks = blocks_nonzero(base_state.data(), base_state.size(), 32);

    TagList lib_tags;
    if (d.opt.enable_compare)
        lib_tags.emplace_back("_enablecompare", "1");
    if (d.opt.enable_fifo_full)
        lib_tags.emplace_back("_enableFIFOfull", "1");

    {
        const Bytes reserved = build_usf_reserved(rom_words.data(), rom_blocks, base_state.data(), base_blocks);
        const Bytes file = build_usf_file(reserved, lib_tags);
        const Path p = dir / path_from_utf8(lib_name);
        if (!write_file(p, file.data(), file.size(), err))
            return false;
        report.files.push_back(p);
        report.lib_bytes = file.size();
        report.rom_bytes = blocks_total(rom_blocks);
        report.total_bytes += file.size();
    }

    // ---- one miniusf per song ------------------------------------------
    const int digits = list.size() >= 100 ? 3 : 2;
    for (size_t i = 0; i < list.size(); i++) {
        const SongRec& s = *list[i];
        const Bytes state = s.state.unpack();
        if (state.size() != base_state.size()) {
            err = "internal error: save states of different sizes in one set";
            return false;
        }
        const std::vector<SparseBlock> diff = blocks_diff(state.data(), base_state.data(), state.size(), 32);

        TagList t;
        t.emplace_back("_lib", lib_name);
        if (d.opt.enable_compare)
            t.emplace_back("_enablecompare", "1");
        if (d.opt.enable_fifo_full)
            t.emplace_back("_enableFIFOfull", "1");
        // Songs that still carry the name they were given when found
        // ("Track 07", by order of discovery) are numbered the way the files
        // are, so that the set does not have holes where duplicates were.
        std::string title = s.info.title;
        {
            char auto_name[32], out_name[32];
            std::snprintf(auto_name, sizeof(auto_name), "Track %02d", s.info.id);
            std::snprintf(out_name, sizeof(out_name), "Track %0*d", digits, (int)i + 1);
            if (s.auto_title && title == auto_name)
                title = out_name;
        }
        t.emplace_back("title", title);
        if (!game.empty())
            t.emplace_back("game", game);
        if (!tags.artist.empty())
            t.emplace_back("artist", tags.artist);
        if (!tags.year.empty())
            t.emplace_back("year", tags.year);
        if (!tags.copyright.empty())
            t.emplace_back("copyright", tags.copyright);
        {
            char num[16];
            std::snprintf(num, sizeof(num), "%d", (int)i + 1);
            t.emplace_back("track", num);
        }
        t.emplace_back("length", format_time_tag((uint64_t)std::llround(s.info.length_seconds * 1000.0)));
        t.emplace_back("fade", format_time_tag((uint64_t)std::llround(s.info.fade_seconds * 1000.0)));
        t.emplace_back("usfby", tags.usfby.empty() ? std::string("USF Maker") : tags.usfby);
        if (!tags.comment.empty())
            t.emplace_back("comment", tags.comment);
        t.emplace_back("utf8", "1");

        const Bytes reserved = build_usf_reserved(nullptr, {}, state.data(), diff);
        const Bytes file = build_usf_file(reserved, t);

        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%0*d ", digits, (int)i + 1);
        const Path p = dir / path_from_utf8(std::string(prefix) + sanitize_filename(title) + ".miniusf");
        if (!write_file(p, file.data(), file.size(), err))
            return false;
        report.files.push_back(p);
        report.total_bytes += file.size();

        // Read it back through the same path a player takes and make sure
        // the save state survived the base + difference encoding.
        UsfImage img;
        if (!load_usf(p, img)) {
            err = "verification failed for " + path_to_utf8(p) + ": " + img.error;
            return false;
        }
        if (img.state != state) {
            err = "verification failed for " + path_to_utf8(p) + ": save state does not round-trip";
            return false;
        }
    }
    return true;
}

} // namespace usfm
