// Tests für den Maximierer. Die ersten vier entsprechen LoudnessMaximizerTest.kt aus Player NEXT.
#include "Maximizer.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

using cnx::Maximizer;
using cnx::Settings;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FEHLER %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const double kPi = 3.14159265358979323846;
static float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

static Maximizer make(double rate, int channels, bool enabled, float threshold, float output, uint32_t mask = 0) {
    Maximizer m;
    Settings s;
    s.enabled = enabled;
    s.thresholdDb = threshold;
    s.outputDb = output;
    m.setSettings(s);
    m.prepare(rate, channels, mask);
    return m;
}

// Stereo wie im Kotlin-Test: links x, rechts -0,7 x.
static std::vector<float> runStereo(Maximizer& m, const std::function<float(int)>& input, int frames, int block = 1) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const float x = input(n);
        buf[2 * n] = x;
        buf[2 * n + 1] = -x * 0.7f;
    }
    for (int pos = 0; pos < frames; pos += block) {
        const int cnt = std::min(block, frames - pos);
        m.process(&buf[static_cast<size_t>(pos) * 2], &buf[static_cast<size_t>(pos) * 2], cnt);
    }
    return buf;
}

static void neverAboveTheCeiling() {
    std::printf("neverAboveTheCeiling\n");
    const int rate = 44100;
    Maximizer m = make(rate, 2, true, -12.0f, -1.0f);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, 1999);
    auto out = runStereo(m, [&](int n) {
        const float base = 0.5f * static_cast<float>(std::sin(2 * kPi * 220 * n / rate)) + 0.3f * static_cast<float>(std::sin(2 * kPi * 3100 * n / rate));
        return dist(rng) == 0 ? 2.0f : base;
    }, rate * 5);
    const float ceiling = dbToLin(-1.0f);
    float mx = 0.0f;
    bool nan = false;
    for (float v : out) { mx = std::max(mx, std::fabs(v)); if (v != v) nan = true; }
    CHECK(mx <= ceiling + 1e-6f, "Spitze %.6f ueber der Obergrenze %.6f", mx, ceiling);
    CHECK(!nan, "NaN in der Ausgabe");
}

static void quietSignalOnlyGetsLouderAndDelayed() {
    std::printf("quietSignalOnlyGetsLouderAndDelayed\n");
    const int rate = 44100;
    Maximizer m = make(rate, 2, true, -6.0f, 0.0f);
    auto signal = [&](int n) { return 0.1f * static_cast<float>(std::sin(2 * kPi * 440 * n / rate)); };
    auto out = runStereo(m, signal, 4000);
    const int delay = m.latencyFrames();
    CHECK(delay == 65, "Verzoegerung %d statt 65 Abtastwerte bei 44,1 kHz", delay);
    const float drive = dbToLin(6.0f);
    float worst = 0.0f;
    for (int n = 1000; n < 4000; ++n) worst = std::max(worst, std::fabs(signal(n - delay) * drive - out[2 * n]));
    CHECK(worst <= 1e-4f, "Abweichung %.6f", worst);
}

static void limitingStartsAtTheThreshold() {
    std::printf("limitingStartsAtTheThreshold\n");
    const int rate = 44100;
    {
        Maximizer below = make(rate, 2, true, -6.0f, -1.0f);
        const float quiet = dbToLin(-9.0f);
        auto out = runStereo(below, [&](int n) { return quiet * static_cast<float>(std::sin(2 * kPi * 440 * n / rate)); }, rate / 2);
        float mx = 0.0f;
        for (int n = rate / 4; n < rate / 2; ++n) mx = std::max(mx, std::fabs(out[2 * n]));
        CHECK(std::fabs(mx - dbToLin(-4.0f)) <= 0.005f, "leises Signal: Spitze %.4f statt %.4f", mx, dbToLin(-4.0f));
        CHECK(below.takeMeter().minGain >= 1.0f - 1e-4f, "leises Signal wurde begrenzt");
    }
    {
        Maximizer above = make(rate, 2, true, -6.0f, -1.0f);
        const float loud = dbToLin(-3.0f);
        runStereo(above, [&](int n) { return loud * static_cast<float>(std::sin(2 * kPi * 440 * n / rate)); }, rate / 4);
        above.takeMeter();
        runStereo(above, [&](int n) { return loud * static_cast<float>(std::sin(2 * kPi * 440 * (n + rate / 4) / rate)); }, rate / 4);
        const float redDb = 20.0f * std::log10(above.takeMeter().minGain);
        CHECK(std::fabs(redDb + 3.0f) <= 0.3f, "Reduktion %.2f dB statt -3 dB", redDb);
    }
}

static void recoversAfterAPeak() {
    std::printf("recoversAfterAPeak\n");
    const int rate = 44100;
    Maximizer m = make(rate, 2, true, 0.0f, 0.0f);
    auto out = runStereo(m, [&](int n) { return n == 100 ? 4.0f : 0.2f * static_cast<float>(std::sin(2 * kPi * 440 * n / rate)); }, rate - 2000);
    m.takeMeter();
    auto tail = runStereo(m, [&](int n) { return 0.2f * static_cast<float>(std::sin(2 * kPi * 440 * (n + rate - 2000) / rate)); }, 2000);
    float mx = 0.0f;
    for (int n = 0; n < 2000; ++n) mx = std::max(mx, std::fabs(tail[2 * n]));
    CHECK(std::fabs(mx - 0.2f) <= 0.005f, "Pegel nach der Spitze %.4f statt 0,2", mx);
    const float redDb = 20.0f * std::log10(m.takeMeter().minGain);
    CHECK(redDb > -0.1f, "noch %.2f dB Reduktion", redDb);
    (void)out;
}

static void bypassIsBitExact() {
    std::printf("bypassIsBitExact\n");
    const int rate = 48000;
    Maximizer m = make(rate, 2, false, -18.0f, -1.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-1.5f, 1.5f);  // auch Werte über Vollaussteuerung
    const int frames = rate;
    std::vector<float> in(static_cast<size_t>(frames) * 2), out(in.size());
    for (auto& v : in) v = dist(rng);
    m.process(in.data(), out.data(), frames);
    const int delay = m.latencyFrames();
    CHECK(delay == 71, "Verzoegerung %d statt 71 Abtastwerte bei 48 kHz", delay);
    int diffs = 0;
    for (int n = delay; n < frames; ++n)
        for (int c = 0; c < 2; ++c)
            if (out[2 * n + c] != in[2 * (n - delay) + c]) ++diffs;
    CHECK(diffs == 0, "%d abweichende Abtastwerte im ausgeschalteten Zustand", diffs);
}

static void toggleIsSmoothAndReturnsToExact() {
    std::printf("toggleIsSmoothAndReturnsToExact\n");
    const int rate = 48000;
    Maximizer m = make(rate, 2, false, -24.0f, -1.0f);
    const int delay = m.latencyFrames();
    const int frames = rate * 6;
    std::vector<float> in(static_cast<size_t>(frames) * 2), out(in.size());
    // Gleichanteil-freies, aber nie durch Null gehendes Hüllkurvensignal ist für Verhältnisse ungeeignet;
    // deshalb ein konstanter Pegel mit wechselndem Vorzeichen (Rechteck 1 kHz), leise genug für keine Begrenzung.
    for (int n = 0; n < frames; ++n) {
        const float v = ((n / 24) % 2 == 0 ? 0.01f : -0.01f);
        in[2 * n] = v;
        in[2 * n + 1] = v * 0.5f;
    }
    Settings s = m.settings();
    const int block = 480;
    float maxStep = 0.0f, lastGain = 1.0f, maxGain = 0.0f;
    for (int pos = 0; pos < frames; pos += block) {
        if (pos == rate) { s.enabled = true; m.setSettings(s); }      // nach 1 s einschalten
        if (pos == rate * 3) { s.enabled = false; m.setSettings(s); } // nach 3 s ausschalten
        m.process(&in[static_cast<size_t>(pos) * 2], &out[static_cast<size_t>(pos) * 2], block);
        for (int n = std::max(pos, delay); n < pos + block; ++n) {
            const float g = out[2 * n] / in[2 * (n - delay)];
            maxStep = std::max(maxStep, std::fabs(g - lastGain));
            maxGain = std::max(maxGain, g);
            lastGain = g;
        }
    }
    CHECK(std::fabs(maxGain - dbToLin(23.0f)) < 0.01f * dbToLin(23.0f), "Anhebung %.3f statt %.3f", maxGain, dbToLin(23.0f));
    CHECK(maxStep < 0.02f, "Verstaerkungssprung %.4f beim Umschalten", maxStep);
    int diffs = 0;
    for (int n = rate * 4; n < frames; ++n)
        for (int c = 0; c < 2; ++c)
            if (out[2 * n + c] != in[2 * (n - delay) + c]) ++diffs;
    CHECK(diffs == 0, "%d abweichende Abtastwerte nach dem Ausschalten", diffs);
}

static void blockSizeDoesNotMatter() {
    std::printf("blockSizeDoesNotMatter\n");
    const int rate = 48000;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> noise(rate);
    for (auto& v : noise) v = dist(rng) * (rng() % 5 == 0 ? 1.0f : 0.05f);
    std::vector<float> ref;
    for (int block : {1, 7, 480, 4096}) {
        Maximizer m = make(rate, 2, true, -18.0f, -1.0f);
        auto out = runStereo(m, [&](int n) { return noise[n]; }, rate, block);
        if (ref.empty()) ref = out;
        else CHECK(out == ref, "Blockgroesse %d liefert ein anderes Ergebnis", block);
    }
}

static void multiChannelSharesOneGain() {
    std::printf("multiChannelSharesOneGain\n");
    for (double rate : {48000.0, 192000.0}) {
        const int ch = 6;
        Maximizer m = make(rate, ch, true, -20.0f, -3.0f, 0x3F);
        const int frames = static_cast<int>(rate);
        const int delay = m.latencyFrames();
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> dist(0.2f, 1.0f);
        std::vector<float> in(static_cast<size_t>(frames) * ch), out(in.size());
        for (int n = 0; n < frames; ++n)
            for (int c = 0; c < ch; ++c)
                in[static_cast<size_t>(n) * ch + c] = dist(rng) * ((n + c) % 2 ? 1.0f : -1.0f);
        m.process(in.data(), out.data(), frames);
        const float ceiling = dbToLin(-3.0f);
        float mx = 0.0f, spread = 0.0f;
        for (int n = delay + 2000; n < frames; ++n) {
            float gMin = 1e9f, gMax = 0.0f;
            for (int c = 0; c < ch; ++c) {
                const float o = out[static_cast<size_t>(n) * ch + c];
                mx = std::max(mx, std::fabs(o));
                if (std::fabs(o) >= ceiling) continue;  // an der Klammer ist das Verhältnis nicht aussagekräftig
                const float g = o / in[static_cast<size_t>(n - delay) * ch + c];
                gMin = std::min(gMin, g);
                gMax = std::max(gMax, g);
            }
            if (gMax > 0.0f && gMin < 1e9f) spread = std::max(spread, (gMax - gMin) / gMax);
        }
        CHECK(mx <= ceiling + 1e-6f, "%.0f Hz: Spitze %.6f ueber %.6f", rate, mx, ceiling);
        CHECK(spread < 1e-4f, "%.0f Hz: Kanaele mit unterschiedlicher Verstaerkung (%.6f)", rate, spread);
        const cnx::MeterBlock mb = m.takeMeter();
        CHECK(mb.inL > 0.9f && mb.inR > 0.9f && mb.outL <= ceiling + 1e-6f && mb.minGain < 0.2f, "%.0f Hz: Pegelanzeige unplausibel", rate);
    }
}

static void brokenInputStaysFinite() {
    std::printf("brokenInputStaysFinite\n");
    Maximizer m = make(48000, 2, true, -18.0f, -1.0f);
    std::vector<float> buf(9600);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = 0.1f;
    buf[100] = std::numeric_limits<float>::quiet_NaN();
    buf[201] = std::numeric_limits<float>::infinity();
    buf[302] = -std::numeric_limits<float>::infinity();
    buf[403] = 1.0e30f;
    buf[504] = 1.0e-42f;  // denormal
    m.process(buf.data(), buf.data(), 4800);
    bool ok = true;
    for (float v : buf) if (!(v == v) || std::fabs(v) > 1.0f) ok = false;
    CHECK(ok, "Ausgabe nicht endlich oder ueber Vollaussteuerung");
}

static void speed() {
    const int rate = 48000, seconds = 120;
    Maximizer m = make(rate, 2, true, -18.0f, -1.0f);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> buf(480 * 2);
    for (auto& v : buf) v = dist(rng);
    const auto t0 = std::chrono::steady_clock::now();
    volatile float sink = 0.0f;
    for (int i = 0; i < seconds * 100; ++i) { m.process(buf.data(), buf.data(), 480); sink = sink + buf[0]; for (auto& v : buf) v = v * 0.5f + 0.4f * ((i & 1) ? 1.0f : -1.0f); }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Tempo: %d s Stereo/48 kHz in %.1f ms (%.0f-fache Echtzeit, %.3f %% eines Kerns)\n", seconds, sec * 1000.0, seconds / sec, 100.0 * sec / seconds);
}

int main() {
    neverAboveTheCeiling();
    quietSignalOnlyGetsLouderAndDelayed();
    limitingStartsAtTheThreshold();
    recoversAfterAPeak();
    bypassIsBitExact();
    toggleIsSmoothAndReturnsToExact();
    blockSizeDoesNotMatter();
    multiChannelSharesOneGain();
    brokenInputStaysFinite();
    speed();
    std::printf("%d Pruefungen, %d fehlgeschlagen\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
