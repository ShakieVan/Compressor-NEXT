// Compressor NEXT – Hilfsprogramm und Hintergrunddienst.
//
//   CompressorNextSvc status      zeigt alle Ausgabegeräte und was "install" täte (ohne Adminrechte)
//   CompressorNextSvc install     richtet alles ein (Setup; Adminrechte)
//   CompressorNextSvc repair      bindet fehlende Geräte ein und startet den Windows-Audiodienst neu (Adminrechte)
//   CompressorNextSvc uninstall   macht alles rückgängig (Adminrechte)
//   CompressorNextSvc service     läuft als Windows-Dienst: bindet neue Ausgabegeräte automatisch ein
//
// Der Dienst nimmt keine Eingaben entgegen. Er beobachtet nur die Geräteliste von Windows.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <objbase.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Binding.h"
#include "SharedState.h"

namespace {

const wchar_t kServiceName[] = L"CompressorNext";
const wchar_t kServiceDisplay[] = L"Compressor NEXT";
const wchar_t kServiceDescription[] = L"Bindet den Lautheits-Maximierer Compressor NEXT automatisch in neue Ausgabegeräte ein.";
const wchar_t kOurKey[] = L"SOFTWARE\\ShakieVan\\CompressorNext";
const wchar_t kAudioKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio";
const wchar_t kProtectedValue[] = L"DisableProtectedAudioDG";
const wchar_t kRenderKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

// Zugriff auf den Datenordner: System/Administratoren voll; Benutzer, der Audiodienst (LocalService) und
// App-Container (in einem solchen kann audiodg laufen) dürfen lesen und schreiben. Die Kennzeichnung
// "niedrige Integrität" erlaubt das Schreiben auch aus einem App-Container.
// Den Ordner selbst darf außer System/Administratoren niemand löschen oder ersetzen (kein Umleiten auf
// fremde Orte); Benutzer dürfen darin Dateien anlegen, der Audiodienst nur die Zustandsdatei lesen/schreiben.
const wchar_t kDirSddl[] = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;0x1201bf;;;BU)(A;;0x1200a9;;;LS)(A;;0x1200a9;;;AC)(A;;0x1200a9;;;S-1-15-2-2)S:(ML;OICI;NW;;;LW)";
const wchar_t kFileSddl[] = L"D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)(A;;0x1201bf;;;LS)(A;;0x1201bf;;;AC)(A;;0x1201bf;;;S-1-15-2-2)S:(ML;;NW;;;LW)";

std::wstring g_logFile;
bool g_console = true;
uint32_t g_traceLines = 60;  // "status" zeigt so viele Einträge des Spurprotokolls; "status all" alle

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void say(const std::wstring& text) {
    if (g_console) {
        std::printf("%s\n", utf8(text).c_str());
        std::fflush(stdout);
    }
    if (g_logFile.empty()) return;
    HANDLE f = CreateFileW(g_logFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 256 * 1024) {  // klein halten
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    sprintf_s(stamp, "%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string line = std::string(stamp) + utf8(text) + "\r\n";
    DWORD written = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(f);
}

std::wstring exeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring exePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring stateFile() {
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(cnx::kStateFileTemplate, path, MAX_PATH);
    return path;
}

std::wstring stateDir() {
    const std::wstring f = stateFile();
    return f.substr(0, f.find_last_of(L'\\'));
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = sizeof(e);
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Datenordner und Zustandsdatei
// ---------------------------------------------------------------------------------------------
bool applySddl(const std::wstring& path, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr, sacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted);
    std::vector<wchar_t> name(path.begin(), path.end());
    name.push_back(0);
    const DWORD rcDacl = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    const DWORD rcLabel = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
    LocalFree(sd);
    if (rcDacl != ERROR_SUCCESS) say(L"Zugriffsrechte nicht setzbar (" + std::to_wstring(rcDacl) + L"): " + path);
    if (rcLabel != ERROR_SUCCESS) say(L"Integritätskennzeichnung nicht setzbar (" + std::to_wstring(rcLabel) + L"): " + path);
    return rcDacl == ERROR_SUCCESS;
}

bool prepareState() {
    const std::wstring dir = stateDir(), file = stateFile();
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        say(L"Datenordner nicht anlegbar: " + dir);
        return false;
    }
    // Ordner und Datei müssen echt sein: Einer Verknüpfung auf einen fremden Ort folgen wir mit Adminrechten nicht.
    const DWORD dirAttr = GetFileAttributesW(dir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY) || (dirAttr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        say(L"Datenordner ist kein gewöhnlicher Ordner: " + dir);
        return false;
    }
    applySddl(dir, kDirSddl);
    const DWORD fileAttr = GetFileAttributesW(file.c_str());
    if (fileAttr != INVALID_FILE_ATTRIBUTES && (fileAttr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        say(L"Zustandsdatei ist keine gewöhnliche Datei: " + file);
        return false;
    }
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        say(L"Zustandsdatei nicht anlegbar: " + file);
        return false;
    }
    std::vector<BYTE> buf(cnx::kStateFileSize, 0);
    DWORD got = 0;
    ReadFile(f, buf.data(), cnx::kStateFileSize, &got, nullptr);
    auto* st = reinterpret_cast<cnx::SharedState*>(buf.data());
    LARGE_INTEGER fileSize{};
    GetFileSizeEx(f, &fileSize);
    const bool valid = got >= 256 && st->settings.magic == cnx::kStateMagic && st->settings.version == cnx::kStateVersion;
    if (valid && fileSize.QuadPart < cnx::kStateFileSize) {
        // Datei einer älteren Version: Einstellungen behalten, nur auf die neue Größe verlängern.
        SetFilePointer(f, static_cast<LONG>(cnx::kStateFileSize), nullptr, FILE_BEGIN);
        SetEndOfFile(f);
    }
    if (!valid) {
        // Neu anlegen mit Standardwerten: eingeschaltet, Schwelle -18 dB, Ausgang -1 dB.
        std::fill(buf.begin(), buf.end(), static_cast<BYTE>(0));
        st->settings.magic = cnx::kStateMagic;
        st->settings.version = cnx::kStateVersion;
        st->settings.seq = 2;
        st->settings.enabled = 1;
        st->settings.thresholdDb = -18.0f;
        st->settings.outputDb = -1.0f;
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(f, buf.data(), cnx::kStateFileSize, &written, nullptr);
        SetEndOfFile(f);
    }
    CloseHandle(f);
    applySddl(file, kFileSddl);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Audio-Modul registrieren
// ---------------------------------------------------------------------------------------------
bool callDll(const char* function) {
    const std::wstring dll = exeDir() + L"\\CompressorNextApo.dll";
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        say(L"Audio-Modul nicht ladbar (" + std::to_wstring(GetLastError()) + L"): " + dll);
        return false;
    }
    using Fn = HRESULT(__stdcall*)();
    const auto fn = reinterpret_cast<Fn>(GetProcAddress(m, function));
    const HRESULT hr = fn ? fn() : E_NOTIMPL;
    FreeLibrary(m);
    if (FAILED(hr)) {
        wchar_t code[16];
        swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
        say(std::wstring(L"Registrierung des Audio-Moduls fehlgeschlagen: ") + code);
    }
    return SUCCEEDED(hr);
}

// Unsignierte Audio-Module lädt Windows nur mit diesem Schalter (wie bei Equalizer APO).
void setProtectedAudioSwitch(bool install) {
    HKEY audio = nullptr, ours = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kAudioKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &audio, nullptr) != ERROR_SUCCESS) return;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kOurKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &ours, nullptr) != ERROR_SUCCESS) {
        RegCloseKey(audio);
        return;
    }
    DWORD value = 0, size = sizeof(value);
    const bool present = RegQueryValueExW(audio, kProtectedValue, nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS;
    if (install) {
        DWORD saved = 0, savedSize = sizeof(saved);
        const bool haveSaved = RegQueryValueExW(ours, L"PreviousProtectedAudioDG", nullptr, nullptr, reinterpret_cast<BYTE*>(&saved), &savedSize) == ERROR_SUCCESS;
        if (!haveSaved) {
            const DWORD previous = present ? value : 0xFFFFFFFFu;  // 0xFFFFFFFF = war nicht vorhanden
            RegSetValueExW(ours, L"PreviousProtectedAudioDG", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&previous), sizeof(previous));
        }
        const DWORD one = 1;
        RegSetValueExW(audio, kProtectedValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    } else {
        DWORD saved = 0, savedSize = sizeof(saved);
        if (RegQueryValueExW(ours, L"PreviousProtectedAudioDG", nullptr, nullptr, reinterpret_cast<BYTE*>(&saved), &savedSize) == ERROR_SUCCESS) {
            if (saved == 0xFFFFFFFFu) RegDeleteValueW(audio, kProtectedValue);
            else RegSetValueExW(audio, kProtectedValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&saved), sizeof(saved));
        }
    }
    RegCloseKey(ours);
    RegCloseKey(audio);
}

// ---------------------------------------------------------------------------------------------
// Dienste steuern
// ---------------------------------------------------------------------------------------------
bool waitForState(SC_HANDLE svc, DWORD wanted, DWORD timeoutMs) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    SERVICE_STATUS st{};
    while (QueryServiceStatus(svc, &st)) {
        if (st.dwCurrentState == wanted) return true;
        if (GetTickCount64() > end) return false;
        Sleep(200);
    }
    return false;
}

bool stopService(SC_HANDLE svc) {
    SERVICE_STATUS st{};
    if (!QueryServiceStatus(svc, &st)) return false;
    if (st.dwCurrentState == SERVICE_STOPPED) return true;
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    return waitForState(svc, SERVICE_STOPPED, 30000);
}

bool startService(SC_HANDLE svc) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) return waitForState(svc, SERVICE_RUNNING, 30000);
        Sleep(5000);
    }
    return false;
}

// Startet den Windows-Audiodienst neu, damit geänderte Effekt-Einträge wirken. Der Ton setzt kurz aus.
bool restartAudio() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const DWORD access = SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS;
    SC_HANDLE audio = OpenServiceW(scm, L"Audiosrv", access);
    if (!audio) {
        say(L"Windows-Audiodienst nicht steuerbar (" + std::to_wstring(GetLastError()) + L")");
        CloseServiceHandle(scm);
        return false;
    }
    // Abhängige Dienste zuerst anhalten, hinterher wieder starten.
    std::vector<std::wstring> dependents;
    DWORD needed = 0, count = 0;
    EnumDependentServicesW(audio, SERVICE_ACTIVE, nullptr, 0, &needed, &count);
    if (needed > 0) {
        std::vector<BYTE> buf(needed);
        if (EnumDependentServicesW(audio, SERVICE_ACTIVE, reinterpret_cast<LPENUM_SERVICE_STATUSW>(buf.data()), needed, &needed, &count)) {
            const auto* list = reinterpret_cast<LPENUM_SERVICE_STATUSW>(buf.data());
            for (DWORD i = 0; i < count; ++i) dependents.emplace_back(list[i].lpServiceName);
        }
    }
    for (const auto& name : dependents)
        if (SC_HANDLE dep = OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS)) { stopService(dep); CloseServiceHandle(dep); }
    const bool stopped = stopService(audio);
    const bool started = startService(audio);
    for (auto it = dependents.rbegin(); it != dependents.rend(); ++it)
        if (SC_HANDLE dep = OpenServiceW(scm, it->c_str(), SERVICE_START | SERVICE_QUERY_STATUS)) { startService(dep); CloseServiceHandle(dep); }
    CloseServiceHandle(audio);
    CloseServiceHandle(scm);
    say(stopped && started ? L"Windows-Audiodienst neu gestartet." : L"Windows-Audiodienst ließ sich nicht sauber neu starten – bitte Windows neu starten.");
    return stopped && started;
}

bool installService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    const std::wstring command = L"\"" + exePath() + L"\" service";
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                   SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc) {
            stopService(svc);
            ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kServiceDisplay);
        }
    }
    if (!svc) {
        say(L"Dienst nicht einrichtbar (" + std::to_wstring(GetLastError()) + L")");
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_DESCRIPTIONW desc{const_cast<wchar_t*>(kServiceDescription)};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION actions[] = {{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 300000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 86400;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    const bool ok = startService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

void removeService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)) {
        stopService(svc);
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

void report(const cnx::BindSummary& s) {
    for (const auto& line : s.log) say(line);
}

// Prüft bei allen aktiven Geräten, ob der Ton wirklich durch das Modul läuft. Wo nicht, wird der nächste
// Platz probiert und der Audiodienst neu gestartet – so lange, bis es überall arbeitet oder alle Plätze
// durch sind. Liefert die Zahl der Geräte, auf denen das Modul am Ende nicht arbeitet.
int verifyAndAdapt() {
    std::vector<std::wstring> givenUp;
    for (int round = 0; round < 8; ++round) {
        std::vector<cnx::EndpointStatus> failing;
        for (const auto& e : cnx::listEndpoints()) {
            if (e.state != 1 || !e.hasFxKey || !e.bound) continue;
            if (std::find(givenUp.begin(), givenUp.end(), e.guid) != givenUp.end()) continue;
            std::wstring detail;
            const cnx::VerifyResult r = cnx::verifyEndpoint(e.guid, &detail);
            if (r == cnx::VerifyResult::Works) say(L"Geprüft, arbeitet (" + e.slot + L"): " + e.name);
            else if (r == cnx::VerifyResult::Unknown) say(L"Nicht prüfbar (" + detail + L"): " + e.name);
            else {
                say(L"Windows lädt das Modul hier nicht (" + e.slot + L"): " + e.name);
                failing.push_back(e);
            }
        }
        if (failing.empty()) return static_cast<int>(givenUp.size());
        for (const auto& e : failing) {
            std::wstring slot;
            if (cnx::rebindWithVariant(e.guid, e.variant + 1, &slot)) {
                say(L"Nächster Versuch als " + slot + L": " + e.name);
            } else {
                cnx::rebindWithVariant(e.guid, 0, &slot);
                givenUp.push_back(e.guid);
                say(L"Kein Platz funktioniert auf diesem Gerät: " + e.name);
            }
        }
        restartAudio();
    }
    return static_cast<int>(givenUp.size());
}

std::wstring guidText(const uint8_t* bytes) {
    GUID g;
    memcpy(&g, bytes, sizeof(g));
    wchar_t text[64];
    StringFromGUID2(g, text, 64);
    return text;
}

// Gibt das Spurprotokoll des Audio-Moduls lesbar aus.
void printTrace(const cnx::SharedState* st) {
    static const wchar_t* const names[] = {L"?", L"Klassenfabrik angefragt", L"Modul erzeugt", L"Schnittstelle abgelehnt", L"Initialisiert",
                                           L"Eingangsformat geprüft", L"Ausgangsformat geprüft", L"Strom gestartet", L"Strom beendet", L"Modul zerstört", L"FEHLER abgefangen"};
    const uint32_t count = st->traceCount;
    const uint32_t stored = count < static_cast<uint32_t>(cnx::kTraceEntries) ? count : cnx::kTraceEntries;
    const uint32_t n = stored < g_traceLines ? stored : g_traceLines;
    say(L"Spurprotokoll des Audio-Moduls (" + std::to_wstring(count) + L" Einträge insgesamt, die letzten " + std::to_wstring(n) + L"):");
    const ULONGLONG now = GetTickCount64();
    std::wstring unknown;  // aufeinanderfolgende abgelehnte Schnittstellen in einer Zeile sammeln
    int unknownCount = 0;
    auto flushUnknown = [&]() {
        if (unknownCount > 0) say(L"      " + std::to_wstring(unknownCount) + L" unbekannte Schnittstellen angefragt:" + unknown);
        unknown.clear();
        unknownCount = 0;
    };
    for (uint32_t k = 0; k < n; ++k) {
        const cnx::TraceEntry& e = st->trace[(count - n + k) % cnx::kTraceEntries];
        if (e.event == cnx::kTraceUnknownInterface) {
            unknown += L" " + guidText(e.guid);
            ++unknownCount;
            continue;
        }
        flushUnknown();
        wchar_t line[320];
        swprintf_s(line, L"  vor %7.1f s  PID %-6u %-24s hr=0x%08X a=%u/%u b=%u Gerät=0x%08X %s",
                   (now - e.tick) / 1000.0, e.pid, e.event <= 10 ? names[e.event] : L"?", e.hr, e.arg & 0xFFFF, e.arg >> 16, e.arg2, e.endpointHash,
                   guidText(e.guid).c_str());
        say(line);
    }
    flushUnknown();
}

// ---------------------------------------------------------------------------------------------
// Befehle
// ---------------------------------------------------------------------------------------------
int cmdStatus() {
    int active = 0, bound = 0, total = 0;
    for (const auto& e : cnx::listEndpoints()) {
        ++total;
        if (e.bound) ++bound;
        if (e.state != 1) continue;
        ++active;
        std::wstring line = L"[aktiv] " + e.name + L": ";
        if (!e.hasFxKey) line += L"ohne Effekt-Einstellungen (wird übersprungen)";
        else if (e.bound) line += L"eingebunden als " + e.slot;
        else line += L"nicht eingebunden; käme als " + e.slot;
        if (e.enhancementsOff) line += L"; Windows-Audioverbesserungen sind AUS";
        say(line);
    }
    say(std::to_wstring(total) + L" Wiedergabegeräte bekannt, " + std::to_wstring(active) + L" aktiv, " + std::to_wstring(bound) + L" eingebunden.");
    // Zähler des Audio-Moduls: zeigen, ob Windows es lädt und ob Ton hindurchläuft.
    HANDLE f = CreateFileW(stateFile().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        say(L"Zustandsdatei fehlt: " + stateFile());
    } else {
        std::vector<BYTE> buf(cnx::kStateFileSize, 0);
        DWORD got = 0;
        ReadFile(f, buf.data(), cnx::kStateFileSize, &got, nullptr);
        CloseHandle(f);
        const auto* st = reinterpret_cast<const cnx::SharedState*>(buf.data());
        int streams = 0;
        const ULONGLONG now = GetTickCount64();
        for (const auto& slot : st->meters)
            if (slot.owner != 0 && slot.tick != 0 && now - slot.tick < 1000) ++streams;
        say(L"Audio-Modul: " + std::to_wstring(st->apoInitCount) + L"-mal von Windows geladen, " + std::to_wstring(st->apoLockCount) +
            L" Audioströme gestartet, " + std::to_wstring(streams) + L" laufen gerade.");
        if (st->apoInitCount == 0) say(L"  -> Windows hat das Modul noch nie geladen (oder es darf die Zustandsdatei nicht schreiben).");
        printTrace(st);
    }
    const cnx::BindSummary plan = cnx::bindAll(true);
    report(plan);
    say(L"Plan: " + std::to_wstring(plan.newlyBound) + L" einbinden, " + std::to_wstring(plan.alreadyBound) + L" schon eingebunden, " +
        std::to_wstring(plan.skipped) + L" übersprungen, " + std::to_wstring(plan.failed) + L" ohne Schreibzugriff.");
    return 0;
}

int needElevation() {
    say(L"Dieser Befehl braucht Administratorrechte.");
    return 5;
}

int cmdInstall() {
    if (!isElevated()) return needElevation();
    bool ok = prepareState();
    g_logFile = exeDir() + L"\\service.log";  // Programmordner: nur für Administratoren beschreibbar
    say(L"Installation beginnt.");
    ok = callDll("DllRegisterServer") && ok;
    setProtectedAudioSwitch(true);
    const cnx::BindSummary sum = cnx::bindAll(false);
    report(sum);
    say(std::to_wstring(sum.newlyBound) + L" Geräte eingebunden, " + std::to_wstring(sum.alreadyBound) + L" waren es schon, " + std::to_wstring(sum.failed) + L" fehlgeschlagen.");
    const int reenabled = cnx::enableEnhancements();
    if (reenabled > 0) say(L"Windows-Audioverbesserungen bei " + std::to_wstring(reenabled) + L" Gerät(en) wieder eingeschaltet.");
    ok = installService() && ok;
    restartAudio();
    const int broken = verifyAndAdapt();
    if (broken > 0) say(L"Auf " + std::to_wstring(broken) + L" aktiven Gerät(en) arbeitet das Modul nicht.");
    say(ok ? L"Installation abgeschlossen." : L"Installation mit Fehlern abgeschlossen.");
    return ok ? (broken > 0 ? 3 : 0) : 1;
}

const wchar_t kSfxValueName[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";

// Untersuchung: beschreibt jeden laufenden Audiodienst-Prozess (audiodg.exe) – wie er abgesichert ist und
// welche Effekt-DLLs er geladen hat. Braucht Adminrechte.
void describeAudiodg() {
    HANDLE self = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &self)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(self, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(self);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int found = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"audiodg.exe") != 0) continue;
        ++found;
        std::wstring line = L"   audiodg PID " + std::to_wstring(pe.th32ProcessID) + L": ";
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
        if (!h) {
            say(line + L"nicht zu öffnen (" + std::to_wstring(GetLastError()) + L")");
            continue;
        }
        HANDLE token = nullptr;
        if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
            DWORD value = 0, size = 0;
            if (GetTokenInformation(token, TokenIsAppContainer, &value, sizeof(value), &size)) line += L"AppContainer=" + std::to_wstring(value);
            value = 0;
            if (GetTokenInformation(token, static_cast<TOKEN_INFORMATION_CLASS>(46) /* TokenIsLessPrivilegedAppContainer */, &value, sizeof(value), &size)) line += L", LPAC=" + std::to_wstring(value);
            BYTE buf[128];
            if (GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf), &size)) {
                PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf)->Label.Sid;
                wchar_t rid[16];
                swprintf_s(rid, L"0x%lX", *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
                line += std::wstring(L", Integrität=") + rid;
            }
            CloseHandle(token);
        } else {
            line += L"Token nicht lesbar (" + std::to_wstring(GetLastError()) + L")";
        }
        HMODULE modules[2048];
        DWORD needed = 0;
        if (EnumProcessModulesEx(h, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
            bool ours = false;
            std::wstring dlls;
            const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 2048);
            for (DWORD i = 0; i < count; ++i) {
                wchar_t name[MAX_PATH] = {};
                if (!GetModuleBaseNameW(h, modules[i], name, MAX_PATH)) continue;
                std::wstring lower = name;
                for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
                if (lower == L"compressornextapo.dll") { ours = true; continue; }
                for (const wchar_t* key : {L"apo", L"wmalfx", L"sysfx", L"rtk", L"rltk", L"nahimic", L"dolby", L"dts"})
                    if (lower.find(key) != std::wstring::npos) { dlls += L" " + std::wstring(name); break; }
            }
            line += std::wstring(L", eigenes Modul geladen=") + (ours ? L"JA" : L"nein") + L", andere Effekt-DLLs:" + (dlls.empty() ? L" keine" : dlls);
        } else {
            line += L", Module nicht lesbar (" + std::to_wstring(GetLastError()) + L")";
        }
        say(line);
        CloseHandle(h);
    }
    CloseHandle(snap);
    if (found == 0) say(L"   kein audiodg-Prozess");
}

HKEY openFxKey(const std::wstring& guid) {
    HKEY key = nullptr;
    const std::wstring path = std::wstring(kRenderKey) + L"\\" + guid + L"\\FxProperties";
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS ? key : nullptr;
}

// Probe: stellt ein Gerät nach, dessen Treiber schon ein eigenes Stream-Modul mitbringt. Nur wenn der Platz frei ist.
bool setTestSfx(const std::wstring& guid, const std::wstring& clsid) {
    HKEY key = openFxKey(guid);
    if (!key) return false;
    bool ok = false;
    if (RegQueryValueExW(key, kSfxValueName, nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND)
        ok = RegSetValueExW(key, kSfxValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(clsid.c_str()), static_cast<DWORD>((clsid.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

void clearTestSfx(const std::wstring& guid, const std::wstring& clsid) {
    HKEY key = openFxKey(guid);
    if (!key) return;
    wchar_t value[80] = {};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    if (RegQueryValueExW(key, kSfxValueName, nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS && _wcsicmp(value, clsid.c_str()) == 0)
        RegDeleteValueW(key, kSfxValueName);
    RegCloseKey(key);
}

bool readMulti(HKEY key, const wchar_t* name, std::vector<std::wstring>* items) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_MULTI_SZ) return false;
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &bytes) != ERROR_SUCCESS) return false;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) items->emplace_back(p);
    return true;
}

bool writeMulti(HKEY key, const wchar_t* name, const std::vector<std::wstring>& items) {
    std::vector<wchar_t> data;
    for (const auto& s : items) { data.insert(data.end(), s.begin(), s.end()); data.push_back(0); }
    data.push_back(0);
    return RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// Hält unseren Dienst während einer Untersuchung an, damit er nicht dazwischenfunkt (er würde ein Gerät
// ohne unser Modul sofort wieder einbinden und den Audiodienst neu starten).
void controlOwnService(bool start) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS)) {
        if (start) startService(svc); else stopService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

// Sicherung der Herstellerliste während einer Untersuchung: Bricht sie mittendrin ab (Absturz, Stromausfall),
// stellt der nächste Lauf die Liste zuerst wieder her.
const wchar_t kProbeBackupKey[] = L"SOFTWARE\\ShakieVan\\CompressorNext\\ProbeBackup";

void saveProbeBackup(const std::wstring& guid, const std::vector<std::wstring>& items) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kProbeBackupKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    writeMulti(key, guid.c_str(), items);
    RegCloseKey(key);
}

bool takeProbeBackup(const std::wstring& guid, std::vector<std::wstring>* items) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProbeBackupKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    const bool found = readMulti(key, guid.c_str(), items) && !items->empty();
    RegDeleteValueW(key, guid.c_str());
    RegCloseKey(key);
    return found;
}

// Untersuchung auf Geräten, deren Treiber eine Liste von Stream-Modulen vorgibt (z. B. Realtek): Welche
// Kombinationen lädt Windows, und was hat der Audiodienst dabei geladen? simulate stellt auf Geräten ohne
// solche Liste eine nach (Microsoft-Proxy-Modul als "Hersteller").
void vendorListTests(const std::vector<cnx::EndpointStatus>& active, bool simulate) {
    const wchar_t* const sfxList = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
    const std::wstring ours = cnx::kApoClsidString;
    const std::wstring proxy = L"{889C03C8-ABAD-4004-BF0A-BC7BB825E166}";  // Microsoft Audio Proxy Discover Effects-APO
    struct Target {
        std::wstring guid, name;
        std::vector<std::wstring> original;
        bool simulated = false;
    };
    std::vector<Target> targets;
    for (const auto& e : active) {
        cnx::unbindOne(e.guid);  // Ausgangszustand des Treibers
        HKEY key = openFxKey(e.guid);
        if (!key) continue;
        Target t;
        t.guid = e.guid;
        t.name = e.name;
        std::vector<std::wstring> leftover;
        if (takeProbeBackup(e.guid, &leftover)) {
            writeMulti(key, sfxList, leftover);
            say(L"Herstellerliste aus einer abgebrochenen Untersuchung wiederhergestellt: " + e.name);
        }
        const bool has = readMulti(key, sfxList, &t.original) && !t.original.empty();
        if (!has && simulate && RegQueryValueExW(key, sfxList, nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND &&
            RegQueryValueExW(key, kSfxValueName, nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND) {
            t.original = {proxy};
            t.simulated = writeMulti(key, sfxList, t.original);
        }
        RegCloseKey(key);
        if (has || t.simulated) {
            std::wstring list;
            for (const auto& s : t.original) list += L" " + s;
            say(std::wstring(t.simulated ? L"Nachgestellte Herstellerliste:" : L"Herstellerliste:") + list + L" – " + t.name);
            if (!t.simulated) saveProbeBackup(t.guid, t.original);
            targets.push_back(t);
        }
    }
    if (targets.empty()) {
        say(L"Kein aktives Gerät mit einer Liste von Stream-Modulen.");
        return;
    }
    struct Test {
        const wchar_t* label;
        int kind;
    };
    const Test tests[] = {
        {L"Ausgangszustand, ohne eigenes Modul", 0},
        {L"Herstellerliste, eigenes hinten", 1},
        {L"eigenes vorn, Herstellerliste", 2},
        {L"[eigenes, eigenes] statt Herstellerliste", 3},
        {L"[eigenes, MS-Proxy] statt Herstellerliste", 4},
        {L"[eigenes] statt Herstellerliste", 5},
    };
    for (const Test& test : tests) {
        for (const Target& t : targets) {
            std::vector<std::wstring> items;
            switch (test.kind) {
                case 0: items = t.original; break;
                case 1: items = t.original; items.push_back(ours); break;
                case 2: items = t.original; items.insert(items.begin(), ours); break;
                case 3: items = {ours, ours}; break;
                case 4: items = {ours, proxy}; break;
                default: items = {ours}; break;
            }
            if (HKEY key = openFxKey(t.guid)) {
                writeMulti(key, sfxList, items);
                RegCloseKey(key);
            }
        }
        restartAudio();
        for (const Target& t : targets) {
            std::wstring detail;
            const cnx::VerifyResult r = cnx::verifyEndpoint(t.guid, &detail, describeAudiodg);
            say(std::wstring(L"Versuch \"") + test.label + L"\": " +
                (r == cnx::VerifyResult::Works ? L"ARBEITET" : r == cnx::VerifyResult::NotLoaded ? L"eigenes Modul meldet sich nicht" : L"nicht prüfbar (" + detail + L")") + L" – " + t.name);
        }
    }
    // Ausgangszustand des Treibers wiederherstellen.
    for (const Target& t : targets) {
        if (HKEY key = openFxKey(t.guid)) {
            if (t.simulated) RegDeleteValueW(key, sfxList); else writeMulti(key, sfxList, t.original);
            RegCloseKey(key);
        }
        std::vector<std::wstring> done;
        takeProbeBackup(t.guid, &done);  // Sicherung wird nicht mehr gebraucht
    }
}

// Probiert auf allen aktiven Geräten jeden Platz durch und berichtet, welche funktionieren (Entwicklung/Diagnose).
// Mit der CLSID eines fremden Stream-Moduls wird zusätzlich ein Treiber mit eigenem Modul nachgestellt.
int cmdProbe(const std::wstring& chainClsid) {
    if (!isElevated()) return needElevation();
    prepareState();
    g_logFile = exeDir() + L"\\service.log";  // Programmordner: nur für Administratoren beschreibbar
    say(L"Platz-Probe beginnt." + (chainClsid.empty() ? std::wstring() : L" Nachgestelltes Herstellermodul: " + chainClsid));
    controlOwnService(false);
    std::vector<cnx::EndpointStatus> active;
    for (const auto& e : cnx::listEndpoints())
        if (e.state == 1 && e.hasFxKey) active.push_back(e);
    // Ein Durchgang: jede Platz-Variante einbinden, Audiodienst neu starten, prüfen.
    auto runPass = [&](const std::vector<bool>& include, std::vector<int>* firstWorking) {
        for (int variant = 0; variant < 8; ++variant) {
            std::vector<size_t> tried;
            std::vector<std::wstring> slots;
            for (size_t i = 0; i < active.size(); ++i) {
                std::wstring slot;
                if (include[i] && cnx::rebindWithVariant(active[i].guid, variant, &slot)) { tried.push_back(i); slots.push_back(slot); }
            }
            if (tried.empty()) break;
            restartAudio();
            for (size_t k = 0; k < tried.size(); ++k) {
                std::wstring detail;
                const cnx::VerifyResult r = cnx::verifyEndpoint(active[tried[k]].guid, &detail);
                say(L"Platz " + std::to_wstring(variant) + L" (" + slots[k] + L"): " +
                    (r == cnx::VerifyResult::Works ? L"ARBEITET" : r == cnx::VerifyResult::NotLoaded ? L"wird nicht geladen" : L"nicht prüfbar (" + detail + L")") + L" – " + active[tried[k]].name);
                if (firstWorking && r == cnx::VerifyResult::Works && (*firstWorking)[tried[k]] < 0) (*firstWorking)[tried[k]] = variant;
            }
        }
    };

    std::vector<int> firstWorking(active.size(), -1);
    if (chainClsid == L"vendor" || chainClsid == L"vendorsim") {
        vendorListTests(active, chainClsid == L"vendorsim");
    } else if (chainClsid == L"lists") {
        // Untersuchung: verschiedene Listen-Kombinationen, um einzugrenzen, wann Windows das Modul in einer
        // Liste mit mehreren Einträgen lädt. Nur auf Geräten, deren Listen von Haus aus leer sind.
        const std::wstring ours = cnx::kApoClsidString;
        const std::wstring proxy = L"{889C03C8-ABAD-4004-BF0A-BC7BB825E166}";   // Microsoft Audio Proxy Discover Effects-APO
        const wchar_t* const sfxList = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
        const wchar_t* const mfxList = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14";
        const wchar_t* const efxList = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";
        const wchar_t* const mfxModes = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},6";
        const wchar_t* const efxModes = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";
        const std::wstring defaultMode = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
        const std::wstring msLfx = L"{C9453E73-8C5C-4463-9984-AF8BAB2F5447}";   // WM audio LFX APO (Microsoft-Effekte)
        struct TestValue {
            const wchar_t* name;
            bool multi;  // REG_MULTI_SZ (Liste) oder REG_SZ (einzelner Eintrag)
            std::vector<std::wstring> items;
        };
        struct ListTest {
            std::wstring label;
            std::vector<TestValue> values;
        };
        (void)mfxList; (void)mfxModes; (void)efxList; (void)efxModes; (void)defaultMode;
        const std::wstring missing = L"{5E1F0C1A-7B2D-4E3F-9A6B-0C1D2E3F4A5B}";  // absichtlich nirgends angemeldet
        const std::vector<ListTest> tests = {
            {L"Kontrolle: Stream-Liste [eigenes, MS-Proxy]", {{sfxList, true, {ours, proxy}}}},
            {L"Stream-Liste [eigenes, MS-Effekte]", {{sfxList, true, {ours, msLfx}}}},
            {L"Stream-Liste [eigenes, nicht vorhandenes Modul]", {{sfxList, true, {ours, missing}}}},
            {L"Stream-Liste [nicht vorhandenes Modul, eigenes]", {{sfxList, true, {missing, ours}}}},
        };
        for (size_t i = 0; i < active.size(); ++i) cnx::unbindOne(active[i].guid);
        for (const ListTest& test : tests) {
            std::vector<std::vector<const wchar_t*>> created(active.size());
            for (size_t i = 0; i < active.size(); ++i) {
                HKEY key = openFxKey(active[i].guid);
                if (!key) continue;
                for (const auto& value : test.values) {
                    if (RegQueryValueExW(key, value.name, nullptr, nullptr, nullptr, nullptr) != ERROR_FILE_NOT_FOUND) continue;  // belegt: nicht anfassen
                    std::vector<wchar_t> data;
                    for (const auto& s : value.items) { data.insert(data.end(), s.begin(), s.end()); data.push_back(0); }
                    if (value.multi) data.push_back(0);
                    if (RegSetValueExW(key, value.name, 0, value.multi ? REG_MULTI_SZ : REG_SZ, reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS)
                        created[i].push_back(value.name);
                }
                RegCloseKey(key);
            }
            restartAudio();
            for (size_t i = 0; i < active.size(); ++i) {
                if (created[i].empty()) continue;
                std::wstring detail;
                const cnx::VerifyResult r = cnx::verifyEndpoint(active[i].guid, &detail, describeAudiodg);
                say(L"Versuch \"" + test.label + L"\": " + (r == cnx::VerifyResult::Works ? L"ARBEITET" : r == cnx::VerifyResult::NotLoaded ? L"wird nicht geladen" : L"nicht prüfbar") + L" – " + active[i].name);
                if (HKEY key = openFxKey(active[i].guid)) {
                    for (const wchar_t* name : created[i]) RegDeleteValueW(key, name);
                    RegCloseKey(key);
                }
            }
        }
        for (auto& w : firstWorking) w = 0;
    } else if (chainClsid == L"self") {
        // Sonderfall: eine Stream-Liste, in der unser Modul zweimal steht. Zeigt, ob Windows Listen mit
        // mehreren Einträgen grundsätzlich mit unserem Modul lädt.
        const wchar_t listName[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
        std::wstring ours = cnx::kApoClsidString;
        std::vector<wchar_t> multi;
        for (int k = 0; k < 2; ++k) { multi.insert(multi.end(), ours.begin(), ours.end()); multi.push_back(0); }
        multi.push_back(0);
        std::vector<bool> written(active.size(), false);
        for (size_t i = 0; i < active.size(); ++i) {
            cnx::unbindOne(active[i].guid);
            if (HKEY key = openFxKey(active[i].guid)) {
                if (RegQueryValueExW(key, listName, nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND)
                    written[i] = RegSetValueExW(key, listName, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()), static_cast<DWORD>(multi.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
                RegCloseKey(key);
            }
        }
        restartAudio();
        for (size_t i = 0; i < active.size(); ++i) {
            if (!written[i]) continue;
            std::wstring detail;
            const cnx::VerifyResult r = cnx::verifyEndpoint(active[i].guid, &detail);
            say(std::wstring(L"Liste mit zwei eigenen Einträgen: ") + (r == cnx::VerifyResult::Works ? L"ARBEITET" : r == cnx::VerifyResult::NotLoaded ? L"wird nicht geladen" : L"nicht prüfbar") + L" – " + active[i].name);
            cnx::unbindOne(active[i].guid);  // entfernt beide Einträge wieder
        }
        for (auto& w : firstWorking) w = 0;
    } else {
        // 1. Durchgang: das Gerät, wie es ist.
        runPass(std::vector<bool>(active.size(), true), &firstWorking);
    }

    // 2. Durchgang: mit nachgestelltem Herstellermodul (nur wo der Stream-Platz von Haus aus frei ist).
    if (!chainClsid.empty() && chainClsid[0] == L'{') {
        std::vector<bool> chained(active.size(), false);
        for (size_t i = 0; i < active.size(); ++i) {
            cnx::unbindOne(active[i].guid);
            chained[i] = setTestSfx(active[i].guid, chainClsid);
            say(std::wstring(chained[i] ? L"Herstellermodul nachgestellt: " : L"Platz belegt, keine Nachstellung: ") + active[i].name);
        }
        runPass(chained, nullptr);
        for (size_t i = 0; i < active.size(); ++i) {
            if (!chained[i]) continue;
            cnx::unbindOne(active[i].guid);
            clearTestSfx(active[i].guid, chainClsid);
        }
    }

    for (size_t i = 0; i < active.size(); ++i) {
        std::wstring slot;
        // Ohne eigenes Ergebnis den Platz nehmen, der vor der Probe als funktionierend gemerkt war.
        if (!cnx::rebindWithVariant(active[i].guid, firstWorking[i] < 0 ? active[i].variant : firstWorking[i], &slot))
            cnx::rebindWithVariant(active[i].guid, 0, &slot);
        say(L"Gewählt: " + slot + L" – " + active[i].name);
    }
    cnx::bindAll(false);
    restartAudio();
    verifyAndAdapt();
    controlOwnService(true);
    return 0;
}

int cmdRepair() {
    if (!isElevated()) return needElevation();
    prepareState();
    g_logFile = exeDir() + L"\\service.log";  // Programmordner: nur für Administratoren beschreibbar
    callDll("DllRegisterServer");
    setProtectedAudioSwitch(true);
    const cnx::BindSummary sum = cnx::bindAll(false);
    report(sum);
    const int enabled = cnx::enableEnhancements();
    if (enabled > 0) say(L"Windows-Audioverbesserungen bei " + std::to_wstring(enabled) + L" Gerät(en) wieder eingeschaltet.");
    installService();
    restartAudio();
    const int broken = verifyAndAdapt();
    return sum.failed == 0 && broken == 0 ? 0 : 1;
}

// Vor einem Update: Dienst anhalten und das Modul überall aushängen, damit die Dateien ersetzbar sind.
// Einstellungen bleiben erhalten; "install" hängt danach wieder ein.
int cmdDetach() {
    if (!isElevated()) return needElevation();
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS)) {
            stopService(svc);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    report(cnx::unbindAll());
    restartAudio();
    return 0;
}

int cmdUninstall() {
    if (!isElevated()) return needElevation();
    g_logFile.clear();
    removeService();
    const cnx::BindSummary sum = cnx::unbindAll();
    report(sum);
    restartAudio();  // gibt die DLL frei, bevor das Setup die Dateien löscht
    callDll("DllUnregisterServer");
    setProtectedAudioSwitch(false);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kOurKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ShakieVan", KEY_WOW64_64KEY, 0);  // nur wenn leer
    const std::wstring dir = stateDir();
    DeleteFileW(stateFile().c_str());
    DeleteFileW((exeDir() + L"\\service.log").c_str());
    RemoveDirectoryW(dir.c_str());
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Dienst
// ---------------------------------------------------------------------------------------------
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;

void setServiceState(DWORD state) {
    SERVICE_STATUS st{};
    st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState = state;
    st.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    SetServiceStatus(g_statusHandle, &st);
}

DWORD WINAPI serviceControl(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        setServiceState(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
    return NO_ERROR;
}

// Bindet fehlende Geräte ein. Wurde ein gerade aktives Gerät neu eingebunden, muss der Audiodienst neu
// starten – höchstens dreimal pro Stunde, damit ein Treiber, der die Einträge ständig zurücksetzt, nicht
// zu einer Neustart-Schleife führt.
void ensureBound(std::vector<ULONGLONG>* restarts) {
    const cnx::BindSummary sum = cnx::bindAll(false);
    report(sum);
    if (!sum.activeChanged) return;
    const ULONGLONG now = GetTickCount64();
    std::vector<ULONGLONG> recent;
    for (ULONGLONG t : *restarts)
        if (now - t < 3600000) recent.push_back(t);
    *restarts = recent;
    if (restarts->size() >= 3) {
        say(L"Neues Gerät eingebunden, aber der Audiodienst wurde in der letzten Stunde schon dreimal neu gestartet. Wirksam nach dem nächsten Neustart.");
        return;
    }
    restarts->push_back(now);
    restartAudio();
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, serviceControl, nullptr);
    if (!g_statusHandle) return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    setServiceState(SERVICE_RUNNING);
    say(L"Dienst gestartet.");

    std::vector<ULONGLONG> restarts;
    HKEY render = nullptr;
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderKey, 0, KEY_NOTIFY | KEY_WOW64_64KEY, &render);
    ensureBound(&restarts);

    while (true) {
        if (render) RegNotifyChangeKeyValue(render, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
        HANDLE handles[] = {g_stopEvent, changed};
        // Ohne Benachrichtigung (Schlüssel nicht zu öffnen) alle zehn Minuten nachsehen.
        const DWORD rc = WaitForMultipleObjects(2, handles, FALSE, render ? INFINITE : 600000);
        if (rc == WAIT_OBJECT_0) break;
        // Änderungen kommen in Schüben (Gerät wird angelegt): kurz warten, bis Ruhe ist.
        if (WaitForSingleObject(g_stopEvent, 3000) == WAIT_OBJECT_0) break;
        ensureBound(&restarts);
    }
    if (render) RegCloseKey(render);
    CloseHandle(changed);
    say(L"Dienst beendet.");
    setServiceState(SERVICE_STOPPED);
}

int cmdService() {
    g_console = false;
    g_logFile = exeDir() + L"\\service.log";  // Programmordner: nur für Administratoren beschreibbar
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring cmd = argc >= 2 ? argv[1] : L"status";
    if (cmd == L"status") {
        if (argc >= 3) g_traceLines = _wcsicmp(argv[2], L"all") == 0 ? 100000u : static_cast<uint32_t>(_wtoi(argv[2]));
        return cmdStatus();
    }
    if (cmd == L"install") { CoInitializeEx(nullptr, COINIT_MULTITHREADED); return cmdInstall(); }
    if (cmd == L"repair") { CoInitializeEx(nullptr, COINIT_MULTITHREADED); return cmdRepair(); }
    if (cmd == L"probe") { CoInitializeEx(nullptr, COINIT_MULTITHREADED); return cmdProbe(argc >= 3 ? argv[2] : L""); }
    if (cmd == L"detach") return cmdDetach();
    if (cmd == L"uninstall") return cmdUninstall();
    if (cmd == L"service") return cmdService();
    say(L"Aufruf: CompressorNextSvc status | install | repair | detach | uninstall");
    return 2;
}
