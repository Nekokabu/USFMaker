// USF Maker - C++ wrapper around the emulator core.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "machine.h"

#include <cstdlib>
#include <cstring>

#include "../lazyusf2/usf/usf.h"
#include "../lazyusf2/usf/usfx.h"

#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace usfm {

static_assert(kStopAudio == USFX_STOP_AUDIO && kStopVI == USFX_STOP_VI && kStopError == USFX_STOP_ERROR, "stop bits out of sync");
static_assert(kBtnA == USFX_BTN_A && kBtnStart == USFX_BTN_START && kBtnL == USFX_BTN_L && kBtnCRight == USFX_BTN_CRIGHT, "button bits out of sync");
static_assert(kBusySI == USFX_BUSY_SI && kBusyAIFull == USFX_BUSY_AI_FULL && kBusyNotRunning == USFX_BUSY_NOTRUN, "busy bits out of sync");

// The core is old C code that was written to play known-good rips, not to
// survive whatever an arbitrary cartridge does after it has gone off the
// rails.  The obvious holes are plugged in the core itself; this is the last
// line of defence so that a wild pointer in a broken game ends the rip of
// that one game instead of taking the whole program down.
#if defined(_MSC_VER)
static int guarded_run(void* state, int mask, bool* crashed)
{
    __try {
        return usfx_run(state, mask);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION ||
                GetExceptionCode() == EXCEPTION_ARRAY_BOUNDS_EXCEEDED
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        *crashed = true;
        return USFX_STOP_ERROR;
    }
}
#else
static int guarded_run(void* state, int mask, bool* crashed)
{
    (void)crashed;
    return usfx_run(state, mask);
}
#endif

Machine::Machine() {}

Machine::~Machine()
{
    shutdown();
}

void Machine::shutdown()
{
    if (state_) {
        // After a crash the core's internal structures can not be trusted
        // enough to walk and free them; leaking them is the safe choice.
        if (!crashed_)
            usf_shutdown(state_);
        std::free(state_);
        state_ = nullptr;
    }
    started_ = false;
    crashed_ = false;
}

void Machine::audio_thunk(void* user, const int16_t* stereo, size_t frames, uint32_t rate)
{
    Machine* m = static_cast<Machine*>(user);
    if (m->audio_)
        m->audio_->on_audio(stereo, frames, rate);
}

void Machine::pi_thunk(void* user, uint32_t cart, uint32_t dram, uint32_t length)
{
    Machine* m = static_cast<Machine*>(user);
    if (m->pi_)
        m->pi_->on_pi_dma(cart, dram, length);
}

bool Machine::prepare(const Bytes& rom_words, const MachineConfig& cfg, std::string& err)
{
    shutdown();
    error_.clear();

    if (rom_words.empty() || (rom_words.size() & 0xFFFF) != 0) {
        err = "internal error: ROM buffer has an unexpected size";
        return false;
    }

    state_ = std::malloc(usf_get_state_size());
    if (!state_) {
        err = "out of memory";
        return false;
    }
    usf_clear(state_);

    usf_set_compare(state_, cfg.enable_compare ? 1 : 0);
    usf_set_fifo_full(state_, cfg.enable_fifo_full ? 1 : 0);
    usf_set_hle_audio(state_, cfg.hle_audio ? 1 : 0);

    usfx_set_rom_shared(state_, rom_words.data(), rom_words.size());
    usfx_set_controller(state_, 0, cfg.controller ? 1 : 0);
    usfx_set_fix_code_mirrors(state_, cfg.fix_code_mirrors ? 1 : 0);
    usfx_track_rom(state_, cfg.track_rom ? 1 : 0);
    usfx_set_cart_save(state_, cfg.cart_saves ? 1 : 0, cfg.eeprom_16k ? 1 : 0);
    aleck64_ = cfg.aleck64;
    dips_ = cfg.aleck64_dips;
    usfx_set_aleck64(state_, aleck64_ ? 1 : 0);
    usfx_set_audio_callback(state_, &Machine::audio_thunk, this);
    usfx_set_pi_dma_callback(state_, &Machine::pi_thunk, this);
    return true;
}

bool Machine::power_on(const Bytes& rom_words, const MachineConfig& cfg, std::string& err)
{
    if (!prepare(rom_words, cfg, err))
        return false;
    usfx_boot_from_rom(state_, cfg.rdram_size);
    started_ = true;
    return true;
}

bool Machine::load_state(const Bytes& rom_words, const Bytes& state, const MachineConfig& cfg, std::string& err)
{
    if (state.size() < 8 || rd32le(state.data()) != 0x23D8A6C8u) {
        err = "not a Project64 save state";
        return false;
    }
    const uint32_t ram = rd32le(state.data() + 4);
    if (ram != 0x400000 && ram != 0x800000) {
        err = "save state has an unsupported RDRAM size";
        return false;
    }
    if (state.size() < usfx_state_file_size(ram)) {
        err = "save state is truncated";
        return false;
    }
    if (!prepare(rom_words, cfg, err))
        return false;
    usf_upload_save_state(state_, state.data(), usfx_state_file_size(ram));
    started_ = true;
    return true;
}

void Machine::set_input(uint32_t buttons, uint32_t cabinet)
{
    if (!state_)
        return;
    usfx_set_buttons(state_, 0, buttons);
    if (!aleck64_)
        return;
    // Aleck64 cabinet (bit layouts as in MAME; everything active low).
    // IN0: DIP switches in the upper half; some games take their joystick
    //      there too (up, down, left, right, button 1, button 2, start).
    // IN1: coin 1 (two layouts), start, service.
    // Mahjong panel: start.
    uint32_t in0 = 0xffffffffu, in1 = 0xffffffffu, inmj = 0xffffffffu;
    for (int i = 0; i < 16; i++)
        if (dips_ & (1u << i))
            in0 &= ~(1u << (i < 8 ? 24 + i : 16 + (i - 8)));
    if (buttons & kBtnDUp) in0 &= ~0x01u;
    if (buttons & kBtnDDown) in0 &= ~0x02u;
    if (buttons & kBtnDLeft) in0 &= ~0x04u;
    if (buttons & kBtnDRight) in0 &= ~0x08u;
    if (buttons & kBtnA) in0 &= ~0x10u;
    if (buttons & kBtnB) in0 &= ~0x20u;
    if (buttons & kBtnStart) {
        in0 &= ~0x80u;
        in1 &= ~(0x00040000u | 0x00000001u | 0x00010000u);   // coin 1, coin 1 (other layout), start
        inmj &= ~0x100u;
    }
    if (cabinet & kCabCoin)
        in1 &= ~(0x00040000u | 0x00000001u);
    if (cabinet & kCabService)
        in1 &= ~(0x00100000u | 0x00000010u);                // service button (two layouts)
    if (cabinet & kCabTest)
        in1 &= ~0x00200000u;                                // test switch
    // mahjong panel keys A..N, kan, pon, chi, reach, ron (MAME's "hipai" layout)
    static const uint32_t mj_bits[19] = {
        0x00000200u, 0x00020000u, 0x00000002u, 0x02000000u, 0x00000400u, 0x00040000u, 0x00000004u,
        0x04000000u, 0x00000800u, 0x00080000u, 0x00000008u, 0x08000000u, 0x00001000u, 0x00100000u,
        0x00002000u, 0x10000000u, 0x00000010u, 0x00200000u, 0x00000020u };
    for (int i = 0; i < 19; i++)
        if (cabinet & (kCabMjA << i))
            inmj &= ~mj_bits[i];
    usfx_set_aleck64_inputs(state_, in0, in1, inmj);
}

int Machine::run(int mask)
{
    if (!state_ || !started_ || crashed_) {
        if (error_.empty())
            error_ = "machine is not running";
        return kStopError;
    }
    bool crashed = false;
    const int r = guarded_run(state_, mask, &crashed);
    if (crashed) {
        crashed_ = true;
        error_ = "the emulator core crashed while running this game";
        return kStopError;
    }
    if (r & USFX_STOP_ERROR) {
        error_ = usfx_last_error(state_);
        if (error_.empty())
            error_ = "emulation stopped";
    }
    return r;
}

int Machine::busy()
{
    if (!state_ || !started_ || crashed_)
        return kBusyNotRunning;
    return usfx_snapshot_busy(state_);
}

bool Machine::snapshot(Bytes& out)
{
    if (!state_ || !started_ || crashed_)
        return false;
    const uint32_t ram = usfx_rdram_size(state_);
    if (!ram)
        return false;
    out.resize(usfx_state_file_size(ram));
    const size_t n = usfx_snapshot(state_, out.data(), out.size());
    if (n != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

const usfx_stats& Machine::stats() const
{
    static const usfx_stats empty = {};
    return state_ ? *usfx_get_stats(state_) : empty;
}

uint32_t Machine::sample_rate() const { return state_ ? usfx_sample_rate(state_) : 0; }
double Machine::seconds() const { return state_ ? usfx_seconds(state_) : 0.0; }
uint32_t Machine::vi_rate() const { return state_ ? usfx_vi_rate(state_) : 60; }
uint32_t Machine::rdram_size() const { return state_ ? usfx_rdram_size(state_) : 0; }
const uint8_t* Machine::rdram() const { return state_ ? usfx_rdram(state_) : nullptr; }
uint32_t Machine::pc() const { return state_ ? usfx_pc(state_) : 0; }
int Machine::audio_hle_ok() const { return state_ ? usfx_audio_hle_ok(state_) : -1; }

// Side state container: "USXS", then for the cartridge save memory and the
// Aleck64 board each a 32 bit length and that many bytes.
static const uint32_t kSideMagic = 0x53585355u;

void Machine::export_side_state(Bytes& out) const
{
    out.clear();
    if (!state_ || crashed_)
        return;
    Bytes cart(usfx_cart_save_max()), a64(usfx_aleck64_max());
    cart.resize(usfx_cart_save_export(state_, cart.data(), cart.size()));
    a64.resize(aleck64_ ? usfx_aleck64_export(state_, a64.data(), a64.size()) : 0);
    if (cart.empty() && a64.empty())
        return;
    out.resize(12 + cart.size() + a64.size());
    wr32le(out.data(), kSideMagic);
    wr32le(out.data() + 4, (uint32_t)cart.size());
    if (!cart.empty())
        std::memcpy(out.data() + 8, cart.data(), cart.size());
    wr32le(out.data() + 8 + cart.size(), (uint32_t)a64.size());
    if (!a64.empty())
        std::memcpy(out.data() + 12 + cart.size(), a64.data(), a64.size());
}

void Machine::import_side_state(const Bytes& data)
{
    if (!state_ || crashed_)
        return;
    const uint8_t* cart = nullptr;
    const uint8_t* a64 = nullptr;
    size_t cart_n = 0, a64_n = 0;
    if (data.size() >= 12 && rd32le(data.data()) == kSideMagic) {
        cart_n = rd32le(data.data() + 4);
        if (cart_n <= data.size() - 12) {
            cart = data.data() + 8;
            a64_n = rd32le(data.data() + 8 + cart_n);
            if (a64_n <= data.size() - 12 - cart_n)
                a64 = data.data() + 12 + cart_n;
            else
                a64_n = 0;
        } else {
            cart_n = 0;
        }
    }
    usfx_cart_save_import(state_, cart_n ? cart : nullptr, cart_n);
    if (aleck64_)
        usfx_aleck64_import(state_, a64_n ? a64 : nullptr, a64_n);
}

const uint8_t* Machine::rom_coverage(size_t* bytes) const
{
    if (!state_) {
        if (bytes)
            *bytes = 0;
        return nullptr;
    }
    return usfx_rom_coverage(state_, bytes);
}

void Machine::clear_rom_coverage()
{
    if (state_)
        usfx_clear_rom_coverage(state_);
}

} // namespace usfm
