// Ruft das Audio-Modul so auf wie die Windows-Audio-Engine – ohne es im System zu registrieren.
// Aufruf: apo_host_test <Pfad zur CompressorNextApo.dll> [Zustandsdatei]
// Mit Zustandsdatei wird zusätzlich geprüft, dass Einstellungen ankommen und die Pegelanzeige gefüllt wird
// (dazu muss die Umgebungsvariable ProgramData auf einen Ordner zeigen, der CompressorNext\state.bin enthält).
#include <windows.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "SharedState.h"

static int g_failed = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failed; std::printf("  FEHLER Zeile %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const CLSID kClsid = {0xce339205, 0xa997, 0x4182, {0xb0, 0x27, 0xa9, 0x63, 0x76, 0x47, 0xf8, 0xca}};

// Hüllobjekt wie in der Audio-Engine: besitzt das Modul und reicht unbekannte Schnittstellen an dessen
// eigenes (nicht delegierendes) IUnknown weiter.
struct Outer final : IUnknown {
    LONG ref = 1;
    IUnknown* inner = nullptr;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        if (!inner) { *ppv = nullptr; return E_NOINTERFACE; }
        return inner->QueryInterface(riid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++ref); }
    STDMETHODIMP_(ULONG) Release() override { return static_cast<ULONG>(--ref); }
};

static IAudioMediaType* makeFormat(DWORD channels, float rate, DWORD mask, bool isFloat = true, DWORD bytes = 4) {
    UNCOMPRESSEDAUDIOFORMAT f{};
    f.guidFormatType = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    f.dwSamplesPerFrame = channels;
    f.dwBytesPerSampleContainer = bytes;
    f.dwValidBitsPerSample = bytes * 8;
    f.fFramesPerSecond = rate;
    f.dwChannelMask = mask;
    IAudioMediaType* mt = nullptr;
    if (FAILED(CreateAudioMediaTypeFromUncompressedAudioFormat(&f, &mt))) return nullptr;
    return mt;
}

static cnx::SharedState* mapState(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, cnx::kStateFileSize, nullptr);
    CloseHandle(file);
    if (!mapping) return nullptr;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, cnx::kStateFileSize);
    CloseHandle(mapping);
    return static_cast<cnx::SharedState*>(view);
}

static void writeSettings(cnx::SharedState* st, bool enabled, float threshold, float output, bool compressorOn = true) {
    st->settings.magic = cnx::kStateMagic;
    st->settings.version = cnx::kStateVersion;
    st->settings.seq += 1;
    st->settings.enabled = enabled ? 1 : 0;
    st->settings.thresholdDb = threshold;
    st->settings.outputDb = output;
    st->settings.excludedCount = 0;
    st->settings.compressorOff = compressorOn ? 0 : 1;
    st->settings.seq += 1;
}

// Equalizer mit einer Glocke (oder ohne Band, wenn gainDb == 0)
static void writeEq(cnx::SharedState* st, bool on, float levelDb, float freq, float gainDb, float q) {
    st->settings.seq += 1;
    st->eq.flags = on ? 0 : cnx::kEqFlagOff;
    st->eq.levelDb = levelDb;
    for (auto& b : st->eq.bands) b = cnx::EqBandShared{};
    if (gainDb != 0.0f) st->eq.bands[2] = cnx::EqBandShared{cnx::kEqPeak, cnx::kEqBandUsed, freq, gainDb, q};
    st->settings.seq += 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("Aufruf: apo_host_test <dll> [Zustandsdatei]\n"); return 2; }
    cnx::SharedState* st = nullptr;
    if (argc >= 3) {
        st = mapState(argv[2]);
        if (!st) { std::printf("Zustandsdatei nicht anlegbar\n"); return 2; }
        writeSettings(st, true, -20.0f, -1.0f);
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { std::printf("DLL nicht ladbar (%lu)\n", GetLastError()); return 2; }
    using GetClassObjectFn = HRESULT(__stdcall*)(REFCLSID, REFIID, LPVOID*);
    using CanUnloadFn = HRESULT(__stdcall*)();
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    auto canUnload = reinterpret_cast<CanUnloadFn>(GetProcAddress(dll, "DllCanUnloadNow"));
    CHECK(getClassObject && canUnload, "Exporte fehlen");
    if (!getClassObject || !canUnload) return 1;

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(kClsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr), "DllGetClassObject 0x%08lX", hr);
    if (FAILED(hr)) return 1;

    // Aggregiert, wie es die Audio-Engine tut (Hüllobjekt CSystemEffectWrapper): Das Modul muss sich als
    // Teil eines fremden Objekts erzeugen lassen und alle Schnittstellen an dessen IUnknown binden.
    {
        Outer outer;
        void* wrong = nullptr;
        hr = factory->CreateInstance(&outer, __uuidof(IAudioProcessingObject), &wrong);
        CHECK(FAILED(hr) && !wrong, "aggregiert mit anderer Schnittstelle als IUnknown muss scheitern: 0x%08lX", hr);
        hr = factory->CreateInstance(&outer, IID_IUnknown, reinterpret_cast<void**>(&outer.inner));
        CHECK(hr == S_OK && outer.inner, "aggregiertes Erzeugen: 0x%08lX", hr);
        if (FAILED(hr) || !outer.inner) return 1;
        CHECK(outer.ref == 1, "Erzeugen darf das Huellobjekt nicht festhalten (Zaehler %ld)", outer.ref);

        IAudioProcessingObject* a = nullptr;
        IAudioProcessingObjectRT* r = nullptr;
        IAudioSystemEffects* f = nullptr;
        CHECK(outer.QueryInterface(__uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&a)) == S_OK && a, "aggregiert: IAudioProcessingObject fehlt");
        if (!a) return 1;
        CHECK(outer.ref == 2, "Schnittstelle muss das Huellobjekt zaehlen (Zaehler %ld)", outer.ref);
        CHECK(a->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&r)) == S_OK && r, "aggregiert: IAudioProcessingObjectRT fehlt");
        CHECK(a->QueryInterface(__uuidof(IAudioSystemEffects), reinterpret_cast<void**>(&f)) == S_OK && f, "aggregiert: IAudioSystemEffects fehlt");
        IUnknown* identity = nullptr;
        CHECK(a->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity)) == S_OK && identity == static_cast<IUnknown*>(&outer),
              "IUnknown einer Schnittstelle muss das Huellobjekt sein");
        if (identity) identity->Release();
        void* none = nullptr;
        CHECK(a->QueryInterface(IID_IClassFactory, &none) == E_NOINTERFACE && !none, "unbekannte Schnittstelle muss abgelehnt werden");

        APOInitSystemEffects2 aggInit{};
        aggInit.APOInit.cbSize = sizeof(aggInit);
        aggInit.APOInit.clsid = kClsid;
        CHECK(a->Initialize(sizeof(aggInit), reinterpret_cast<BYTE*>(&aggInit)) == S_OK, "aggregiert: Initialize");
        if (f) f->Release();
        if (r) r->Release();
        a->Release();
        CHECK(outer.ref == 1, "nach dem Freigeben der Schnittstellen: Zaehler %ld statt 1", outer.ref);
        CHECK(canUnload() == S_FALSE, "Modul lebt noch, DLL meldet sich aber entladbar");
        outer.inner->Release();
        outer.inner = nullptr;
        CHECK(canUnload() == S_OK, "aggregiertes Modul wurde nicht zerstoert");
        std::printf("Aggregiert erzeugt (wie durch die Audio-Engine): in Ordnung\n");
    }

    IAudioProcessingObject* apo = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo));
    CHECK(SUCCEEDED(hr), "CreateInstance 0x%08lX", hr);
    if (FAILED(hr)) return 1;

    IAudioProcessingObjectRT* rt = nullptr;
    IAudioProcessingObjectConfiguration* cfg = nullptr;
    IAudioSystemEffects2* fx2 = nullptr;
    IAudioSystemEffects* fx = nullptr;
    CHECK(SUCCEEDED(apo->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&rt))), "IAudioProcessingObjectRT fehlt");
    CHECK(SUCCEEDED(apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), reinterpret_cast<void**>(&cfg))), "IAudioProcessingObjectConfiguration fehlt");
    CHECK(SUCCEEDED(apo->QueryInterface(__uuidof(IAudioSystemEffects2), reinterpret_cast<void**>(&fx2))), "IAudioSystemEffects2 fehlt");
    CHECK(SUCCEEDED(apo->QueryInterface(__uuidof(IAudioSystemEffects), reinterpret_cast<void**>(&fx))), "IAudioSystemEffects fehlt");
    if (!rt || !cfg || !fx2 || !fx) return 1;

    APO_REG_PROPERTIES* reg = nullptr;
    hr = apo->GetRegistrationProperties(&reg);
    CHECK(SUCCEEDED(hr) && reg && reg->clsid == kClsid && (reg->Flags & APO_FLAG_INPLACE), "Registrierungsangaben");
    if (reg) { std::printf("APO: %ls, Flags 0x%X\n", reg->szFriendlyName, reg->Flags); CoTaskMemFree(reg); }

    // Initialisierung wie durch die Audio-Engine (ohne Eigenschaftsspeicher)
    APOInitSystemEffects2 init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = kClsid;
    init.AudioProcessingMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    hr = apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
    CHECK(SUCCEEDED(hr), "Initialize 0x%08lX", hr);
    CHECK(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)) == APOERR_ALREADY_INITIALIZED, "doppeltes Initialize nicht abgewiesen");

    // Formatverhandlung: Gleitkomma wird angenommen, 16-Bit-Ganzzahl bekommt einen Gegenvorschlag
    IAudioMediaType* fmt = makeFormat(2, 48000.0f, KSAUDIO_SPEAKER_STEREO);
    IAudioMediaType* supported = nullptr;
    hr = apo->IsInputFormatSupported(nullptr, fmt, &supported);
    CHECK(hr == S_OK, "Gleitkomma-Eingang: 0x%08lX", hr);
    if (supported) { supported->Release(); supported = nullptr; }
    hr = apo->IsOutputFormatSupported(fmt, fmt, &supported);
    CHECK(hr == S_OK, "Gleitkomma-Ausgang: 0x%08lX", hr);
    if (supported) { supported->Release(); supported = nullptr; }
    IAudioMediaType* pcm16 = makeFormat(2, 48000.0f, KSAUDIO_SPEAKER_STEREO, false, 2);
    hr = apo->IsInputFormatSupported(nullptr, pcm16, &supported);
    CHECK(hr == S_FALSE && supported, "16-Bit-Eingang sollte einen Gegenvorschlag liefern: 0x%08lX", hr);
    if (supported) { supported->Release(); supported = nullptr; }
    pcm16->Release();

    UINT effects = 0;
    GUID* list = nullptr;
    hr = fx2->GetEffectsList(&list, &effects, nullptr);
    CHECK(SUCCEEDED(hr) && effects == (st ? 1u : 0u), "GetEffectsList: 0x%08lX, %u Effekte", hr, effects);
    if (list) CoTaskMemFree(list);

    // Verarbeitung: 48 kHz Stereo in 10-ms-Blöcken, Ein- und Ausgang im selben Puffer
    const UINT32 frames = 480;
    std::vector<float> buffer(frames * 2);
    APO_CONNECTION_DESCRIPTOR inDesc{}, outDesc{};
    inDesc.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    inDesc.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
    inDesc.u32MaxFrameCount = frames;
    inDesc.pFormat = fmt;
    inDesc.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    outDesc = inDesc;
    APO_CONNECTION_DESCRIPTOR* inDescs[] = {&inDesc};
    APO_CONNECTION_DESCRIPTOR* outDescs[] = {&outDesc};
    hr = cfg->LockForProcess(1, inDescs, 1, outDescs);
    CHECK(SUCCEEDED(hr), "LockForProcess 0x%08lX", hr);
    if (FAILED(hr)) return 1;

    HNSTIME latency = 0;
    apo->GetLatency(&latency);
    CHECK(latency > 14000 && latency < 15100, "Latenz %lld (100-ns-Einheiten)", latency);
    std::printf("Latenz: %.2f ms\n", latency / 10000.0);
    CHECK(canUnload() == S_FALSE, "DLL meldet sich entladbar, obwohl ein Strom laeuft");

    APO_CONNECTION_PROPERTY inProp{}, outProp{};
    inProp.pBuffer = outProp.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
    inProp.u32Signature = outProp.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    APO_CONNECTION_PROPERTY* inProps[] = {&inProp};
    APO_CONNECTION_PROPERTY* outProps[] = {&outProp};

    const double pi = 3.14159265358979323846;
    auto runBlocks = [&](int blocks, float amplitude, float* maxOut) {
        static int n = 0;
        float mx = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            for (UINT32 i = 0; i < frames; ++i, ++n) {
                const float v = amplitude * static_cast<float>(std::sin(2 * pi * 440.0 * n / 48000.0));
                buffer[2 * i] = v;
                buffer[2 * i + 1] = v;
            }
            inProp.u32ValidFrameCount = frames;
            inProp.u32BufferFlags = BUFFER_VALID;
            outProp.u32ValidFrameCount = 0;
            outProp.u32BufferFlags = BUFFER_INVALID;
            rt->APOProcess(1, inProps, 1, outProps);
            if (outProp.u32BufferFlags != BUFFER_VALID || outProp.u32ValidFrameCount != frames) { ++g_failed; std::printf("  FEHLER: Ausgabepuffer nicht gueltig\n"); return; }
            if (b >= blocks / 2) for (float v : buffer) mx = std::max(mx, std::fabs(v));
        }
        *maxOut = mx;
    };

    float mx = 0.0f;
    if (!st) {
        // Ohne Zustandsdatei: unverändert (nur verzögert)
        runBlocks(100, 0.05f, &mx);
        CHECK(std::fabs(mx - 0.05f) < 1e-4f, "ohne Zustandsdatei: Pegel %.5f statt 0,05", mx);
        std::printf("Ohne Zustandsdatei: reicht durch (Pegel %.4f)\n", mx);
    } else {
        // Leise (-26 dBFS) mit Schwelle -20 / Ausgang -1: 19 dB lauter
        runBlocks(100, 0.05f, &mx);
        const float expected = 0.05f * std::pow(10.0f, 19.0f / 20.0f);
        CHECK(std::fabs(mx - expected) < 0.01f, "leise: Pegel %.4f statt %.4f", mx, expected);
        std::printf("Eingeschaltet, leise Stelle: %.4f -> %.4f (+%.1f dB)\n", 0.05f, mx, 20 * std::log10(mx / 0.05f));
        // Laut (0 dBFS): an der Obergrenze -1 dBFS
        runBlocks(100, 1.0f, &mx);
        const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
        CHECK(mx <= ceiling + 1e-6f && mx > ceiling - 0.01f, "laut: Pegel %.4f statt %.4f", mx, ceiling);
        std::printf("Eingeschaltet, laute Stelle: 1.0000 -> %.4f (Obergrenze %.4f)\n", mx, ceiling);

        // Pegelanzeige: ein belegter Platz mit frischen Werten
        int used = 0;
        for (int i = 0; i < cnx::kMeterSlots; ++i) {
            const cnx::MeterSlot& s = st->meters[i];
            if (s.owner == 0) continue;
            ++used;
            CHECK(s.sampleRate == 48000 && s.channels == 2, "Pegelplatz: %u Hz, %u Kanaele", s.sampleRate, s.channels);
            CHECK(GetTickCount64() - s.tick < 1000, "Pegelplatz veraltet");
            CHECK(s.inL > 0.9f && s.outL <= ceiling + 1e-6f && s.minGain < 0.2f && (s.flags & cnx::kMeterFlagEngaged), "Pegelwerte unplausibel (in %.3f, out %.3f, gain %.3f)", s.inL, s.outL, s.minGain);
            std::printf("Pegelanzeige: Eingang %.1f dBFS, Ausgang %.1f dBFS, Reduktion %.1f dB\n", 20 * std::log10(s.inL), 20 * std::log10(s.outL), 20 * std::log10(s.minGain));
        }
        CHECK(used == 1, "%d belegte Pegelplaetze statt 1", used);

        // Equalizer hinter dem Maximierer: Eine Glocke +6 dB bei 440 Hz hebt die begrenzte Spitze über die Obergrenze.
        st->spectrum.uiTick = GetTickCount64();  // die Oberfläche fragt nach dem Spektrum
        const uint32_t posBefore = st->spectrum.writePos;
        writeEq(st, true, 0.0f, 440.0f, 6.0f, 1.0f);
        runBlocks(100, 1.0f, &mx);
        const float boosted = ceiling * std::pow(10.0f, 6.0f / 20.0f);
        CHECK(std::fabs(mx - boosted) < 0.01f, "Equalizer +6 dB: Pegel %.4f statt %.4f", mx, boosted);
        std::printf("Equalizer hinter dem Maximierer, +6 dB bei 440 Hz: %.4f (Obergrenze %.4f)\n", mx, ceiling);
        for (int i = 0; i < cnx::kMeterSlots; ++i)
            if (st->meters[i].owner != 0)
                CHECK(std::fabs(st->meters[i].outL - boosted) < 0.02f, "Pegelanzeige zeigt den Ausgang vor dem Equalizer (%.3f)", st->meters[i].outL);

        // Spektrum: Dieser Strom schreibt den Ausgang (bei L = R genau den Abtastwert) in den Ringpuffer.
        const cnx::SpectrumShared& sp = st->spectrum;
        CHECK(sp.owner != 0 && sp.sampleRate == 48000 && sp.writePos - posBefore == 100 * frames, "Spektrum: Besitzer %u, %u Hz, %u neue Werte",
              sp.owner, sp.sampleRate, sp.writePos - posBefore);
        const uint32_t last = (sp.writePos - 1) & (cnx::kSpectrumRing - 1);
        CHECK(sp.ring[last] == buffer[2 * (frames - 1)], "Spektrum: letzter Wert %.5f statt %.5f", sp.ring[last], buffer[2 * (frames - 1)]);
        std::printf("Spektrum: %u Werte geschrieben\n", sp.writePos - posBefore);

        // Auto-Pegel (die Oberfläche schreibt −6 dB): wieder an der Obergrenze
        writeEq(st, true, -6.0f, 440.0f, 6.0f, 1.0f);
        runBlocks(100, 1.0f, &mx);
        CHECK(std::fabs(mx - ceiling) < 0.01f, "Auto-Pegel: Pegel %.4f statt %.4f", mx, ceiling);

        // Fragt die Oberfläche nicht mehr, schreibt das Modul nicht mehr.
        st->spectrum.uiTick = GetTickCount64() - 10000;
        const uint32_t posIdle = st->spectrum.writePos;
        runBlocks(10, 1.0f, &mx);
        CHECK(st->spectrum.writePos == posIdle, "Spektrum wird geschrieben, obwohl niemand fragt");

        // Equalizer aus: wieder nur der Maximierer
        writeEq(st, false, -6.0f, 440.0f, 6.0f, 1.0f);
        runBlocks(100, 1.0f, &mx);
        CHECK(std::fabs(mx - ceiling) < 0.005f && mx <= ceiling + 1e-6f, "Equalizer aus: Pegel %.4f statt %.4f", mx, ceiling);

        // Nur der Kompressor aus, Equalizer an: leise Stelle unverändert, +6 dB durch den Equalizer
        writeSettings(st, true, -20.0f, -1.0f, false);
        writeEq(st, true, 0.0f, 440.0f, 6.0f, 1.0f);
        runBlocks(100, 0.05f, &mx);
        CHECK(std::fabs(mx - 0.05f * 1.9953f) < 1e-3f, "nur Equalizer: Pegel %.5f statt %.5f", mx, 0.05f * 1.9953f);
        std::printf("Kompressor aus, Equalizer an: %.4f -> %.4f\n", 0.05f, mx);
        writeEq(st, true, 0.0f, 0.0f, 0.0f, 1.0f);

        // Ausschalten im laufenden Betrieb: nach der Überblendung wieder Originalpegel
        writeSettings(st, false, -20.0f, -1.0f);
        runBlocks(100, 0.05f, &mx);
        CHECK(std::fabs(mx - 0.05f) < 1e-4f, "ausgeschaltet: Pegel %.5f statt 0,05", mx);
        std::printf("Ausgeschaltet: %.4f (unveraendert)\n", mx);
    }

    // Vorführmodus (4. Argument = Sekunden): in Echtzeit ein musikähnliches, an- und abschwellendes Signal
    // verarbeiten (Bassdrum, Bass, Akkord, Becken), damit sich Pegelanzeige und Spektrum der Oberfläche ansehen
    // lassen. Die Einstellungen setzt dabei die Oberfläche.
    if (st && argc >= 4) {
        const int seconds = _wtoi(argv[3]);
        int n = 0;
        uint32_t noise = 12345;
        float hatPrev = 0.0f;
        const double bassNotes[] = {55.0, 55.0, 73.42, 82.41, 65.41, 65.41, 49.0, 61.74};
        const double chords[2][4] = {{220.0, 261.63, 329.63, 440.0}, {196.0, 246.94, 293.66, 392.0}};
        for (int b = 0; b < seconds * 100; ++b) {
            for (UINT32 i = 0; i < frames; ++i, ++n) {
                const double t = n / 48000.0;
                const double beat = std::fmod(t, 0.5);           // 120 Schläge pro Minute
                const int bar = static_cast<int>(t / 2.0);
                const double kick = 0.55 * std::exp(-beat / 0.12) * std::sin(2 * pi * (48.0 * beat + 60.0 * 0.03 * (1 - std::exp(-beat / 0.03))));
                const double fb = bassNotes[static_cast<int>(t / 0.5) % 8];
                double bass = 0;
                for (int h = 1; h <= 6; ++h) bass += std::sin(2 * pi * fb * h * t) / h;
                bass *= 0.12 * (0.4 + 0.6 * std::exp(-beat / 0.3));
                double pad = 0;
                for (double f : chords[bar % 2])
                    for (int h = 1; h <= 4; ++h) pad += std::sin(2 * pi * f * h * t + h) / (h * h);
                pad *= 0.035;
                noise = noise * 1664525u + 1013904223u;
                const float white = static_cast<float>((noise >> 8) & 0xFFFF) / 32768.0f - 1.0f;
                const double hatBeat = std::fmod(t + 0.25, 0.5);
                const float hat = static_cast<float>(0.25 * std::exp(-hatBeat / 0.035)) * (white - hatPrev);
                hatPrev = white;
                const double swell = 0.15 + 0.85 * std::pow(0.5 + 0.5 * std::sin(2 * pi * 0.08 * t), 2.0);
                const double mix = swell * (kick + bass + pad);
                buffer[2 * i] = static_cast<float>(mix + hat);
                buffer[2 * i + 1] = static_cast<float>(mix * 0.95 + 0.8 * hat);
            }
            inProp.u32ValidFrameCount = frames;
            inProp.u32BufferFlags = BUFFER_VALID;
            rt->APOProcess(1, inProps, 1, outProps);
            Sleep(10);
        }
    }

    // Stille: erst läuft die Verzögerungsleitung leer, dann meldet das Modul Stille
    for (int b = 0; b < 3; ++b) {
        inProp.u32ValidFrameCount = frames;
        inProp.u32BufferFlags = BUFFER_SILENT;
        rt->APOProcess(1, inProps, 1, outProps);
    }
    CHECK(outProp.u32BufferFlags == BUFFER_SILENT && outProp.u32ValidFrameCount == frames, "Stille wird nicht als Stille weitergegeben");

    hr = cfg->UnlockForProcess();
    CHECK(SUCCEEDED(hr), "UnlockForProcess 0x%08lX", hr);
    if (st) {
        int used = 0;
        for (int i = 0; i < cnx::kMeterSlots; ++i) if (st->meters[i].owner != 0) ++used;
        CHECK(used == 0, "Pegelplatz nach dem Ende nicht freigegeben");
        CHECK(st->spectrum.owner == 0, "Spektrum nach dem Ende nicht freigegeben");
    }

    fmt->Release();
    fx->Release(); fx2->Release(); cfg->Release(); rt->Release(); apo->Release(); factory->Release();
    CHECK(canUnload() == S_OK, "DLL nach dem Freigeben nicht entladbar");
    FreeLibrary(dll);
    CoUninitialize();
    std::printf(g_failed == 0 ? "APO-Test bestanden\n" : "APO-Test: %d Fehler\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
