// Tests für den Equalizer hinter dem Maximierer.
#include "Equalizer.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <vector>

using cnx::BiquadCoefs;
using cnx::EqBand;
using cnx::Equalizer;
using cnx::EqSettings;
using cnx::EqType;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FEHLER %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const double kPi = 3.14159265358979323846;
static const EqType kAllTypes[] = {EqType::Peak, EqType::LowShelf, EqType::HighShelf, EqType::LowCut, EqType::HighCut, EqType::Notch};
static const char* const kTypeNames[] = {"Glocke", "Tiefen-Kuhschwanz", "Hoehen-Kuhschwanz", "Hochpass", "Tiefpass", "Kerbe"};

static EqBand band(EqType type, float freq, float gain, float q, bool on = true) {
    EqBand b;
    b.used = true;
    b.on = on;
    b.type = type;
    b.freqHz = freq;
    b.gainDb = gain;
    b.q = q;
    return b;
}

static Equalizer make(double rate, int channels, const EqSettings& s) {
    Equalizer e;
    e.setSettings(s);
    e.prepare(rate, channels);
    return e;
}

// Verstärkung eines eingeschwungenen Sinus in dB (Mono, Effektivwert der zweiten Hälfte).
static double measureDb(Equalizer& e, double rate, double freq, double seconds = 0.5) {
    const int frames = static_cast<int>(rate * seconds);
    std::vector<float> buf(static_cast<size_t>(frames));
    for (int n = 0; n < frames; ++n) buf[n] = 0.25f * static_cast<float>(std::sin(2 * kPi * freq * n / rate));
    std::vector<float> ref = buf;
    for (int pos = 0; pos < frames; pos += 480) e.process(&buf[pos], std::min(480, frames - pos));
    double si = 0, so = 0;
    for (int n = frames / 2; n < frames; ++n) { si += double(ref[n]) * ref[n]; so += double(buf[n]) * buf[n]; }
    return 10.0 * std::log10(so / si);
}

static void emptyAndDisabledAreBitExact() {
    std::printf("emptyAndDisabledAreBitExact\n");
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> in(48000 * 2);
    for (auto& v : in) v = dist(rng);

    EqSettings empty;
    empty.enabled = true;
    Equalizer a = make(48000, 2, empty);
    std::vector<float> buf = in;
    a.process(buf.data(), 48000);
    CHECK(buf == in && !a.active(), "ohne Baender muss der Puffer unberuehrt bleiben");

    EqSettings off;
    off.enabled = false;
    off.bands[0] = band(EqType::Peak, 1000, 9, 1);
    off.levelDb = -6;
    Equalizer b = make(48000, 2, off);
    buf = in;
    b.process(buf.data(), 48000);
    CHECK(buf == in && !b.active(), "ausgeschaltet muss der Puffer unberuehrt bleiben");

    EqSettings bypassed;
    bypassed.enabled = true;
    bypassed.bands[2] = band(EqType::LowCut, 200, 0, 0.7f, false);
    Equalizer c = make(48000, 2, bypassed);
    buf = in;
    c.process(buf.data(), 48000);
    CHECK(buf == in, "ein abgeschaltetes Band darf nichts veraendern");
}

static void responseMatchesDesign() {
    std::printf("responseMatchesDesign\n");
    const double rates[] = {44100, 48000, 96000};
    struct Case { EqType type; float freq, gain, q; };
    const Case cases[] = {
        {EqType::Peak, 1000, 6, 1.41f},   {EqType::Peak, 120, -9, 3},       {EqType::LowShelf, 150, 5, 0.71f},
        {EqType::HighShelf, 6000, -4, 0.71f}, {EqType::LowCut, 80, 0, 0.71f}, {EqType::HighCut, 9000, 0, 0.71f},
        {EqType::Notch, 3000, 0, 4},
    };
    const double probes[] = {40, 100, 300, 1000, 2500, 7000, 15000};
    for (double rate : rates) {
        for (const Case& k : cases) {
            EqSettings s;
            s.enabled = true;
            s.bands[3] = band(k.type, k.freq, k.gain, k.q);
            const BiquadCoefs c = cnx::designBiquad(k.type, rate, k.freq, k.gain, k.q);
            double worst = 0;
            for (double f : probes) {
                if (k.type == EqType::Notch && std::fabs(std::log2(f / k.freq)) < 0.2) continue;  // im Kerbgrund misst man nur Rauschen
                Equalizer e = make(rate, 1, s);
                const double expected = cnx::biquadMagnitudeDb(c, rate, f);
                if (expected < -40) continue;
                const double got = measureDb(e, rate, f);
                worst = std::max(worst, std::fabs(got - expected));
                CHECK(std::fabs(got - expected) < 0.05, "%s bei %.0f Hz (%.0f Hz Abtastrate): %.3f dB statt %.3f dB",
                      kTypeNames[static_cast<int>(k.type)], f, rate, got, expected);
            }
        }
    }
    // Eckpunkte, die man aus dem Kochbuch kennt
    EqSettings s;
    s.enabled = true;
    s.bands[0] = band(EqType::Peak, 1000, 6, 1.41f);
    Equalizer e = make(48000, 1, s);
    const double atCenter = measureDb(e, 48000, 1000);
    CHECK(std::fabs(atCenter - 6.0) < 0.02, "Glocke +6 dB bei 1 kHz: %.3f dB", atCenter);
    s.bands[0] = band(EqType::LowCut, 100, 0, 0.7071f);
    Equalizer hp = make(48000, 1, s);
    const double at25 = measureDb(hp, 48000, 25, 1.0);
    CHECK(at25 < -22.0, "Hochpass 100 Hz bei 25 Hz: %.1f dB (12 dB/Oktave erwartet)", at25);
    Equalizer hp2 = make(48000, 1, s);
    const double at100 = measureDb(hp2, 48000, 100, 1.0);
    CHECK(std::fabs(at100 + 3.01) < 0.05, "Hochpass 100 Hz (Q 0,71) bei 100 Hz: %.2f dB statt -3 dB", at100);
}

static void bandsAddUp() {
    std::printf("bandsAddUp\n");
    EqSettings s;
    s.enabled = true;
    s.bands[0] = band(EqType::LowShelf, 120, 4, 0.71f);
    s.bands[5] = band(EqType::Peak, 2500, -3, 2);
    s.bands[7] = band(EqType::HighCut, 14000, 0, 0.71f);
    s.levelDb = -4;
    for (double f : {60.0, 500.0, 2500.0, 12000.0}) {
        double expected = -4;
        for (const EqBand& b : s.bands)
            if (b.used) expected += cnx::biquadMagnitudeDb(cnx::designBiquad(b.type, 48000, b.freqHz, b.gainDb, b.q), 48000, f);
        Equalizer e = make(48000, 1, s);
        const double got = measureDb(e, 48000, f);
        CHECK(std::fabs(got - expected) < 0.05, "Summe bei %.0f Hz: %.3f dB statt %.3f dB", f, got, expected);
    }
}

// Einstellungen im laufenden Betrieb ändern darf nicht knacken. Ein lineares Filter erzeugt aus einem
// 200-Hz-Sinus keine hohen Frequenzen; alles oberhalb von 2 kHz am Ausgang stammt also vom Umschalten.
static void changesDoNotClick() {
    std::printf("changesDoNotClick\n");
    const double rate = 48000;
    const int frames = static_cast<int>(rate * 20);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> typeDist(0, 5), slotDist(0, cnx::kEqSlots - 1), coin(0, 5);
    std::uniform_real_distribution<float> logF(std::log(50.0f), std::log(10000.0f)), gain(-12, 12), logQ(std::log(0.3f), std::log(5.0f));

    EqSettings s;
    s.enabled = true;
    Equalizer e = make(rate, 1, s);
    // Hochpass 4. Ordnung bei 2 kHz zum Messen
    const BiquadCoefs h1 = cnx::designBiquad(EqType::LowCut, rate, 2000, 0, 0.5412);
    const BiquadCoefs h2 = cnx::designBiquad(EqType::LowCut, rate, 2000, 0, 1.3066);
    double z[4] = {0, 0, 0, 0};
    auto hp = [&](double x) {
        double y = h1.b0 * x + z[0];
        z[0] = h1.b1 * x - h1.a1 * y + z[1];
        z[1] = h1.b2 * x - h1.a2 * y;
        const double y2 = h2.b0 * y + z[2];
        z[2] = h2.b1 * y - h2.a1 * y2 + z[3];
        z[3] = h2.b2 * y - h2.a2 * y2;
        return y2;
    };
    std::vector<float> buf(480);
    double worst = 0;
    double worstBy[6] = {0, 0, 0, 0, 0, 0};
    int n = 0;
    for (int block = 0; n < frames; ++block) {
        // alle 10 ms etwas ändern: Werte ziehen, Typ wechseln, Band an/aus, Pegel, ganzer Equalizer an/aus
        const int slot = slotDist(rng);
        const int what = coin(rng);
        switch (what) {
        case 0: s.bands[slot] = band(kAllTypes[typeDist(rng)], std::exp(logF(rng)), gain(rng), std::exp(logQ(rng))); break;
        case 1: s.bands[slot].type = kAllTypes[typeDist(rng)]; s.bands[slot].used = true; break;
        case 2: s.bands[slot].on = !s.bands[slot].on; break;
        case 3: s.bands[slot].used = !s.bands[slot].used; break;
        case 4: s.levelDb = gain(rng) * 0.5f; break;
        default: if (block % 25 == 0) s.enabled = !s.enabled; s.bands[slot].freqHz *= 1.05f; break;
        }
        e.setSettings(s);
        for (int i = 0; i < 480; ++i) buf[i] = 0.25f * static_cast<float>(std::sin(2 * kPi * 200 * (n + i) / rate));
        e.process(buf.data(), 480);
        for (int i = 0; i < 480; ++i, ++n) {
            const double y = hp(buf[i]);
            if (n > 4800) {
                worst = std::max(worst, std::fabs(y));
                worstBy[what] = std::max(worstBy[what], std::fabs(y));
            }
        }
    }
    std::printf("  Zufallsspruenge alle 10 ms, groesster Rest oberhalb 2 kHz: %.1f dBFS (Sinus -12 dBFS); je Art:", 20 * std::log10(std::max(worst, 1e-12)));
    for (double w : worstBy) std::printf(" %.1f", 20 * std::log10(std::max(w, 1e-12)));
    std::printf("\n");
    // Sprünge über sieben Oktaven alle 10 ms sind ein Härtetest; hier zählt, dass nichts springt.
    CHECK(worst < 0.01, "Umschalten erzeugt Knackser: Rest %.4f (%.1f dBFS)", worst, 20 * std::log10(worst));

    // Realistisch: ein Band mit der Maus ziehen (alle 16 ms ein neuer Wert) und dabei Typ und Güte ändern.
    EqSettings d;
    d.enabled = true;
    d.bands[0] = band(EqType::Peak, 80, 9, 4);
    Equalizer drag = make(rate, 1, d);
    for (double& v : z) v = 0;
    double dragWorst = 0;
    n = 0;
    const int dragFrames = static_cast<int>(rate * 6);
    int next = 0;
    while (n < dragFrames) {
        const double t = static_cast<double>(n) / rate;
        if (n >= next) {
            next += static_cast<int>(rate * 0.016);
            if (t < 2) { d.bands[0].freqHz = static_cast<float>(80 * std::pow(2.0, 6 * t / 2)); }          // 80 Hz → 5 kHz
            else if (t < 3) { d.bands[0].gainDb = static_cast<float>(9 - 21 * (t - 2)); }                 // +9 → −12 dB
            else if (t < 4) { d.bands[0].q = static_cast<float>(4 * std::pow(0.05, t - 3)); }             // Güte 4 → 0,2
            else if (t < 4.02) { d.bands[0].type = EqType::HighShelf; }                                    // Typ wechseln
            else { d.bands[0].freqHz = static_cast<float>(5000 * std::pow(2.0, -5 * (t - 4) / 2)); }      // zurück nach unten
            drag.setSettings(d);
        }
        for (int i = 0; i < 480; ++i) buf[i] = 0.25f * static_cast<float>(std::sin(2 * kPi * 200 * (n + i) / rate));
        drag.process(buf.data(), 480);
        for (int i = 0; i < 480; ++i, ++n) {
            const double y = hp(buf[i]);
            if (n > 4800) dragWorst = std::max(dragWorst, std::fabs(y));
        }
    }
    std::printf("  Band ziehen: groesster Rest oberhalb 2 kHz: %.1f dBFS\n", 20 * std::log10(std::max(dragWorst, 1e-12)));
    CHECK(dragWorst < 0.0003, "Ziehen eines Bandes erzeugt Knackser: Rest %.5f (%.1f dBFS)", dragWorst, 20 * std::log10(dragWorst));
}

static void disableReturnsToBitExact() {
    std::printf("disableReturnsToBitExact\n");
    EqSettings s;
    s.enabled = true;
    s.bands[1] = band(EqType::Peak, 800, 8, 2);
    s.bands[4] = band(EqType::HighShelf, 5000, -6, 0.71f);
    s.levelDb = -8;
    Equalizer e = make(48000, 2, s);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> buf(480 * 2), ref;
    for (int b = 0; b < 50; ++b) { for (auto& v : buf) v = dist(rng); e.process(buf.data(), 480); }
    s.enabled = false;
    e.setSettings(s);
    for (int b = 0; b < 40; ++b) { for (auto& v : buf) v = dist(rng); e.process(buf.data(), 480); }  // 400 ms: Überblendung und Pegel
    CHECK(!e.active(), "nach dem Ausschalten noch aktiv");
    for (auto& v : buf) v = dist(rng);
    ref = buf;
    e.process(buf.data(), 480);
    CHECK(buf == ref, "nach dem Ausschalten nicht bitgenau");

    // Wieder einschalten: springt nicht, sondern blendet ein
    s.enabled = true;
    e.setSettings(s);
    for (auto& v : buf) v = 0.1f;
    e.process(buf.data(), 32);
    CHECK(std::fabs(buf[0] - 0.1f) < 0.01f, "Einschalten springt sofort (%.4f statt ~0,1)", buf[0]);
}

static void levelGlidesToTarget() {
    std::printf("levelGlidesToTarget\n");
    EqSettings s;
    s.enabled = true;
    s.levelDb = -6;
    Equalizer e = make(48000, 1, s);
    const double got = measureDb(e, 48000, 1000);
    CHECK(std::fabs(got + 6.0) < 0.01, "Pegel -6 dB: %.3f dB", got);
    s.levelDb = 0;
    e.setSettings(s);
    std::vector<float> buf(48000, 0.5f);
    e.process(buf.data(), 48000);
    CHECK(buf.back() == 0.5f && !e.active(), "Pegel 0 dB nicht exakt erreicht (%.7f)", buf.back());
}

static void stableAtExtremes() {
    std::printf("stableAtExtremes\n");
    const double rates[] = {8000, 44100, 48000, 96000, 192000};
    const float freqs[] = {1, 10, 20, 1000, 20000, 24000, 1e9f};
    const float qs[] = {0.0f, 0.05f, 0.71f, 20.0f, 1e6f};
    const float gains[] = {-100, -24, 0, 24, 100};
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> buf(480 * 2);
    int bad = 0;
    for (double rate : rates)
        for (EqType t : kAllTypes)
            for (float f : freqs)
                for (float q : qs)
                    for (float g : gains) {
                        EqSettings s;
                        s.enabled = true;
                        s.bands[0] = band(t, f, g, q);
                        Equalizer e = make(rate, 2, s);
                        float mx = 0;
                        for (int b = 0; b < 20; ++b) {
                            for (auto& v : buf) v = dist(rng);
                            e.process(buf.data(), 480);
                            for (float v : buf) mx = std::max(mx, std::fabs(v));
                        }
                        if (!std::isfinite(mx) || mx > 100.0f) {
                            if (++bad <= 5) std::printf("  instabil: %s f=%g Q=%g G=%g bei %.0f Hz: %g\n", kTypeNames[static_cast<int>(t)], f, q, g, rate, mx);
                        }
                    }
    CHECK(bad == 0, "%d Kombinationen instabil", bad);
    // Kaputte Einstellungen werden entschärft
    EqSettings s;
    s.enabled = true;
    s.bands[0] = band(static_cast<EqType>(77), std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1);
    s.levelDb = std::numeric_limits<float>::quiet_NaN();
    Equalizer e = make(48000, 2, s);
    for (auto& v : buf) v = dist(rng);
    e.process(buf.data(), 480);
    bool finite = true;
    for (float v : buf) finite = finite && std::isfinite(v);
    CHECK(finite, "kaputte Einstellungen erzeugen ungueltige Werte");
}

static void channelsStayIndependent() {
    std::printf("channelsStayIndependent\n");
    EqSettings s;
    s.enabled = true;
    s.bands[0] = band(EqType::Peak, 300, 10, 4);
    s.bands[1] = band(EqType::LowCut, 50, 0, 2);
    Equalizer e = make(48000, 3, s);
    std::vector<float> buf(480 * 3);
    bool silent = true;
    for (int b = 0; b < 100; ++b) {
        for (int i = 0; i < 480; ++i) {
            buf[3 * i] = 0.3f * static_cast<float>(std::sin(i * 0.05));
            buf[3 * i + 1] = 0.0f;
            buf[3 * i + 2] = 0.2f;
        }
        e.process(buf.data(), 480);
        for (int i = 0; i < 480; ++i) silent = silent && buf[3 * i + 1] == 0.0f;
    }
    CHECK(silent, "ein stiller Kanal bekommt Signal aus einem anderen");
    CHECK(std::fabs(buf[2]) < 1e-3f, "Gleichanteil hinter dem Hochpass nicht verschwunden (%.5f)", buf[2]);
}

static void speed() {
    const int rate = 48000, seconds = 120;
    EqSettings s;
    s.enabled = true;
    s.bands[0] = band(EqType::LowCut, 30, 0, 0.71f);
    s.bands[1] = band(EqType::LowShelf, 100, 3, 0.71f);
    s.bands[2] = band(EqType::Peak, 800, -2, 1.4f);
    s.bands[3] = band(EqType::Peak, 3000, 2, 2);
    s.bands[4] = band(EqType::HighShelf, 9000, -3, 0.71f);
    s.levelDb = -3;
    Equalizer e = make(rate, 2, s);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> buf(480 * 2);
    for (auto& v : buf) v = dist(rng);
    const auto t0 = std::chrono::steady_clock::now();
    volatile float sink = 0.0f;
    for (int i = 0; i < seconds * 100; ++i) {
        if (i % 3 == 0) { s.bands[2].freqHz = 800.0f + (i % 300); e.setSettings(s); }  // wie beim Ziehen mit der Maus
        e.process(buf.data(), 480);
        sink = sink + buf[0];
        for (auto& v : buf) v = v * 0.5f + 0.2f * ((i & 1) ? 1.0f : -1.0f);
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Tempo: 5 Baender, %d s Stereo/48 kHz in %.1f ms (%.0f-fache Echtzeit, %.3f %% eines Kerns)\n", seconds, sec * 1000.0, seconds / sec,
                100.0 * sec / seconds);
}

int main() {
    emptyAndDisabledAreBitExact();
    responseMatchesDesign();
    bandsAddUp();
    changesDoNotClick();
    disableReturnsToBitExact();
    levelGlidesToTarget();
    stableAtExtremes();
    channelsStayIndependent();
    speed();
    std::printf("%d Pruefungen, %d fehlgeschlagen\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
