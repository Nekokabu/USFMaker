// USF Maker - preview playback through the Windows wave output.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "player.h"

#include <mmsystem.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace usfm;

namespace {

// Collects what the emulator produces until there is enough for one block.
class BlockSink : public AudioSink {
public:
    std::vector<int16_t> pcm;
    uint32_t rate = 0;
    void on_audio(const int16_t* stereo, size_t frames, uint32_t r) override
    {
        rate = r;
        pcm.insert(pcm.end(), stereo, stereo + frames * 2);
    }
};

class WaveOut {
public:
    ~WaveOut() { close(); }

    bool open(uint32_t rate)
    {
        close();
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = rate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = rate * 4;
        if (waveOutOpen(&dev_, WAVE_MAPPER, &fmt, (DWORD_PTR)event_, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            dev_ = nullptr;
            close();
            return false;
        }
        rate_ = rate;
        for (Block& b : blocks_) {
            b.data.assign((size_t)(rate / 10) * 4 + 4096, 0);
            std::memset(&b.hdr, 0, sizeof(b.hdr));
            b.busy = false;
        }
        return true;
    }

    void close()
    {
        if (dev_) {
            waveOutReset(dev_);
            for (Block& b : blocks_) {
                if (b.hdr.dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(dev_, &b.hdr, sizeof(WAVEHDR));
                b.busy = false;
            }
            waveOutClose(dev_);
            dev_ = nullptr;
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        rate_ = 0;
    }

    uint32_t rate() const { return rate_; }

    // Queues one block; waits while the device is still busy with earlier
    // ones.  Returns false if `stop` was raised while waiting.
    bool write(const int16_t* stereo, size_t frames, const std::atomic<bool>& stop)
    {
        while (frames) {
            Block* free_block = nullptr;
            for (;;) {
                for (Block& b : blocks_) {
                    if (b.busy && (b.hdr.dwFlags & WHDR_DONE)) {
                        waveOutUnprepareHeader(dev_, &b.hdr, sizeof(WAVEHDR));
                        b.busy = false;
                    }
                    if (!b.busy && !free_block)
                        free_block = &b;
                }
                if (free_block)
                    break;
                if (stop.load())
                    return false;
                WaitForSingleObject(event_, 50);
            }
            const size_t cap = free_block->data.size() / 4;
            const size_t n = frames < cap ? frames : cap;
            std::memcpy(free_block->data.data(), stereo, n * 4);
            std::memset(&free_block->hdr, 0, sizeof(WAVEHDR));
            free_block->hdr.lpData = (LPSTR)free_block->data.data();
            free_block->hdr.dwBufferLength = (DWORD)(n * 4);
            if (waveOutPrepareHeader(dev_, &free_block->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
                return false;
            if (waveOutWrite(dev_, &free_block->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                waveOutUnprepareHeader(dev_, &free_block->hdr, sizeof(WAVEHDR));
                return false;
            }
            free_block->busy = true;
            stereo += n * 2;
            frames -= n;
        }
        return true;
    }

    // Lets the queued audio play out (unless asked to stop).
    void drain(const std::atomic<bool>& stop)
    {
        for (int i = 0; i < 100 && !stop.load(); i++) {
            bool any = false;
            for (Block& b : blocks_)
                if (b.busy && !(b.hdr.dwFlags & WHDR_DONE))
                    any = true;
            if (!any)
                break;
            WaitForSingleObject(event_, 50);
        }
    }

private:
    struct Block {
        WAVEHDR hdr;
        std::vector<uint8_t> data;
        bool busy = false;
    };
    HWAVEOUT dev_ = nullptr;
    HANDLE event_ = nullptr;
    uint32_t rate_ = 0;
    Block blocks_[4];
};

} // namespace

Player::Player() {}

Player::~Player()
{
    stop();
}

bool Player::start(Ripper& ripper, const SongInfo& song, HWND notify, UINT message, std::wstring& err)
{
    stop();
    std::string e;
    if (!ripper.open_player(song.id, machine_, e)) {
        err.assign(e.begin(), e.end());
        return false;
    }
    song_ = song;
    id_ = song.id;
    notify_ = notify;
    message_ = message;
    stop_.store(false);
    position_.store(0.0);
    running_.store(true);
    thread_ = std::thread([this] { thread_main(); });
    return true;
}

void Player::stop()
{
    stop_.store(true);
    if (thread_.joinable())
        thread_.join();
    running_.store(false);
    machine_.shutdown();
}

void Player::thread_main()
{
    BlockSink sink;
    WaveOut out;
    machine_.set_audio_sink(&sink);

    bool failed = false;
    uint64_t played = 0;                 // frames handed to the device
    double last_audio_at = 0;
    size_t last_size = 0;
    // The preview honours the length and fade that will be written to the file.
    const double length = song_.length_seconds;
    const double fade = song_.fade_seconds;

    while (!stop_.load()) {
        const int r = machine_.run(kStopAudio | kStopVI);
        if (r & kStopError) {
            failed = sink.rate == 0;
            break;
        }
        if (sink.pcm.size() != last_size) {
            last_size = sink.pcm.size();
            last_audio_at = machine_.seconds();
        } else if (machine_.seconds() - last_audio_at > 5.0) {
            break; // the game stopped producing sound
        }
        if (!sink.rate)
            continue;
        const size_t block = (size_t)(sink.rate / 20);
        if (sink.pcm.size() / 2 < block)
            continue;

        if (out.rate() != sink.rate && !out.open(sink.rate)) {
            failed = true;
            break;
        }
        const size_t frames = sink.pcm.size() / 2;
        bool finished = false;
        if (length > 0) {
            for (size_t i = 0; i < frames; i++) {
                const double t = (double)(played + i) / sink.rate;
                if (t < length)
                    continue;
                double g = 0.0;
                if (fade > 0 && t < length + fade)
                    g = 1.0 - (t - length) / fade;
                else
                    finished = true;
                sink.pcm[i * 2] = (int16_t)std::lround(sink.pcm[i * 2] * g);
                sink.pcm[i * 2 + 1] = (int16_t)std::lround(sink.pcm[i * 2 + 1] * g);
            }
        }
        if (!out.write(sink.pcm.data(), frames, stop_))
            break;
        played += frames;
        position_.store((double)played / sink.rate);
        sink.pcm.clear();
        last_size = 0;
        if (finished)
            break;
    }

    if (!stop_.load() && !failed)
        out.drain(stop_);
    out.close();
    machine_.set_audio_sink(nullptr);
    running_.store(false);
    if (!stop_.load() && notify_)
        PostMessageW(notify_, message_, (WPARAM)id_, failed ? 1 : 0);
}
