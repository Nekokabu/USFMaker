// USF Maker - preview playback through the Windows wave output.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "engine/ripper.h"

// Plays a song exactly as a USF player would: by running the emulator from
// the song's save state.  What you hear here is what the saved file gives.
class Player {
public:
    Player();
    ~Player();

    // Starts playing song `id`.  `notify` receives `message` (wParam = id,
    // lParam = 0 finished normally / 1 failed) when playback ends by itself.
    bool start(usfm::Ripper& ripper, const usfm::SongInfo& song, HWND notify, UINT message, std::wstring& err);
    void stop();
    bool playing() const { return running_.load(); }
    int current_id() const { return id_; }
    // seconds played so far
    double position() const { return position_.load(); }

private:
    void thread_main();

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<double> position_{ 0.0 };
    int id_ = 0;
    usfm::SongInfo song_;
    usfm::Machine machine_;
    HWND notify_ = nullptr;
    UINT message_ = 0;
};
