// USF Maker - audio analysis: silence, fingerprints, loop and ending detection.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace usfm {

namespace {

const float kEnergyFloor = 1e-9f;     // about -90 dB relative to full scale
const float kSilentLevel = -6.5f;     // log10 of total energy below which a frame counts as silent

// Mean absolute difference (in log10 units) below which two frames are
// considered to show the same sound.
// Measured on real renders (level-normalised features): the same music
// rendered twice, even at different volumes, differs by about 0.05-0.15 with
// the odd frame up to 0.25; a different passage of the same song starts at
// about 0.38 and unrelated music is around 0.7-1.2.
const float kCoarseMatch = 0.25f;
const float kFineMatch = 0.28f;

void fft(std::vector<float>& re, std::vector<float>& im)
{
    const size_t n = re.size();
    // bit reversal
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / (double)len;
        const float wr = (float)std::cos(ang), wi = (float)std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (size_t k = 0; k < len / 2; k++) {
                const size_t a = i + k, b = i + k + len / 2;
                const float xr = re[b] * cr - im[b] * ci;
                const float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

inline float frame_distance(const float* a, const float* b)
{
    float d = 0;
    for (int i = 0; i < kFeatBands; i++)
        d += std::fabs(a[i] - b[i]);
    return d * (1.0f / kFeatBands);
}

// Coarse (20 per second) view of a track.
struct Coarse {
    std::vector<float> bands;
    std::vector<uint8_t> silent;
    size_t n = 0;
    const float* frame(size_t i) const { return &bands[i * kFeatBands]; }
};

Coarse make_coarse(const FeatureTrack& t)
{
    Coarse c;
    c.n = t.frames() / kCoarseStep;
    c.bands.assign(c.n * kFeatBands, 0.0f);
    c.silent.assign(c.n, 0);
    for (size_t i = 0; i < c.n; i++) {
        float* dst = &c.bands[i * kFeatBands];
        int quiet = 0;
        for (int k = 0; k < kCoarseStep; k++) {
            const float* src = t.frame(i * kCoarseStep + k);
            for (int b = 0; b < kFeatBands; b++)
                dst[b] += src[b];
            if (t.silent(i * kCoarseStep + k))
                ++quiet;
        }
        for (int b = 0; b < kFeatBands; b++)
            dst[b] *= 1.0f / kCoarseStep;
        c.silent[i] = quiet == kCoarseStep;
    }
    return c;
}

inline bool coarse_match(const Coarse& a, size_t i, const Coarse& b, size_t j)
{
    if (a.silent[i] || b.silent[j])
        return a.silent[i] && b.silent[j];
    return frame_distance(a.frame(i), b.frame(j)) < kCoarseMatch;
}

inline bool fine_match(const FeatureTrack& a, size_t i, const FeatureTrack& b, size_t j)
{
    const bool sa = a.silent(i), sb = b.silent(j);
    if (sa || sb)
        return sa && sb;
    return frame_distance(a.frame(i), b.frame(j)) < kFineMatch;
}

} // namespace

bool FeatureTrack::silent(size_t i) const
{
    return level[i] < kSilentLevel;
}

FeatureExtractor::FeatureExtractor() {}

void FeatureExtractor::reset()
{
    track_ = FeatureTrack();
    rate_ = 0;
    fft_n_ = 0;
    total_ = 0;
    next_emit_ = 0;
    ring_pos_ = 0;
    ring_.clear();
}

void FeatureExtractor::clear_track()
{
    track_.bands.clear();
    track_.level.clear();
}

void FeatureExtractor::configure(uint32_t rate)
{
    rate_ = rate;
    track_.sample_rate = rate;
    size_t n = 256;
    while ((double)n < 0.04 * rate)
        n <<= 1;
    fft_n_ = n;
    window_.resize(n);
    for (size_t i = 0; i < n; i++)
        window_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double)i / (double)n));
    const double f_lo = 80.0;
    const double f_hi = std::min(8000.0, 0.45 * rate);
    band_of_bin_.assign(n / 2 + 1, -1);
    for (size_t k = 1; k <= n / 2; k++) {
        const double f = (double)k * rate / (double)n;
        if (f < f_lo || f >= f_hi)
            continue;
        int b = (int)(std::log(f / f_lo) / std::log(f_hi / f_lo) * kFeatBands);
        if (b < 0) b = 0;
        if (b >= kFeatBands) b = kFeatBands - 1;
        band_of_bin_[k] = b;
    }
    ring_.assign(n, 0.0f);
    ring_pos_ = 0;
    re_.resize(n);
    im_.resize(n);
    next_emit_ = (double)total_ + (double)rate / kFeatRate;
}

void FeatureExtractor::emit(size_t)
{
    const size_t n = fft_n_;
    // oldest sample is at ring_pos_
    for (size_t i = 0; i < n; i++) {
        re_[i] = ring_[(ring_pos_ + i) % n] * window_[i];
        im_[i] = 0.0f;
    }
    fft(re_, im_);
    float e[kFeatBands];
    for (int b = 0; b < kFeatBands; b++)
        e[b] = 0.0f;
    // scale so that a full scale sine comes out near 1.0
    const float scale = 16.0f / ((float)n * (float)n);
    float total = 0.0f;
    for (size_t k = 1; k <= n / 2; k++) {
        const float p = (re_[k] * re_[k] + im_[k] * im_[k]) * scale;
        total += p;
        const int b = band_of_bin_[k];
        if (b >= 0)
            e[b] += p;
    }
    // What is stored is the *shape* of the spectrum, not its level: the
    // same tune is often heard at several volumes (title screen versus
    // in-game, fade-ins) and must still be recognised as the same tune.
    // Bands more than 40 dB below the strongest one are clamped so that
    // the noise floor does not turn into part of the shape.
    float lg[kFeatBands];
    float top = -100.0f;
    for (int b = 0; b < kFeatBands; b++) {
        lg[b] = std::log10(e[b] + kEnergyFloor);
        if (lg[b] > top)
            top = lg[b];
    }
    float mean = 0.0f;
    for (int b = 0; b < kFeatBands; b++) {
        if (lg[b] < top - 4.0f)
            lg[b] = top - 4.0f;
        mean += lg[b];
    }
    mean *= 1.0f / kFeatBands;
    for (int b = 0; b < kFeatBands; b++)
        track_.bands.push_back(lg[b] - mean);
    track_.level.push_back(std::log10(total + kEnergyFloor));
}

void FeatureExtractor::feed(const int16_t* stereo, size_t frames, uint32_t rate)
{
    if (!rate || !frames)
        return;
    if (rate != rate_)
        configure(rate);
    const size_t n = fft_n_;
    for (size_t i = 0; i < frames; i++) {
        const float m = ((float)stereo[i * 2] + (float)stereo[i * 2 + 1]) * (0.5f / 32768.0f);
        ring_[ring_pos_] = m;
        ring_pos_ = (ring_pos_ + 1) % n;
        ++total_;
        if ((double)total_ >= next_emit_) {
            emit(0);
            next_emit_ += (double)rate_ / kFeatRate;
        }
    }
}

// ---------------------------------------------------------------------------

void SilenceMeter::reset()
{
    *this = SilenceMeter();
}

void SilenceMeter::feed(const int16_t* stereo, size_t frames, uint32_t rate)
{
    if (!frames || !rate)
        return;
    rate_ = rate;
    int lo_l = 32767, hi_l = -32768, lo_r = 32767, hi_r = -32768;
    for (size_t i = 0; i < frames; i++) {
        const int l = stereo[i * 2], r = stereo[i * 2 + 1];
        if (l < lo_l) lo_l = l;
        if (l > hi_l) hi_l = l;
        if (r < lo_r) lo_r = r;
        if (r > hi_r) hi_r = r;
    }
    const int p2p = std::max(hi_l - lo_l, hi_r - lo_r);
    const int pk = std::max(std::max(std::abs(lo_l), std::abs(hi_l)), std::max(std::abs(lo_r), std::abs(hi_r)));
    if (pk > peak_)
        peak_ = pk;
    total_ += frames;
    seconds_ += (double)frames / rate;
    // peak-to-peak rather than absolute level so that a DC offset is still silence
    if (p2p <= 16) {
        silent_run_ += frames;
        sound_run_ = 0;
    } else {
        sound_run_ += frames;
        silent_run_ = 0;
        loud_ += frames;
    }
}

// ---------------------------------------------------------------------------

double first_sound(const FeatureTrack& t)
{
    for (size_t i = 0; i < t.frames(); i++)
        if (!t.silent(i))
            return (double)i / kFeatRate;
    return -1.0;
}

double sound_total(const FeatureTrack& t)
{
    size_t n = 0;
    for (size_t i = 0; i < t.frames(); i++)
        if (!t.silent(i))
            ++n;
    return (double)n / kFeatRate;
}

EndResult find_end(const FeatureTrack& t, double gap_seconds)
{
    EndResult r;
    const size_t n = t.frames();
    const size_t gap = (size_t)(gap_seconds * kFeatRate);
    size_t i = 0;
    while (i < n && t.silent(i))
        ++i;
    if (i >= n)
        return r;
    size_t run_start = 0, run = 0;
    for (; i < n; i++) {
        if (t.silent(i)) {
            if (run == 0)
                run_start = i;
            if (++run >= gap) {
                r.found = true;
                r.end_seconds = (double)run_start / kFeatRate;
                size_t k = i + 1;
                while (k < n && t.silent(k))
                    ++k;
                r.resume_seconds = k < n ? (double)k / kFeatRate : -1.0;
                r.silence_seconds = (double)(k - run_start) / kFeatRate;
                return r;
            }
        } else {
            run = 0;
        }
    }
    return r;
}

LoopResult find_loop(const FeatureTrack& t)
{
    LoopResult best;
    const Coarse c = make_coarse(t);
    const size_t cps = kFeatRate / kCoarseStep; // coarse frames per second
    if (c.n < 20 * cps)
        return best;

    size_t a0 = 0;
    while (a0 < c.n && c.silent[a0])
        ++a0;
    if (a0 >= c.n)
        return best;

    const size_t min_lag = 4 * cps;
    const size_t window = 2 * cps;          // local window for the "no bad patch" rule
    const double need_frac = 0.92;

    struct Cand { size_t t0, lag, run; double frac; };
    std::vector<Cand> confirmed;
    Cand pending = { 0, 0, 0, 0 };
    bool have_pending = false;

    std::vector<uint8_t> m;
    for (size_t lag = min_lag; lag + a0 + 4 * cps < c.n; lag++) {
        const size_t end = c.n - lag;       // t runs over [a0, end)
        const size_t span = end - a0;

        // cheap rejection: sample the last part of the range
        {
            const size_t probe_span = std::min(span, lag);
            const size_t step = std::max<size_t>(1, probe_span / 48);
            size_t hit = 0, tot = 0, loud = 0;
            for (size_t k = end - probe_span; k < end; k += step) {
                ++tot;
                if (coarse_match(c, k, c, k + lag))
                    ++hit;
                if (!c.silent[k])
                    ++loud;
            }
            if (hit * 100 < tot * 80 || loud * 4 < tot)
                continue;
        }

        m.assign(span, 0);
        for (size_t k = 0; k < span; k++)
            m[k] = coarse_match(c, a0 + k, c, a0 + k + lag) ? 1 : 0;

        // Walk backwards from the end and stop at the first patch where the
        // match clearly breaks down; everything after it is the matching run.
        size_t t0 = 0;
        {
            size_t win_hits = 0;
            // sliding window over [k, k+window)
            for (size_t k = span; k-- > 0;) {
                win_hits += m[k];
                if (k + window < span)
                    win_hits -= m[k + window];
                const size_t wlen = std::min(window, span - k);
                if (wlen == window && win_hits * 2 < window) {
                    // the bad patch is [k, k+window): the run starts where
                    // matching resumes inside it
                    size_t s = k + window;
                    while (s > k && m[s - 1])
                        --s;
                    t0 = s;
                    break;
                }
            }
        }
        const size_t run = span - t0;
        if (run < 8 * cps)
            continue;
        size_t hits = 0, loud = 0;
        for (size_t k = t0; k < span; k++) {
            hits += m[k];
            if (!c.silent[a0 + k])
                ++loud;
        }
        const double frac = (double)hits / (double)run;
        if (frac < need_frac || loud * 2 < run)
            continue;

        Cand cand = { a0 + t0, lag, run, frac };
        if (run >= lag + 2 * cps) {
            confirmed.push_back(cand);
        } else if (run >= std::max<size_t>(15 * cps, lag / 2)) {
            // Looks periodic but less than one full extra pass was seen.
            const size_t need = cand.t0 + 2 * lag + 4 * cps;
            if (!have_pending || need < pending.t0 + 2 * pending.lag + 4 * cps) {
                pending = cand;
                have_pending = true;
            }
        }
    }

    if (confirmed.empty()) {
        if (have_pending) {
            best.pending = true;
            best.need_seconds = (double)(pending.t0 + 2 * pending.lag + 4 * cps) / cps;
        }
        return best;
    }

    // Earliest start wins (that is the real intro); among those the shortest period.
    size_t min_t0 = confirmed[0].t0;
    for (const Cand& k : confirmed)
        min_t0 = std::min(min_t0, k.t0);
    const Cand* pick = nullptr;
    for (const Cand& k : confirmed) {
        if (k.t0 > min_t0 + 1 * cps)
            continue;
        if (!pick || k.lag < pick->lag)
            pick = &k;
    }

    // Refine the period on the fine grid.
    const size_t fine_t0 = pick->t0 * kCoarseStep;
    size_t best_lag = pick->lag * kCoarseStep;
    double best_score = -1;
    const size_t n = t.frames();
    for (long d = -(long)kCoarseStep - 1; d <= (long)kCoarseStep + 1; d++) {
        const long lag_l = (long)(pick->lag * kCoarseStep) + d;
        if (lag_l <= 0 || (size_t)lag_l + fine_t0 + kFeatRate >= n)
            continue;
        const size_t lag = (size_t)lag_l;
        size_t hit = 0, tot = 0;
        for (size_t k = fine_t0; k + lag < n; k += 3) {
            ++tot;
            if (fine_match(t, k, t, k + lag))
                ++hit;
        }
        const double s = tot ? (double)hit / (double)tot : 0;
        if (s > best_score) {
            best_score = s;
            best_lag = lag;
        }
    }

    best.found = true;
    best.intro_seconds = (double)pick->t0 / cps;
    best.loop_seconds = (double)best_lag / kFeatRate;
    best.score = pick->frac;
    return best;
}

MatchResult find_in(const FeatureTrack& probe, double probe_start, double probe_seconds,
                    const FeatureTrack& ref, double ref_limit_seconds)
{
    MatchResult r;
    if (!probe.frames() || !ref.frames())
        return r;
    const Coarse p = make_coarse(probe);
    const Coarse q = make_coarse(ref);
    const size_t cps = kFeatRate / kCoarseStep;

    size_t p0 = (size_t)(std::max(0.0, probe_start) * cps);
    if (p0 >= p.n)
        return r;
    size_t pn = std::min((size_t)(probe_seconds * cps), p.n - p0);
    if (pn < cps)
        return r; // under a second there is not enough to go on
    size_t loud = 0;
    for (size_t i = 0; i < pn; i++)
        if (!p.silent[p0 + i])
            ++loud;
    if (loud * 2 < pn)
        return r; // mostly silence, nothing to compare

    size_t qn = std::min(q.n, (size_t)(ref_limit_seconds * cps) + pn);
    if (qn < pn)
        return r;

    double best = 0;
    size_t best_o = 0;
    const size_t step = std::max<size_t>(1, pn / 24);
    for (size_t o = 0; o + pn <= qn; o++) {
        size_t hit = 0, tot = 0;
        for (size_t i = 0; i < pn; i += step) {
            ++tot;
            if (coarse_match(p, p0 + i, q, o + i))
                ++hit;
        }
        if (hit * 100 < tot * 70)
            continue;
        hit = 0;
        for (size_t i = 0; i < pn; i++)
            if (coarse_match(p, p0 + i, q, o + i))
                ++hit;
        const double s = (double)hit / (double)pn;
        if (s > best) {
            best = s;
            best_o = o;
        }
    }
    if (best < 0.80)
        return r;

    // confirm on the fine grid, allowing the alignment to shift a little
    const size_t fp0 = p0 * kCoarseStep, fpn = pn * kCoarseStep;
    double fbest = 0;
    long fbest_o = (long)(best_o * kCoarseStep);
    for (long d = -(long)kCoarseStep; d <= (long)kCoarseStep; d++) {
        const long o = (long)(best_o * kCoarseStep) + d;
        if (o < 0 || (size_t)o + fpn > ref.frames() || fp0 + fpn > probe.frames())
            continue;
        size_t hit = 0, tot = 0;
        for (size_t i = 0; i < fpn; i += 2) {
            ++tot;
            if (fine_match(probe, fp0 + i, ref, (size_t)o + i))
                ++hit;
        }
        const double s = tot ? (double)hit / (double)tot : 0;
        if (s > fbest) {
            fbest = s;
            fbest_o = o;
        }
    }
    r.score = fbest;
    r.offset_seconds = (double)fbest_o / kFeatRate;
    r.matched = fbest >= 0.70;
    return r;
}

double similarity(const FeatureTrack& a, const FeatureTrack& b, double seconds)
{
    const size_t want = (size_t)(seconds * kFeatRate);
    double best = 0;
    for (long shift = -8; shift <= 8; shift++) {
        // frame i of `a` against frame i + shift of `b`
        const size_t a0 = shift < 0 ? (size_t)(-shift) : 0;
        const size_t b0 = shift > 0 ? (size_t)shift : 0;
        if (a0 >= a.frames() || b0 >= b.frames())
            continue;
        const size_t n = std::min(want, std::min(a.frames() - a0, b.frames() - b0));
        if (n < (size_t)kFeatRate / 2)
            continue;
        size_t hit = 0;
        for (size_t i = 0; i < n; i++)
            if (fine_match(a, a0 + i, b, b0 + i))
                ++hit;
        best = std::max(best, (double)hit / (double)n);
    }
    return best;
}

} // namespace usfm
