// Compressor NEXT – Lautheits-Maximierer nach Art von LoudMax.
//
// Portierung von Player NEXT (playback/dsp/LoudnessMaximizer.kt), gleiche Mathematik:
// Ein vorausschauender Brickwall-Limiter. Eingangsspitzen über der Schwelle werden begrenzt; eine
// Spitze genau auf der Schwelle kommt beim Ausgangspegel an. Leise Stellen werden also um
// (Ausgang − Schwelle) lauter, und keine Spitze überschreitet den Ausgangspegel.
//
// Verstärkungsweg je Abtastwert:
//   benötigte Verstärkung -> Minimum über 1,5 ms -> sofortiger Angriff / 120 ms Rückkehr
//   -> gleitender Mittelwert über 1,5 ms. Das Audio läuft 1,5 ms verzögert.
// Minimum und Mittelwert gleicher Länge garantieren, dass an der verzögerten Spitze nie mehr
// Verstärkung anliegt, als diese Spitze verträgt. Alle Kanäle teilen eine Verstärkung, damit das
// Klangbild nicht wandert.
//
// Zusätze für den Betrieb als Windows-Systemeffekt:
//   - Schwelle, Ausgang und Ein/Aus werden über 20 ms überblendet (kein Knacken beim Umschalten).
//   - Die Verzögerung bleibt auch ausgeschaltet bestehen; ausgeschaltet wird bitgenau durchgereicht.
//   - Beliebig viele Kanäle (5.1/7.1); die Pegelanzeige fasst sie zu Links/Rechts zusammen.
//
// Echtzeitfähig: process() reserviert keinen Speicher, sperrt nichts und ruft kein Betriebssystem auf.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cnx {

struct Settings {
    bool  enabled = true;
    float thresholdDb = -18.0f;  // -30 .. 0
    float outputDb = -1.0f;      // -12 .. 0
};

struct MeterBlock {
    float inL = 0.0f, inR = 0.0f;    // Spitzen vor dem Maximierer (linear)
    float outL = 0.0f, outR = 0.0f;  // Spitzen danach (linear)
    float minGain = 1.0f;            // stärkste Begrenzung (1 = keine)
};

class Maximizer {
public:
    static constexpr double kWindowSec = 0.0015;
    static constexpr double kReleaseSec = 0.120;
    static constexpr double kRampSec = 0.020;
    static constexpr float kThresholdMinDb = -30.0f;
    static constexpr float kOutputMinDb = -12.0f;

    // Nicht echtzeitfähig (reserviert Speicher). channelMask = 0: Standardbelegung annehmen.
    void prepare(double sampleRate, int channels, uint32_t channelMask) {
        fs_ = std::clamp(sampleRate, 8000.0, 768000.0);
        ch_ = std::max(1, channels);
        window_ = std::max(1, static_cast<int>(std::lround(kWindowSec * fs_)));
        delay_ = window_ - 1;
        releaseCoef_ = static_cast<float>(std::exp(-1.0 / (kReleaseSec * fs_)));
        rampLen_ = std::max(1, static_cast<int>(std::lround(kRampSec * fs_)));

        line_.assign(static_cast<size_t>(std::max(1, delay_)) * ch_, 0.0f);
        minValue_.assign(static_cast<size_t>(window_) + 1, 1.0f);
        minIndex_.assign(static_cast<size_t>(window_) + 1, 0);
        avg_.assign(window_, 1.0f);
        frame_.assign(ch_, 0.0f);

        // Zuordnung der Kanäle zu den beiden Balken der Pegelanzeige
        toL_.assign(ch_, 1);
        toR_.assign(ch_, 1);
        uint32_t mask = channelMask;
        if (mask == 0) {
            if (ch_ == 2) mask = 0x3;
            else if (ch_ == 4) mask = 0x33;
            else if (ch_ == 6) mask = 0x3F;
            else if (ch_ == 8) mask = 0x63F;
        }
        if (mask != 0 && ch_ > 1) {
            const uint32_t leftBits = 0x1 | 0x10 | 0x40 | 0x200;    // vorn links, hinten links, links der Mitte, seitlich links
            const uint32_t rightBits = 0x2 | 0x20 | 0x80 | 0x400;
            int idx = 0;
            for (uint32_t bit = 0; bit < 32 && idx < ch_; ++bit) {
                const uint32_t m = 1u << bit;
                if (!(mask & m)) continue;
                if (m & leftBits) toR_[idx] = 0;
                else if (m & rightBits) toL_[idx] = 0;
                ++idx;  // Center, LFE usw. zählen auf beiden Seiten
            }
        }

        drive_ = driveTarget_ = set_.enabled ? driveFor(set_) : 1.0f;
        ceiling_ = ceilingTarget_ = ceilingFor(set_);
        rampLeft_ = 0;
        limiting_ = set_.enabled;
        reset();
    }

    // Echtzeitfähig; zwischen zwei process()-Aufrufen aufrufen.
    void setSettings(const Settings& s) {
        Settings n = s;
        if (!(n.thresholdDb >= kThresholdMinDb)) n.thresholdDb = kThresholdMinDb;
        if (n.thresholdDb > 0.0f) n.thresholdDb = 0.0f;
        if (!(n.outputDb >= kOutputMinDb)) n.outputDb = kOutputMinDb;
        if (n.outputDb > 0.0f) n.outputDb = 0.0f;
        if (n.enabled == set_.enabled && n.thresholdDb == set_.thresholdDb && n.outputDb == set_.outputDb) return;
        set_ = n;
        driveTarget_ = n.enabled ? driveFor(n) : 1.0f;
        if (n.enabled) {
            ceilingTarget_ = ceilingFor(n);
            limiting_ = true;
        }
        // Beim Ausschalten bleibt die Obergrenze stehen, bis die Anhebung zurückgefahren ist.
        rampLeft_ = rampLen_;
        driveStep_ = (driveTarget_ - drive_) / static_cast<float>(rampLen_);
        ceilingStep_ = (ceilingTarget_ - ceiling_) / static_cast<float>(rampLen_);
    }
    const Settings& settings() const { return set_; }

    void reset() {
        std::fill(line_.begin(), line_.end(), 0.0f);
        linePos_ = 0;
        minHead_ = 0;
        minSize_ = 0;
        frameIndex_ = 0;
        released_ = 1.0f;
        std::fill(avg_.begin(), avg_.end(), 1.0f);
        avgPos_ = 0;
        avgSum_ = static_cast<double>(window_);
        unityRun_ = window_;
        meter_ = MeterBlock();
    }

    int latencyFrames() const { return delay_; }
    int channels() const { return ch_; }
    // Zuordnung eines Kanals zu den Balken "L"/"R" der Pegelanzeige (Center, LFE usw.: beide)
    bool feedsLeft(int c) const { return toL_[static_cast<size_t>(c)] != 0; }
    bool feedsRight(int c) const { return toR_[static_cast<size_t>(c)] != 0; }
    double sampleRate() const { return fs_; }
    bool engaged() const { return limiting_; }

    // in und out dürfen derselbe Puffer sein (verschachtelte Kanäle, 32-Bit-Gleitkomma).
    void process(const float* in, float* out, int frames) {
        const int ch = ch_;
        const int window = window_;
        const int qSize = window + 1;
        MeterBlock m = meter_;

        for (int n = 0; n < frames; ++n) {
            const float* src = in + static_cast<size_t>(n) * ch;
            float* dst = out + static_cast<size_t>(n) * ch;

            if (rampLeft_ > 0) {
                if (--rampLeft_ == 0) {
                    drive_ = driveTarget_;
                    ceiling_ = ceilingTarget_;
                    if (!set_.enabled) limiting_ = false;
                } else {
                    drive_ += driveStep_;
                    ceiling_ += ceilingStep_;
                }
            }
            const float drive = drive_;
            const float ceiling = ceiling_;

            float peak = 0.0f;
            for (int c = 0; c < ch; ++c) {
                float v = src[c];
                if (!(v == v) || v > 1.0e6f || v < -1.0e6f) v = 0.0f;  // kaputte Werte entschärfen
                const float a = std::fabs(v);
                if (toL_[c] && a > m.inL) m.inL = a;
                if (toR_[c] && a > m.inR) m.inR = a;
                v *= drive;
                frame_[c] = v;
                const float d = std::fabs(v);
                if (d > peak) peak = d;
            }
            const float required = (limiting_ && peak > ceiling) ? ceiling / peak : 1.0f;

            // Gleitendes Minimum (monotone Warteschlange über die letzten `window` Abtastwerte)
            while (minSize_ > 0) {
                int last = minHead_ + minSize_ - 1;
                if (last >= qSize) last -= qSize;
                if (minValue_[last] >= required) --minSize_; else break;
            }
            int slot = minHead_ + minSize_;
            if (slot >= qSize) slot -= qSize;
            minValue_[slot] = required;
            minIndex_[slot] = frameIndex_;
            ++minSize_;
            while (minIndex_[minHead_] + static_cast<uint64_t>(window) <= frameIndex_) {
                if (++minHead_ >= qSize) minHead_ = 0;
                --minSize_;
            }
            ++frameIndex_;
            const float held = minValue_[minHead_];

            // Sofortiger Angriff, weiche Rückkehr: nie über dem gehaltenen Minimum.
            released_ = held < released_ ? held : held - (held - released_) * releaseCoef_;
            if (released_ > 0.999999f) released_ = 1.0f;

            avgSum_ += static_cast<double>(released_) - static_cast<double>(avg_[avgPos_]);
            avg_[avgPos_] = released_;
            if (++avgPos_ >= window) avgPos_ = 0;
            float gain;
            if (released_ == 1.0f) {
                if (unityRun_ < window) ++unityRun_;
            } else {
                unityRun_ = 0;
            }
            if (unityRun_ >= window) {
                gain = 1.0f;                               // ganzes Fenster ohne Begrenzung: exakt 1
                avgSum_ = static_cast<double>(window);     // und Rundungsreste der laufenden Summe verwerfen
            } else {
                gain = static_cast<float>(avgSum_ / window);
                if (gain > 1.0f) gain = 1.0f;
            }
            if (gain < m.minGain) m.minGain = gain;

            const bool clamp = limiting_ || gain < 1.0f;
            for (int c = 0; c < ch; ++c) {
                float delayed;
                if (delay_ == 0) {
                    delayed = frame_[c];
                } else {
                    float& cell = line_[static_cast<size_t>(linePos_) * ch + c];
                    delayed = cell;
                    cell = frame_[c];
                }
                float y = delayed * gain;
                // Die Garantie gilt bis auf Rundung; die Klammer fängt das letzte Bit.
                if (clamp) { if (y > ceiling) y = ceiling; else if (y < -ceiling) y = -ceiling; }
                dst[c] = y;
                const float a = std::fabs(y);
                if (toL_[c] && a > m.outL) m.outL = a;
                if (toR_[c] && a > m.outR) m.outR = a;
            }
            if (delay_ > 0 && ++linePos_ >= delay_) linePos_ = 0;
        }
        meter_ = m;
    }

    // Liefert die Werte der Pegelanzeige seit dem letzten Aufruf und setzt sie zurück.
    MeterBlock takeMeter() {
        MeterBlock m = meter_;
        meter_ = MeterBlock();
        return m;
    }

private:
    static float driveFor(const Settings& s) { return std::pow(10.0f, (s.outputDb - s.thresholdDb) / 20.0f); }
    static float ceilingFor(const Settings& s) { return std::pow(10.0f, s.outputDb / 20.0f); }

    double fs_ = 48000.0;
    int ch_ = 2;
    int window_ = 72;
    int delay_ = 71;
    float releaseCoef_ = 0.0f;
    int rampLen_ = 960;

    Settings set_;
    float drive_ = 1.0f, driveTarget_ = 1.0f, driveStep_ = 0.0f;
    float ceiling_ = 1.0f, ceilingTarget_ = 1.0f, ceilingStep_ = 0.0f;
    int rampLeft_ = 0;
    bool limiting_ = true;

    std::vector<float> line_;   // Verzögerungsleitung
    int linePos_ = 0;
    std::vector<float> minValue_;
    std::vector<uint64_t> minIndex_;
    int minHead_ = 0;
    int minSize_ = 0;
    uint64_t frameIndex_ = 0;
    float released_ = 1.0f;
    std::vector<float> avg_;    // gleitender Mittelwert der Verstärkung
    int avgPos_ = 0;
    double avgSum_ = 0.0;
    int unityRun_ = 0;

    std::vector<float> frame_;
    std::vector<uint8_t> toL_, toR_;
    MeterBlock meter_;
};

} // namespace cnx
