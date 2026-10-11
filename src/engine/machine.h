// USF Maker - C++ wrapper around the emulator core.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#include "common.h"

#include <memory>

struct usfx_stats;

namespace usfm {

// Events that make Machine::run() return.
enum : int {
    kStopAudio = 0x01, // an audio buffer was handed to the DAC
    kStopVI = 0x02,    // a vertical interrupt (one video frame) passed
    kStopError = 0x04, // the core gave up; see Machine::error()
};

// Controller buttons (same values as the core's USFX_BTN_*).
enum : uint32_t {
    kBtnA = 0x80, kBtnB = 0x40, kBtnZ = 0x20, kBtnStart = 0x10,
    kBtnDUp = 0x08, kBtnDDown = 0x04, kBtnDLeft = 0x02, kBtnDRight = 0x01,
    kBtnL = 0x2000, kBtnR = 0x1000,
    kBtnCUp = 0x0800, kBtnCDown = 0x0400, kBtnCLeft = 0x0200, kBtnCRight = 0x0100,
};
inline uint32_t stick(int x, int y) { return (((uint32_t)(uint8_t)(int8_t)x) << 16) | (((uint32_t)(uint8_t)(int8_t)y) << 24); }

// Reasons a save state is not safe to take right now (core's USFX_BUSY_*).
enum : int {
    kBusySI = 0x001, kBusyPI = 0x002, kBusySP = 0x004, kBusyDP = 0x008,
    kBusyAI = 0x010, kBusyAIFull = 0x020, kBusyCheck = 0x040, kBusyCPU = 0x080,
    kBusyReset = 0x100, kBusyNotRunning = 0x200,
};

struct MachineConfig {
    // File tags.  These change how the machine is emulated, so whatever is
    // used while capturing has to be written into the USF as well.
    bool enable_compare = true;
    bool enable_fifo_full = true;

    bool hle_audio = true;            // player side option; does not go into the file
    uint32_t rdram_size = 0x800000;   // 4 MB or 8 MB (cold boot only)

    // --- things a USF player does not have ---
    bool controller = false;          // plug a controller into port 1
    bool fix_code_mirrors = false;    // see usfx_set_fix_code_mirrors()
    bool track_rom = false;           // record which cartridge bytes are read
    // Blank but working save memory (EEPROM, SRAM, FlashRAM), as on a brand
    // new cartridge.  Games ask for their save file while booting, and some
    // wait forever if nothing answers.
    bool cart_saves = false;
    bool eeprom_16k = false;          // the EEPROM is a 16 kbit part (default 4 kbit)
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Interleaved L,R 16 bit frames at `rate` Hz.
    virtual void on_audio(const int16_t* stereo, size_t frames, uint32_t rate) = 0;
};

class PiDmaSink {
public:
    virtual ~PiDmaSink() = default;
    virtual void on_pi_dma(uint32_t cart_offset, uint32_t ram_addr, uint32_t length) = 0;
};

// One emulated console.  Not thread safe; use one Machine per thread.
class Machine {
public:
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // `rom_words` must stay alive (and unchanged) for the life of the machine.
    // Cold boot of the cartridge:
    bool power_on(const Bytes& rom_words, const MachineConfig& cfg, std::string& err);
    // Resume from a Project64 save state, the way a USF player does:
    bool load_state(const Bytes& rom_words, const Bytes& state, const MachineConfig& cfg, std::string& err);

    void shutdown();
    bool running() const { return started_; }

    void set_audio_sink(AudioSink* sink) { audio_ = sink; }
    void set_pi_dma_sink(PiDmaSink* sink) { pi_ = sink; }
    void set_buttons(uint32_t buttons);

    // Runs until one of the kStop* events in `mask` happens; returns which.
    int run(int mask);
    const std::string& error() const { return error_; }

    int busy();                      // kBusy* bits, 0 = a save state is safe
    bool snapshot(Bytes& out);       // Project64 save state of the current machine

    const usfx_stats& stats() const;
    uint32_t sample_rate() const;
    uint32_t vi_rate() const;        // 50 or 60
    double seconds() const;          // emulated time since the machine was started
    uint32_t rdram_size() const;
    const uint8_t* rdram() const;
    uint32_t pc() const;
    int audio_hle_ok() const;        // 1 yes, 0 the HLE fell back to the interpreter, -1 unknown

    // The cartridge save memory is not part of a save state.  To continue a
    // game from a snapshot with what it has "saved" so far, take this along
    // and hand it to the machine the snapshot is loaded into.  An empty
    // buffer means "still blank".
    void export_cart_save(Bytes& out) const;
    void import_cart_save(const Bytes& data);

    // Bitmap, one bit per 32 bit cartridge word that has been read.
    const uint8_t* rom_coverage(size_t* bytes) const;
    void clear_rom_coverage();

private:
    bool prepare(const Bytes& rom_words, const MachineConfig& cfg, std::string& err);
    static void audio_thunk(void* user, const int16_t* stereo, size_t frames, uint32_t rate);
    static void pi_thunk(void* user, uint32_t cart, uint32_t dram, uint32_t length);

    void* state_ = nullptr;
    bool started_ = false;
    bool crashed_ = false;
    AudioSink* audio_ = nullptr;
    PiDmaSink* pi_ = nullptr;
    std::string error_;
};

} // namespace usfm
