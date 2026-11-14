// Einbinden des Audio-Moduls in die Windows-Ausgabegeräte (Registry unter MMDevices\Audio\Render).
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace cnx {

struct EndpointStatus {
    std::wstring guid;
    std::wstring name;
    DWORD state = 0;          // 1 = aktiv (angeschlossen und eingeschaltet)
    bool hasFxKey = false;
    bool bound = false;
    bool enhancementsOff = false;
    std::wstring slot;        // wo das Modul steht bzw. hinkäme
    int variant = 0;          // gewählte Platz-Variante
    int variantCount = 0;
};

struct BindSummary {
    int total = 0;
    int alreadyBound = 0;
    int newlyBound = 0;
    int skipped = 0;
    int failed = 0;
    bool activeChanged = false;  // ein gerade aktives Gerät wurde neu eingebunden -> Audiodienst neu starten
    std::vector<std::wstring> log;
};

std::vector<EndpointStatus> listEndpoints();
std::wstring describeEndpoint(const std::wstring& guid);

// Bindet alle Wiedergabegeräte ein, die das Modul noch nicht haben. dryRun: nur berichten.
BindSummary bindAll(bool dryRun);

// Entfernt das Modul überall und stellt die ursprünglichen Einträge wieder her.
BindSummary unbindAll();

// Bindet ein Gerät mit einer bestimmten Platz-Variante neu ein (vorher aushängen) und merkt sie sich.
// Liefert false, wenn es die Variante nicht gibt oder das Schreiben scheitert.
bool rebindWithVariant(const std::wstring& guid, int variant, std::wstring* slot);

// Nimmt das Modul aus einem einzelnen Gerät heraus (Ursprungswerte wiederherstellen).
bool unbindOne(const std::wstring& guid);

// Schaltet die Windows-Audioverbesserungen überall wieder ein, wo sie aus sind. Ohne sie lädt Windows
// keine Effekt-Module; nach wiederholten Ladefehlern schaltet Windows sie von sich aus ab.
int enableEnhancements();

// Prüft, ob Ton auf diesem Gerät wirklich durch das Modul läuft: spielt kurz Stille ab und wartet auf
// die Meldung des Moduls in der Zustandsdatei.
enum class VerifyResult { Works, NotLoaded, Unknown };
// whileRunning (optional) wird aufgerufen, solange der Teststrom noch läuft (für Untersuchungen).
VerifyResult verifyEndpoint(const std::wstring& guid, std::wstring* detail, void (*whileRunning)() = nullptr);

} // namespace cnx
