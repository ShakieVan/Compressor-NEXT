// Gemeinsamer Zustand zwischen der Bedienoberfläche und dem Audio-Modul.
//
// Eine kleine Datei (%ProgramData%\CompressorNext\state.bin), die beide Seiten in den Speicher
// einblenden. Die Oberfläche schreibt die Einstellungen, das Audio-Modul liest sie bei jedem
// Audioblock (ohne Systemaufruf) und schreibt die Werte der Pegelanzeige und des Spektrums zurück.
// Aufbau identisch in src/CompressorNext/SharedState.cs.
//
// Ab 1.1 ist die Datei 256 KB groß (vorher 64 KB). Neue Felder liegen in vorher reservierten oder
// angehängten Bereichen und bedeuten mit dem Wert 0 die Voreinstellung. Eine Datei von 1.0 wird deshalb
// nur verlängert (von der Einrichtung bzw. der Oberfläche); Version und Einstellungen bleiben.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cnx {

constexpr uint32_t kStateMagic = 0x31584E43;  // "CNX1"
constexpr uint32_t kStateVersion = 1;
constexpr uint32_t kStateFileSize = 262144;
constexpr int kMaxExcluded = 32;
constexpr int kMeterSlots = 16;
constexpr wchar_t kStateFileTemplate[] = L"%ProgramData%\\CompressorNext\\state.bin";

// CLSID des Audio-Moduls
constexpr wchar_t kApoClsidString[] = L"{CE339205-A997-4182-B027-A9637647F8CA}";

#pragma pack(push, 4)
struct SharedSettings {          // 64 Byte ab Offset 0
    uint32_t magic;
    uint32_t version;
    uint32_t seq;                // ungerade, solange die Oberfläche schreibt
    uint32_t enabled;            // 0/1
    float    thresholdDb;        // -30 .. 0
    float    outputDb;           // -12 .. 0
    uint32_t excludedCount;      // Anzahl gültiger Einträge in SharedState::excluded
    uint32_t compressorOff;      // 1: nur der Kompressor ist aus (der Equalizer arbeitet weiter); "enabled" schaltet alles
    uint32_t reserved[8];
};

struct MeterSlot {               // 64 Byte
    uint64_t tick;               // GetTickCount64 der letzten Meldung, 0 = frei
    uint32_t owner;              // != 0, solange ein Audiostrom den Platz belegt
    uint32_t seq;                // ungerade, solange das Audio-Modul schreibt
    uint32_t endpointHash;       // Ausgabegerät (hashEndpoint)
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t flags;              // Bit 0: Maximierer arbeitet
    float    inL, inR;           // Spitzen vor dem Maximierer (linear)
    float    outL, outR;         // Spitzen danach (linear)
    float    minGain;            // stärkste Begrenzung im Zeitraum (1 = keine)
    uint32_t reserved[3];
};

// Spurprotokoll des Audio-Moduls: ein Ringpuffer der letzten Ereignisse (was Windows vom Modul verlangt
// hat und mit welchem Ergebnis). "CompressorNextSvc status" gibt ihn lesbar aus.
enum TraceEvent : uint32_t {
    kTraceGetClassObject = 1,   // hr
    kTraceCreate = 2,           // arg = 1: aggregiert; guid = verlangte Schnittstelle; hr
    kTraceUnknownInterface = 3, // guid = abgelehnte Schnittstelle
    kTraceInitialize = 4,       // arg = Größe der Initialisierungsdaten; arg2 = 1: nur Erkundung; guid = Modus; hr
    kTraceInputFormat = 5,      // arg = Kanäle, arg2 = Abtastrate (angefragt); hr
    kTraceOutputFormat = 6,     // dito
    kTraceLock = 7,             // arg = Kanäle, arg2 = Abtastrate; hr
    kTraceUnlock = 8,
    kTraceDestroy = 9,
    kTraceFault = 10,           // Fehler in der Verarbeitung abgefangen
};

struct TraceEntry {              // 48 Byte
    uint64_t tick;
    uint32_t pid;
    uint32_t event;
    uint32_t hr;
    uint32_t arg;
    uint8_t  guid[16];
    uint32_t endpointHash;
    uint32_t arg2;
};

constexpr int kTraceEntries = 1300;

// Equalizer hinter dem Maximierer. Feste Plätze: Ein Band behält seinen Platz (und seine Nummer), solange
// es besteht. So verwechselt das Audio-Modul beim Löschen eines Bandes keine Filter.
constexpr int kEqSlots = 8;

enum EqTypeCode : uint16_t {
    kEqPeak = 0,       // Glocke
    kEqLowShelf = 1,   // Tiefen-Kuhschwanz
    kEqHighShelf = 2,  // Höhen-Kuhschwanz
    kEqLowCut = 3,     // Hochpass, 12 dB/Oktave
    kEqHighCut = 4,    // Tiefpass, 12 dB/Oktave
    kEqNotch = 5,      // Kerbe
    kEqTypeCount = 6,
};

constexpr uint16_t kEqBandUsed = 1;    // Platz belegt
constexpr uint16_t kEqBandBypass = 2;  // Band vorübergehend aus

constexpr uint32_t kEqFlagOff = 1;          // Equalizer aus
constexpr uint32_t kEqFlagNoAutoLevel = 2;  // Auto-Pegel aus (nur für die Oberfläche; sie berechnet levelDb)

struct EqBandShared {            // 16 Byte
    uint16_t type;
    uint16_t flags;
    float    freqHz;
    float    gainDb;
    float    q;
};

struct EqShared {                // 256 Byte ab Offset 65536
    uint32_t flags;
    float    levelDb;            // Pegel hinter dem Equalizer (Auto-Pegel: minus die stärkste Anhebung der Kurve)
    uint32_t reserved0[6];
    EqBandShared bands[kEqSlots];
    uint32_t reserved[24];
};

// Spektrum des Ausgangs: Das Audio-Modul schreibt den Mittelwert aller Kanäle in einen Ringpuffer, die
// Oberfläche rechnet daraus die FFT. Es schreibt nur ein Audiostrom (owner) und nur, solange die
// Oberfläche danach fragt (uiTick höchstens kSpectrumWantMs alt); sonst kostet es nichts.
constexpr int kSpectrumRing = 32768;  // Zweierpotenz
constexpr uint64_t kSpectrumWantMs = 2000;
constexpr uint64_t kSpectrumStaleMs = 400;

struct SpectrumShared {          // ab Offset 65792
    uint64_t tick;               // GetTickCount64 der letzten Schreibung
    uint64_t uiTick;             // GetTickCount64, zu dem die Oberfläche zuletzt Daten wollte
    uint32_t owner;              // schreibender Audiostrom (0 = frei)
    uint32_t sampleRate;
    uint32_t writePos;           // Zahl aller geschriebenen Werte; Position = writePos % kSpectrumRing
    uint32_t endpointHash;
    uint32_t reserved[8];
    float    ring[kSpectrumRing];
};

struct SharedState {
    SharedSettings settings;             // 0 .. 63
    uint32_t excluded[kMaxExcluded];     // 64 .. 191: ausgenommene Ausgabegeräte (hashEndpoint)
    uint32_t apoInitCount;               // 192: wie oft Windows das Audio-Modul initialisiert hat
    uint32_t apoLockCount;               // 196: wie oft ein Audiostrom gestartet wurde
    uint64_t apoLastLockTick;            // 200: GetTickCount64 des letzten Stromstarts
    uint32_t reserved[12];               // 208 .. 255
    MeterSlot meters[kMeterSlots];       // 256 .. 1279
    uint32_t traceCount;                 // 1280: Zahl aller bisherigen Einträge (Position = traceCount % kTraceEntries)
    uint32_t traceReserved;
    TraceEntry trace[kTraceEntries];     // ab 1288
    uint8_t  pad[65536 - 1288 - kTraceEntries * 48];
    EqShared eq;                         // 65536 .. 65791
    SpectrumShared spectrum;             // ab 65792
};
#pragma pack(pop)

static_assert(sizeof(TraceEntry) == 48, "Layout");
static_assert(offsetof(SharedState, traceCount) == 1280, "Layout");

static_assert(sizeof(SharedSettings) == 64, "Layout");
static_assert(sizeof(MeterSlot) == 64, "Layout");
static_assert(offsetof(SharedState, excluded) == 64, "Layout");
static_assert(offsetof(SharedState, meters) == 256, "Layout");
static_assert(sizeof(EqBandShared) == 16, "Layout");
static_assert(sizeof(EqShared) == 256, "Layout");
static_assert(offsetof(SharedState, eq) == 65536, "Layout");
static_assert(offsetof(SharedState, spectrum) == 65792, "Layout");
static_assert(offsetof(SpectrumShared, ring) == 64, "Layout");
static_assert(sizeof(SharedState) <= kStateFileSize, "Layout");

constexpr uint32_t kMeterFlagEngaged = 1;

// FNV-1a über die Endpunkt-GUID in Kleinbuchstaben, z. B. "{f5e3dddb-a14a-404d-915f-850d7bc0d3d8}".
inline uint32_t hashEndpoint(const wchar_t* guid) {
    uint32_t h = 2166136261u;
    if (!guid) return 0;
    for (const wchar_t* p = guid; *p; ++p) {
        wchar_t c = *p;
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        h ^= static_cast<uint32_t>(c) & 0xFFu;
        h *= 16777619u;
    }
    return h == 0 ? 1 : h;
}

} // namespace cnx
