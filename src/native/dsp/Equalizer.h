// Compressor NEXT – parametrischer Equalizer hinter dem Maximierer.
//
// Bis zu acht Biquad-Filter nach dem RBJ-Kochbuch – dieselben Formeln wie in Player NEXT
// (playback/dsp/Biquad.kt, dort Glocke und Tiefen-Kuhschwanz), ergänzt um Höhen-Kuhschwanz, Hochpass,
// Tiefpass und Kerbe. Transponierte Direktform II mit doppelt genauen Zuständen, damit auch 20 Hz bei
// 192 kHz sauber bleiben. Die Kuhschwänze nehmen die Güte direkt (Q 0,71 entspricht Player NEXTs
// fester Flankensteilheit S = 1).
//
// Damit beim Einstellen nichts knackt:
//   - Frequenz, Verstärkung und Güte gleiten (Zeitkonstante 30 ms, Koeffizienten alle 32 Abtastwerte neu).
//   - Ein neues, entferntes oder abgeschaltetes Band wird über 20 ms ein- bzw. ausgeblendet
//     (y = x + m·(Filter(x) − x)); bei einem Typwechsel erst aus-, dann mit dem neuen Typ eingeblendet.
//   - Der Pegel hinter dem Equalizer (Auto-Pegel) gleitet ebenso.
// Ist kein Band aktiv und der Pegel 0 dB, rührt process() den Puffer nicht an (bitgenau).
//
// Echtzeitfähig: process() und setSettings() reservieren keinen Speicher, sperren nichts und rufen kein
// Betriebssystem auf.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "SharedState.h"

namespace cnx {

enum class EqType : uint16_t {
    Peak = kEqPeak,
    LowShelf = kEqLowShelf,
    HighShelf = kEqHighShelf,
    LowCut = kEqLowCut,
    HighCut = kEqHighCut,
    Notch = kEqNotch,
};

// Höchste Güte je Typ: Ein Kuhschwanz mit hoher Güte bekommt einen Resonanzbuckel weit über seine
// Verstärkung hinaus, ein Hoch-/Tiefpass eine Resonanzspitze von 20·log10(Q) dB.
inline float eqMaxQ(EqType t) {
    switch (t) {
    case EqType::LowShelf:
    case EqType::HighShelf: return 2.0f;
    case EqType::LowCut:
    case EqType::HighCut: return 10.0f;
    default: return 20.0f;
    }
}

struct EqBand {
    bool used = false;
    bool on = true;
    EqType type = EqType::Peak;
    float freqHz = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
};

struct EqSettings {
    bool enabled = false;
    float levelDb = 0.0f;
    EqBand bands[kEqSlots];
};

struct BiquadCoefs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;  // a0 = 1
};

constexpr float kEqFreqMin = 10.0f, kEqFreqMax = 24000.0f;
constexpr float kEqGainLimit = 24.0f;
constexpr float kEqQMin = 0.05f, kEqQMax = 20.0f;
constexpr float kEqLevelMin = -30.0f, kEqLevelMax = 12.0f;

// RBJ-Kochbuch ("Cookbook formulae for audio EQ biquad filter coefficients"), auf a0 = 1 normiert.
inline BiquadCoefs designBiquad(EqType type, double fs, double freqHz, double gainDb, double q) {
    const double f = std::clamp(freqHz, 1.0, 0.49 * fs);
    const double w0 = 2.0 * 3.14159265358979323846 * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * std::clamp(q, 0.01, 100.0));
    const double a = std::pow(10.0, gainDb / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case EqType::Peak:
        b0 = 1 + alpha * a;
        b1 = -2 * cw;
        b2 = 1 - alpha * a;
        a0 = 1 + alpha / a;
        a1 = -2 * cw;
        a2 = 1 - alpha / a;
        break;
    case EqType::LowShelf: {
        const double sq = 2 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1) - (a - 1) * cw + sq);
        b1 = 2 * a * ((a - 1) - (a + 1) * cw);
        b2 = a * ((a + 1) - (a - 1) * cw - sq);
        a0 = (a + 1) + (a - 1) * cw + sq;
        a1 = -2 * ((a - 1) + (a + 1) * cw);
        a2 = (a + 1) + (a - 1) * cw - sq;
        break;
    }
    case EqType::HighShelf: {
        const double sq = 2 * std::sqrt(a) * alpha;
        b0 = a * ((a + 1) + (a - 1) * cw + sq);
        b1 = -2 * a * ((a - 1) + (a + 1) * cw);
        b2 = a * ((a + 1) + (a - 1) * cw - sq);
        a0 = (a + 1) - (a - 1) * cw + sq;
        a1 = 2 * ((a - 1) - (a + 1) * cw);
        a2 = (a + 1) - (a - 1) * cw - sq;
        break;
    }
    case EqType::LowCut:
        b0 = (1 + cw) / 2;
        b1 = -(1 + cw);
        b2 = (1 + cw) / 2;
        a0 = 1 + alpha;
        a1 = -2 * cw;
        a2 = 1 - alpha;
        break;
    case EqType::HighCut:
        b0 = (1 - cw) / 2;
        b1 = 1 - cw;
        b2 = (1 - cw) / 2;
        a0 = 1 + alpha;
        a1 = -2 * cw;
        a2 = 1 - alpha;
        break;
    case EqType::Notch:
        b0 = 1;
        b1 = -2 * cw;
        b2 = 1;
        a0 = 1 + alpha;
        a1 = -2 * cw;
        a2 = 1 - alpha;
        break;
    }
    BiquadCoefs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

// Betrag des Frequenzgangs in dB (für Tests; die Oberfläche rechnet dasselbe in EqMath.cs).
inline double biquadMagnitudeDb(const BiquadCoefs& c, double fs, double freqHz) {
    const double w = 2.0 * 3.14159265358979323846 * freqHz / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const std::complex<double> h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
}

class Equalizer {
public:
    static constexpr int kSub = 32;              // Abtastwerte je Koeffizienten-Nachführung
    static constexpr double kGlideSec = 0.030;   // Zeitkonstante beim Gleiten der Parameter
    static constexpr double kFadeSec = 0.020;    // Ein-/Ausblenden eines Bandes

    // Nicht echtzeitfähig (reserviert Speicher). Springt ohne Überblendung auf die zuletzt gesetzten Einstellungen.
    void prepare(double sampleRate, int channels) {
        fs_ = std::clamp(sampleRate, 8000.0, 768000.0);
        ch_ = std::max(1, channels);
        z_.assign(static_cast<size_t>(kEqSlots) * ch_ * 2, 0.0);
        glide_ = 1.0 - std::exp(-kSub / (kGlideSec * fs_));
        fadeStep_ = 1.0 / std::max(1.0, kFadeSec * fs_);
        for (Stage& s : st_) {
            jumpToTarget(s);
            s.mix = s.mixStart = s.want ? 1.0 : 0.0;
        }
        level_ = levelStart_ = levelTarget_;
    }

    // Echtzeitfähig; zwischen zwei process()-Aufrufen aufrufen.
    void setSettings(const EqSettings& in) {
        for (int i = 0; i < kEqSlots; ++i) {
            const EqBand& b = in.bands[i];
            Stage& s = st_[i];
            const auto code = static_cast<uint16_t>(b.type);
            s.tType = code < kEqTypeCount ? b.type : EqType::Peak;
            s.tLogF = std::log(clampFinite(b.freqHz, kEqFreqMin, kEqFreqMax, 1000.0f));
            s.tGain = clampFinite(b.gainDb, -kEqGainLimit, kEqGainLimit, 0.0f);
            s.tLogQ = std::log(clampFinite(b.q, kEqQMin, eqMaxQ(s.tType), 1.0f));
            s.want = in.enabled && b.used && b.on;
        }
        levelTarget_ = in.enabled ? std::pow(10.0, clampFinite(in.levelDb, kEqLevelMin, kEqLevelMax, 0.0f) / 20.0) : 1.0;
    }

    // Leert die Filterzustände (nach Stille), Einstellungen bleiben.
    void reset() { std::fill(z_.begin(), z_.end(), 0.0); }

    // false: process() tut nichts (bitgenau).
    bool active() const {
        if (level_ != 1.0 || levelTarget_ != 1.0) return true;
        for (const Stage& s : st_)
            if (s.want || s.mix > 0.0) return true;
        return false;
    }

    // Verarbeitet verschachtelte Kanäle (32-Bit-Gleitkomma) an Ort und Stelle.
    void process(float* buf, int frames) {
        if (!active()) return;
        for (int done = 0; done < frames;) {
            const int n = std::min(kSub, frames - done);
            float* p = buf + static_cast<size_t>(done) * ch_;
            for (int i = 0; i < kEqSlots; ++i) {
                Stage& s = st_[i];
                if (advance(s, i, n)) run(s, i, p, n);
            }
            applyLevel(p, n);
            done += n;
        }
    }

    double sampleRate() const { return fs_; }
    int channels() const { return ch_; }

private:
    struct Stage {
        bool want = false;
        EqType tType = EqType::Peak;
        double tLogF = 6.907755, tGain = 0.0, tLogQ = 0.0;
        EqType type = EqType::Peak;
        double logF = 6.907755, gain = 0.0, logQ = 0.0;
        double mix = 0.0, mixStart = 0.0;
        BiquadCoefs c;
    };

    static float clampFinite(float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return std::clamp(v, lo, hi);
    }

    void jumpToTarget(Stage& s) {
        s.type = s.tType;
        s.logF = s.tLogF;
        s.gain = s.tGain;
        s.logQ = s.tLogQ;
        s.c = designBiquad(s.type, fs_, std::exp(s.logF), s.gain, std::exp(s.logQ));
    }

    void glideTo(double& v, double target, double eps, bool& changed) const {
        const double d = target - v;
        if (d == 0.0) return;
        v = std::fabs(d) <= eps ? target : v + d * glide_;
        changed = true;
    }

    // Führt Überblendung und Parameter eines Bandes um einen Abschnitt weiter; false = Band ruht.
    bool advance(Stage& s, int index, int n) {
        if (s.mix <= 0.0) {
            if (!s.want) return false;
            // Einblenden beginnt immer mit den Zielwerten und leerem Zustand (auch nach einem Typwechsel).
            jumpToTarget(s);
            double* z = &z_[static_cast<size_t>(index) * ch_ * 2];
            std::fill(z, z + static_cast<size_t>(ch_) * 2, 0.0);
        }
        const double target = (s.want && s.type == s.tType) ? 1.0 : 0.0;
        s.mixStart = s.mix;
        const double step = fadeStep_ * n;
        s.mix = target > s.mix ? std::min(target, s.mix + step) : std::max(target, s.mix - step);
        if (s.type == s.tType) {
            bool changed = false;
            glideTo(s.logF, s.tLogF, 1e-4, changed);
            glideTo(s.gain, s.tGain, 1e-3, changed);
            glideTo(s.logQ, s.tLogQ, 1e-4, changed);
            if (changed) s.c = designBiquad(s.type, fs_, std::exp(s.logF), s.gain, std::exp(s.logQ));
        }
        return true;
    }

    void run(const Stage& s, int index, float* p, int n) {
        const BiquadCoefs k = s.c;
        double* z = &z_[static_cast<size_t>(index) * ch_ * 2];
        const bool full = s.mixStart >= 1.0 && s.mix >= 1.0;
        const double m0 = s.mixStart, dm = (s.mix - s.mixStart) / n;
        for (int c = 0; c < ch_; ++c) {
            double z1 = z[2 * c], z2 = z[2 * c + 1];
            float* q = p + c;
            if (full) {
                for (int i = 0; i < n; ++i, q += ch_) {
                    const double x = *q;
                    const double y = k.b0 * x + z1;
                    z1 = k.b1 * x - k.a1 * y + z2;
                    z2 = k.b2 * x - k.a2 * y;
                    *q = static_cast<float>(y);
                }
            } else {
                for (int i = 0; i < n; ++i, q += ch_) {
                    const double x = *q;
                    const double y = k.b0 * x + z1;
                    z1 = k.b1 * x - k.a1 * y + z2;
                    z2 = k.b2 * x - k.a2 * y;
                    const double m = m0 + dm * (i + 1);
                    *q = static_cast<float>(x + m * (y - x));
                }
            }
            // Ausklingende Zustände nicht in den langsamen Bereich subnormaler Zahlen laufen lassen.
            if (std::fabs(z1) < 1e-30) z1 = 0.0;
            if (std::fabs(z2) < 1e-30) z2 = 0.0;
            z[2 * c] = z1;
            z[2 * c + 1] = z2;
        }
    }

    void applyLevel(float* p, int n) {
        levelStart_ = level_;
        const double d = levelTarget_ - level_;
        if (d != 0.0) level_ = std::fabs(d) <= 1e-4 ? levelTarget_ : level_ + d * glide_;  // Rest −80 dB: unhörbar
        if (levelStart_ == 1.0 && level_ == 1.0) return;
        const double dl = (level_ - levelStart_) / n;
        for (int i = 0; i < n; ++i) {
            const auto g = static_cast<float>(levelStart_ + dl * (i + 1));
            float* f = p + static_cast<size_t>(i) * ch_;
            for (int c = 0; c < ch_; ++c) f[c] *= g;
        }
    }

    double fs_ = 48000.0;
    int ch_ = 2;
    double glide_ = 0.0;
    double fadeStep_ = 0.0;
    Stage st_[kEqSlots];
    std::vector<double> z_;
    double level_ = 1.0, levelStart_ = 1.0, levelTarget_ = 1.0;
};

} // namespace cnx
