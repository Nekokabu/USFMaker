// USF Maker - audio analysis: silence, fingerprints, loop and ending detection.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

#include "common.h"

namespace usfm {

// A recording is reduced to a sequence of short spectral snapshots
// ("frames").  Two pieces of audio that were rendered from the same music
// data produce almost identical frames, even from different save states,
// which is what both duplicate detection and loop detection rely on.
enum : int {
    kFeatBands = 24,      // log spaced frequency bands per frame
    kFeatRate = 100,      // frames per second
    kCoarseStep = 5,      // fine frames per coarse frame (20 per second)
};

struct FeatureTrack {
    uint32_t sample_rate = 0;
    std::vector<float> bands;   // kFeatBands values per frame: log10 band energy, level removed
    std::vector<float> level;   // per frame, log10 of total energy
    size_t frames() const { return level.size(); }
    const float* frame(size_t i) const { return &bands[i * kFeatBands]; }
    double seconds() const { return (double)frames() / kFeatRate; }
    bool silent(size_t i) const;
};

// Streaming extractor: feed audio as it is produced.
class FeatureExtractor {
public:
    FeatureExtractor();
    void reset();
    void feed(const int16_t* stereo, size_t frames, uint32_t rate);
    const FeatureTrack& track() const { return track_; }
    FeatureTrack& track() { return track_; }
    uint64_t samples_fed() const { return total_; }
    // Drops everything collected so far but keeps the configuration.
    void clear_track();

private:
    void configure(uint32_t rate);
    void emit(size_t end_sample_in_buf);

    FeatureTrack track_;
    uint32_t rate_ = 0;
    size_t fft_n_ = 0;
    std::vector<float> window_;
    std::vector<int> band_of_bin_;
    std::vector<float> ring_;        // mono samples, last fft_n_ kept
    size_t ring_pos_ = 0;
    uint64_t total_ = 0;             // mono samples fed
    double next_emit_ = 0;           // sample index at which the next frame is due
    std::vector<float> re_, im_;
};

// Tracks whether the output is silent, buffer by buffer.
class SilenceMeter {
public:
    void reset();
    void feed(const int16_t* stereo, size_t frames, uint32_t rate);
    bool silent_now() const { return silent_run_ > 0; }
    // seconds of uninterrupted silence up to now / of sound since it last started
    double silence_seconds() const { return rate_ ? (double)silent_run_ / rate_ : 0; }
    double sound_seconds() const { return rate_ ? (double)sound_run_ / rate_ : 0; }
    uint64_t total_samples() const { return total_; }
    uint64_t loud_samples() const { return loud_; }
    double total_seconds() const { return seconds_; }
    uint32_t rate() const { return rate_; }
    int peak() const { return peak_; }

private:
    uint32_t rate_ = 0;
    uint64_t silent_run_ = 0, sound_run_ = 0, total_ = 0, loud_ = 0;
    double seconds_ = 0;
    int peak_ = 0;
};

struct LoopResult {
    bool found = false;
    double intro_seconds = 0;     // where the looping part starts
    double loop_seconds = 0;      // length of one pass
    double score = 0;             // fraction of matching frames
    // A promising period was seen but the recording is too short to confirm
    // it; `need_seconds` of audio would be enough.
    bool pending = false;
    double need_seconds = 0;
};

// Looks for "the audio from intro onwards repeats every loop_seconds".
LoopResult find_loop(const FeatureTrack& t);

struct EndResult {
    bool found = false;
    double end_seconds = 0;       // where the gap starts
    double resume_seconds = -1;   // where sound comes back, or -1 if it stays silent to the end
    double silence_seconds = 0;   // length of the gap as far as it was recorded
};

// Looks for the music stopping: sound followed by at least gap_seconds of silence.
EndResult find_end(const FeatureTrack& t, double gap_seconds);

// Time of the first frame with sound, or -1 if the track is silent throughout.
double first_sound(const FeatureTrack& t);
// Total seconds of non-silent frames.
double sound_total(const FeatureTrack& t);

struct MatchResult {
    bool matched = false;
    double offset_seconds = 0;    // position in `ref` where `probe` starts
    double score = 0;
};

// Does `probe` (from probe_start, at most probe_seconds long) occur in `ref`,
// starting no later than ref_limit_seconds into it?
MatchResult find_in(const FeatureTrack& probe, double probe_start, double probe_seconds,
                    const FeatureTrack& ref, double ref_limit_seconds);

} // namespace usfm
