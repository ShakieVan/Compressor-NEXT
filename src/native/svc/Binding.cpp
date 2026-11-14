// Einbinden des Audio-Moduls in die Windows-Ausgabegeräte.
//
// Windows liest pro Ausgabegerät aus ...\MMDevices\Audio\Render\{Gerät}\FxProperties, welche Effekt-Module
// (APOs) es lädt: Stream-Effekt (,5), Modus-Effekt (,6), Endpunkt-Effekt (,7) – alle vor dem
// Lautstärkeregler des Geräts. Neuere Treiber (z. B. Realtek) geben stattdessen Listen vor (,13 ,14 ,15).
//
// Für jedes Gerät gibt es mehrere Platz-Varianten, in dieser Reihenfolge von "schonend" nach "eingreifend".
// Die Einrichtung nimmt die erste und prüft bei aktiven Geräten, ob der Ton wirklich durch das Modul läuft
// (verifyEndpoint); wenn nicht, kommt die nächste Variante dran. Die funktionierende Variante wird je Gerät
// gemerkt (...\CompressorNext\Preferred). Alles, was wir ändern, wird mit dem Ursprungswert unter
// ...\CompressorNext\Endpoints vermerkt und beim Deinstallieren zurückgesetzt.
#include "Binding.h"

#include <algorithm>

#include "SharedState.h"

namespace cnx {

namespace {

const wchar_t kRenderKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
const wchar_t kRecordKey[] = L"SOFTWARE\\ShakieVan\\CompressorNext\\Endpoints";
const wchar_t kPreferredKey[] = L"SOFTWARE\\ShakieVan\\CompressorNext\\Preferred";
#define CNX_FX L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}"
#define CNX_MODES L"{d3993a3f-99c2-4402-b5ec-a92a0367664b}"
const wchar_t kLfx[] = CNX_FX L",1", kGfx[] = CNX_FX L",2";
const wchar_t kSfx[] = CNX_FX L",5", kMfx[] = CNX_FX L",6", kEfx[] = CNX_FX L",7";
const wchar_t kSfxList[] = CNX_FX L",13", kMfxList[] = CNX_FX L",14", kEfxList[] = CNX_FX L",15";
const wchar_t kSfxModes[] = CNX_MODES L",5", kMfxModes[] = CNX_MODES L",6", kEfxModes[] = CNX_MODES L",7";
const wchar_t kDefaultMode[] = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
const wchar_t kDisableSysFx[] = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
const wchar_t kEndpointName[] = L"{a45c254e-df1c-4efd-8020-67d146a850e0},2";
const wchar_t kAdapterName[] = L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6";
const wchar_t kBluetoothCombined[] = L"{b3f8fa53-0004-438e-9003-51a46e139bfc},41";
const wchar_t kAbsent[] = L"!ABSENT";
const wchar_t* const kAllFxNames[] = {kSfx, kMfx, kEfx, kSfxList, kMfxList, kEfxList, kSfxModes, kMfxModes, kEfxModes};

struct Value {
    bool present = false;
    bool multi = false;
    bool usable = true;  // false: unbekannter Typ, nicht anfassen
    std::vector<std::wstring> items;

    bool empty() const { return !present || items.empty() || (items.size() == 1 && items[0].empty()); }
    const std::wstring& first() const { static const std::wstring none; return items.empty() ? none : items[0]; }
    bool operator==(const Value& o) const {
        if (present != o.present) return false;
        if (!present) return true;
        if (multi != o.multi || items.size() != o.items.size()) return false;
        for (size_t i = 0; i < items.size(); ++i)
            if (_wcsicmp(items[i].c_str(), o.items[i].c_str()) != 0) return false;
        return true;
    }
};

bool isOurs(const std::wstring& s) { return _wcsicmp(s.c_str(), kApoClsidString) == 0; }

bool containsOurs(const Value& v) {
    if (!v.present) return false;
    for (const auto& s : v.items)
        if (isOurs(s)) return true;
    return false;
}

Value single(const std::wstring& s) {
    Value v;
    v.present = true;
    v.items.push_back(s);
    return v;
}

Value list(std::vector<std::wstring> items) {
    Value v;
    v.present = true;
    v.multi = true;
    v.items = std::move(items);
    return v;
}

Value readValue(HKEY key, const wchar_t* name) {
    Value v;
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return v;
    v.present = true;
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
        v.usable = false;
        return v;
    }
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
    DWORD size = bytes;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS) {
        v.usable = false;
        return v;
    }
    if (type == REG_MULTI_SZ) {
        v.multi = true;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) v.items.emplace_back(p);
    } else {
        v.items.emplace_back(buf.data());
    }
    return v;
}

LSTATUS writeValue(HKEY key, const wchar_t* name, const Value& v) {
    if (!v.present) {
        const LSTATUS rc = RegDeleteValueW(key, name);
        return rc == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : rc;
    }
    if (v.multi) {
        std::vector<wchar_t> buf;
        for (const auto& s : v.items) {
            buf.insert(buf.end(), s.begin(), s.end());
            buf.push_back(0);
        }
        buf.push_back(0);
        return RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(buf.data()), static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
    }
    const std::wstring& s = v.first();
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(s.c_str()), static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
}

std::wstring encode(const Value& v) {
    if (!v.present) return kAbsent;
    std::wstring s = v.multi ? L"M:" : L"S:";
    for (size_t i = 0; i < v.items.size(); ++i) {
        if (i) s += L'|';
        s += v.items[i];
    }
    return s;
}

Value decode(const std::wstring& s) {
    Value v;
    if (s.size() < 2 || s == kAbsent) return v;
    v.present = true;
    v.multi = s[0] == L'M';
    size_t start = 2;
    while (true) {
        const size_t bar = s.find(L'|', start);
        v.items.push_back(s.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    return v;
}

std::wstring readString(HKEY key, const wchar_t* name) {
    const Value v = readValue(key, name);
    return v.present && v.usable ? v.first() : std::wstring();
}

std::vector<std::wstring> endpointGuids() {
    std::vector<std::wstring> result;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderKey, 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return result;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        result.emplace_back(name);
    }
    RegCloseKey(root);
    return result;
}

HKEY openEndpointKey(const std::wstring& guid, const wchar_t* sub, REGSAM access, LSTATUS* status = nullptr) {
    std::wstring path = std::wstring(kRenderKey) + L"\\" + guid;
    if (sub) path += std::wstring(L"\\") + sub;
    HKEY key = nullptr;
    const LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, access | KEY_WOW64_64KEY, &key);
    if (status) *status = rc;
    return rc == ERROR_SUCCESS ? key : nullptr;
}

struct FxState {
    Value lfx, gfx, sfx, mfx, efx, sfxList, mfxList, efxList, sfxModes, mfxModes, efxModes;

    void read(HKEY fx) {
        lfx = readValue(fx, kLfx); gfx = readValue(fx, kGfx);
        sfx = readValue(fx, kSfx); mfx = readValue(fx, kMfx); efx = readValue(fx, kEfx);
        sfxList = readValue(fx, kSfxList); mfxList = readValue(fx, kMfxList); efxList = readValue(fx, kEfxList);
        sfxModes = readValue(fx, kSfxModes); mfxModes = readValue(fx, kMfxModes); efxModes = readValue(fx, kEfxModes);
    }
    bool usable() const {
        for (const Value* v : {&lfx, &gfx, &sfx, &mfx, &efx, &sfxList, &mfxList, &efxList})
            if (!v->usable) return false;
        return true;
    }
    // Wo steht unser Modul? Leer, wenn nirgends.
    std::wstring boundSlot() const {
        if (containsOurs(sfx)) return L"Stream-Effekt";
        if (containsOurs(sfxList)) return sfxList.items.size() == 1 ? L"Stream-Effekt (Liste, ersetzt Herstellermodul)" : (isOurs(sfxList.items.front()) ? L"Stream-Effekt (Liste, vorn)" : L"Stream-Effekt (Liste)");
        if (containsOurs(mfx)) return L"Modus-Effekt";
        if (containsOurs(mfxList)) return L"Modus-Effekt (Liste)";
        if (containsOurs(efx)) return L"Endpunkt-Effekt";
        if (containsOurs(efxList)) return L"Endpunkt-Effekt (Liste)";
        return L"";
    }
};

struct Change {
    const wchar_t* name;
    Value original;
    Value written;
};

struct Variant {
    std::wstring slot;
    std::vector<Change> changes;
};

// Alle Platz-Varianten für ein Gerät, ausgehend vom Zustand OHNE unser Modul.
std::vector<Variant> variants(const FxState& s, bool bluetoothCombined) {
    std::vector<Variant> out;
    if (!s.usable()) return out;  // unbekanntes Format: Finger weg
    const std::wstring ours = kApoClsidString;
    const Value defaultModes = list({kDefaultMode});
    auto add = [&](const wchar_t* slot) -> Variant& {
        out.push_back(Variant{slot, {}});
        return out.back();
    };
    auto modes = [&](Variant& v, const wchar_t* name, const Value& current) {
        if (current.empty() && current.usable) v.changes.push_back({name, current, defaultModes});
    };
    auto withOurs = [&](const Value& current, const Value& singleFallback, bool front) {
        std::vector<std::wstring> items = current.empty() ? std::vector<std::wstring>() : current.items;
        if (items.empty() && !singleFallback.empty()) items.push_back(singleFallback.first());
        if (front) items.insert(items.begin(), ours); else items.push_back(ours);
        return list(items);
    };

    const bool composite = !s.sfxList.empty() || !s.mfxList.empty() || !s.efxList.empty();
    if (composite) {
        // Der Treiber gibt Listen vor (z. B. Realtek): in eine Liste einreihen.
        { Variant& v = add(L"Stream-Effekt (Liste)"); v.changes.push_back({kSfxList, s.sfxList, withOurs(s.sfxList, s.sfx, false)}); modes(v, kSfxModes, s.sfxModes); }
        if (!s.sfxList.empty() || !s.sfx.empty()) { Variant& v = add(L"Stream-Effekt (Liste, vorn)"); v.changes.push_back({kSfxList, s.sfxList, withOurs(s.sfxList, s.sfx, true)}); modes(v, kSfxModes, s.sfxModes); }
        if (!bluetoothCombined) { Variant& v = add(L"Endpunkt-Effekt (Liste)"); v.changes.push_back({kEfxList, s.efxList, withOurs(s.efxList, s.efx, false)}); modes(v, kEfxModes, s.efxModes); }
        { Variant& v = add(L"Modus-Effekt (Liste)"); v.changes.push_back({kMfxList, s.mfxList, withOurs(s.mfxList, s.mfx, false)}); modes(v, kMfxModes, s.mfxModes); }
        if (!s.sfxList.empty()) { Variant& v = add(L"Stream-Effekt (Liste, ersetzt Herstellermodul)"); v.changes.push_back({kSfxList, s.sfxList, list({ours})}); modes(v, kSfxModes, s.sfxModes); }
        return out;
    }

    const bool modern = !s.sfx.empty() || !s.mfx.empty() || !s.efx.empty();
    if (!modern && !s.lfx.empty()) {
        // Altes Schema (nur LFX/GFX): Sobald ein neuer Eintrag existiert, ignoriert Windows die alten.
        // Deshalb die alten Module in die neuen Plätze übernehmen und uns dahinter setzen.
        Variant& v = add(L"Endpunkt-Effekt");
        v.changes.push_back({kSfx, s.sfx, single(s.lfx.first())}); modes(v, kSfxModes, s.sfxModes);
        if (!s.gfx.empty()) { v.changes.push_back({kMfx, s.mfx, single(s.gfx.first())}); modes(v, kMfxModes, s.mfxModes); }
        v.changes.push_back({kEfx, s.efx, single(ours)}); modes(v, kEfxModes, s.efxModes);
        return out;
    }
    if (s.sfx.empty()) {
        Variant& v = add(L"Stream-Effekt");
        if (!modern && !s.gfx.empty()) { v.changes.push_back({kMfx, s.mfx, single(s.gfx.first())}); modes(v, kMfxModes, s.mfxModes); }
        v.changes.push_back({kSfx, s.sfx, single(ours)}); modes(v, kSfxModes, s.sfxModes);
    }
    if (s.mfx.empty() && (modern || s.gfx.empty())) { Variant& v = add(L"Modus-Effekt"); v.changes.push_back({kMfx, s.mfx, single(ours)}); modes(v, kMfxModes, s.mfxModes); }
    if (s.efx.empty() && !bluetoothCombined) { Variant& v = add(L"Endpunkt-Effekt"); v.changes.push_back({kEfx, s.efx, single(ours)}); modes(v, kEfxModes, s.efxModes); }
    if (!s.sfx.empty()) {
        { Variant& v = add(L"Stream-Effekt (Liste)"); v.changes.push_back({kSfxList, s.sfxList, list({s.sfx.first(), ours})}); modes(v, kSfxModes, s.sfxModes); }
        { Variant& v = add(L"Stream-Effekt (Liste, vorn)"); v.changes.push_back({kSfxList, s.sfxList, list({ours, s.sfx.first()})}); modes(v, kSfxModes, s.sfxModes); }
        { Variant& v = add(L"Stream-Effekt (ersetzt Herstellermodul)"); v.changes.push_back({kSfx, s.sfx, single(ours)}); modes(v, kSfxModes, s.sfxModes); }
    } else {
        Variant& v = add(L"Stream-Effekt (Liste)");
        v.changes.push_back({kSfxList, s.sfxList, list({ours})}); modes(v, kSfxModes, s.sfxModes);
    }
    return out;
}

void deleteRecord(const std::wstring& guid) {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kRecordKey) + L"\\" + guid).c_str());
}

void writeRecord(const std::wstring& guid, const std::vector<Change>& changes) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(kRecordKey) + L"\\" + guid).c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    for (const Change& c : changes) {
        writeValue(key, c.name, single(encode(c.original)));
        writeValue(key, (std::wstring(c.name) + L"#w").c_str(), single(encode(c.written)));
    }
    RegCloseKey(key);
}

int preferredVariant(const std::wstring& guid) {
    DWORD value = 0, size = sizeof(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPreferredKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExW(key, guid.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS) value = 0;
    RegCloseKey(key);
    return static_cast<int>(value);
}

void setPreferredVariant(const std::wstring& guid, int variant) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kPreferredKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const DWORD value = static_cast<DWORD>(variant);
    RegSetValueExW(key, guid.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

DWORD deviceState(const std::wstring& guid) {
    DWORD state = 0, size = sizeof(state);
    if (HKEY ep = openEndpointKey(guid, nullptr, KEY_QUERY_VALUE)) {
        RegQueryValueExW(ep, L"DeviceState", nullptr, nullptr, reinterpret_cast<BYTE*>(&state), &size);
        RegCloseKey(ep);
    }
    return state;
}

bool isBluetoothCombined(const std::wstring& guid) {
    bool result = false;
    if (HKEY props = openEndpointKey(guid, L"Properties", KEY_QUERY_VALUE)) {
        result = RegQueryValueExW(props, kBluetoothCombined, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(props);
    }
    return result;
}

// Nimmt unser Modul aus einem Gerät heraus und stellt die vermerkten Ursprungswerte wieder her.
bool unbindEndpoint(const std::wstring& guid) {
    HKEY fx = openEndpointKey(guid, L"FxProperties", KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (!fx) return false;
    bool touched = false;
    // 1. Vermerkte Änderungen zurücknehmen, sofern der Wert noch so dasteht, wie wir ihn geschrieben haben.
    HKEY rec = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(kRecordKey) + L"\\" + guid).c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &rec) == ERROR_SUCCESS) {
        for (const wchar_t* name : kAllFxNames) {
            const Value original = readValue(rec, name);
            const Value written = readValue(rec, (std::wstring(name) + L"#w").c_str());
            if (!original.present || !written.present) continue;
            if (readValue(fx, name) == decode(written.first())) {
                writeValue(fx, name, decode(original.first()));
                touched = true;
            }
        }
        RegCloseKey(rec);
    }
    // 2. Sicherheitsnetz: Steht unser Modul noch irgendwo, dort entfernen.
    for (const wchar_t* name : {kSfx, kMfx, kEfx}) {
        const Value v = readValue(fx, name);
        if (v.usable && containsOurs(v)) { writeValue(fx, name, Value()); touched = true; }
    }
    for (const wchar_t* name : {kSfxList, kMfxList, kEfxList}) {
        const Value v = readValue(fx, name);
        if (!v.usable || !containsOurs(v)) continue;
        std::vector<std::wstring> rest;
        for (const auto& s : v.items)
            if (!isOurs(s)) rest.push_back(s);
        writeValue(fx, name, rest.empty() ? Value() : list(rest));
        touched = true;
    }
    deleteRecord(guid);
    RegCloseKey(fx);
    return touched;
}

enum class BindOutcome { Bound, Already, Skipped, Failed };

// Bindet ein (nicht eingebundenes) Gerät mit der gewünschten Variante ein.
BindOutcome bindEndpoint(const std::wstring& guid, int wanted, bool dryRun, std::wstring* slot, LSTATUS* error) {
    LSTATUS rc = ERROR_SUCCESS;
    HKEY fx = openEndpointKey(guid, L"FxProperties", dryRun ? KEY_QUERY_VALUE : (KEY_QUERY_VALUE | KEY_SET_VALUE), &rc);
    if (!fx) {
        if (error) *error = rc;
        return rc == ERROR_FILE_NOT_FOUND ? BindOutcome::Skipped : BindOutcome::Failed;
    }
    FxState s;
    s.read(fx);
    if (!s.boundSlot().empty()) {
        if (slot) *slot = s.boundSlot();
        RegCloseKey(fx);
        return BindOutcome::Already;
    }
    const std::vector<Variant> all = variants(s, isBluetoothCombined(guid));
    if (all.empty() || wanted < 0 || wanted >= static_cast<int>(all.size())) {
        if (error) *error = ERROR_NOT_SUPPORTED;
        RegCloseKey(fx);
        return all.empty() ? BindOutcome::Skipped : BindOutcome::Failed;
    }
    const Variant& v = all[wanted];
    if (slot) *slot = v.slot;
    if (!dryRun) {
        deleteRecord(guid);  // ein alter Vermerk passt nicht mehr (z. B. nach einem Treiber-Update)
        writeRecord(guid, v.changes);
        for (const Change& c : v.changes) {
            rc = writeValue(fx, c.name, c.written);
            if (rc != ERROR_SUCCESS) break;
        }
        if (rc != ERROR_SUCCESS) {
            for (const Change& c : v.changes) writeValue(fx, c.name, c.original);  // zurück auf Anfang
            deleteRecord(guid);
            if (error) *error = rc;
            RegCloseKey(fx);
            return BindOutcome::Failed;
        }
    }
    RegCloseKey(fx);
    return BindOutcome::Bound;
}

int variantCountFor(const std::wstring& guid) {
    HKEY fx = openEndpointKey(guid, L"FxProperties", KEY_QUERY_VALUE);
    if (!fx) return 0;
    FxState s;
    s.read(fx);
    RegCloseKey(fx);
    return static_cast<int>(variants(s, isBluetoothCombined(guid)).size());
}

} // namespace

std::wstring describeEndpoint(const std::wstring& guid) {
    std::wstring text = guid;
    if (HKEY props = openEndpointKey(guid, L"Properties", KEY_QUERY_VALUE)) {
        const std::wstring name = readString(props, kEndpointName), adapter = readString(props, kAdapterName);
        if (!name.empty()) text = name + (adapter.empty() ? L"" : L" (" + adapter + L")") + L" " + guid;
        RegCloseKey(props);
    }
    return text;
}

std::vector<EndpointStatus> listEndpoints() {
    std::vector<EndpointStatus> result;
    for (const std::wstring& guid : endpointGuids()) {
        EndpointStatus e;
        e.guid = guid;
        e.state = deviceState(guid);
        e.name = describeEndpoint(guid);
        e.variant = preferredVariant(guid);
        if (HKEY props = openEndpointKey(guid, L"Properties", KEY_QUERY_VALUE)) {
            DWORD off = 0, size = sizeof(off);
            if (RegQueryValueExW(props, kDisableSysFx, nullptr, nullptr, reinterpret_cast<BYTE*>(&off), &size) == ERROR_SUCCESS) e.enhancementsOff = off != 0;
            RegCloseKey(props);
        }
        if (HKEY fx = openEndpointKey(guid, L"FxProperties", KEY_QUERY_VALUE)) {
            e.hasFxKey = true;
            FxState s;
            s.read(fx);
            RegCloseKey(fx);
            e.slot = s.boundSlot();
            e.bound = !e.slot.empty();
            if (!e.bound) {
                const std::vector<Variant> all = variants(s, isBluetoothCombined(guid));
                e.variantCount = static_cast<int>(all.size());
                if (all.empty()) e.slot = L"unbekanntes Format";
                else e.slot = all[std::min<size_t>(static_cast<size_t>(std::max(0, e.variant)), all.size() - 1)].slot;
            }
        }
        result.push_back(std::move(e));
    }
    return result;
}

BindSummary bindAll(bool dryRun) {
    BindSummary sum;
    for (const std::wstring& guid : endpointGuids()) {
        ++sum.total;
        std::wstring slot;
        LSTATUS rc = ERROR_SUCCESS;
        int wanted = preferredVariant(guid);
        const int count = variantCountFor(guid);
        if (wanted >= count) wanted = 0;  // Treiber hat sich geändert: von vorn
        switch (bindEndpoint(guid, wanted, dryRun, &slot, &rc)) {
            case BindOutcome::Already: ++sum.alreadyBound; break;
            case BindOutcome::Skipped: ++sum.skipped; break;
            case BindOutcome::Failed:
                ++sum.failed;
                sum.log.push_back(L"Einbinden fehlgeschlagen (" + std::to_wstring(rc) + L"): " + describeEndpoint(guid));
                break;
            case BindOutcome::Bound:
                ++sum.newlyBound;
                if (deviceState(guid) == 1) sum.activeChanged = true;
                sum.log.push_back((dryRun ? L"Würde einbinden als " : L"Eingebunden als ") + slot + L": " + describeEndpoint(guid));
                break;
        }
    }
    return sum;
}

BindSummary unbindAll() {
    BindSummary sum;
    for (const std::wstring& guid : endpointGuids()) {
        ++sum.total;
        if (unbindEndpoint(guid)) {
            ++sum.newlyBound;
            if (deviceState(guid) == 1) sum.activeChanged = true;
            sum.log.push_back(L"Entfernt: " + describeEndpoint(guid));
        }
    }
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kRecordKey, KEY_WOW64_64KEY, 0);
    return sum;
}

bool unbindOne(const std::wstring& guid) { return unbindEndpoint(guid); }

bool rebindWithVariant(const std::wstring& guid, int variant, std::wstring* slot) {
    unbindEndpoint(guid);
    LSTATUS rc = ERROR_SUCCESS;
    const BindOutcome outcome = bindEndpoint(guid, variant, false, slot, &rc);
    if (outcome != BindOutcome::Bound) return false;
    setPreferredVariant(guid, variant);
    return true;
}

int enableEnhancements() {
    int count = 0;
    for (const std::wstring& guid : endpointGuids()) {
        HKEY props = openEndpointKey(guid, L"Properties", KEY_QUERY_VALUE | KEY_SET_VALUE);
        if (!props) continue;
        DWORD off = 0, size = sizeof(off);
        if (RegQueryValueExW(props, kDisableSysFx, nullptr, nullptr, reinterpret_cast<BYTE*>(&off), &size) == ERROR_SUCCESS && off != 0) {
            if (RegDeleteValueW(props, kDisableSysFx) == ERROR_SUCCESS) ++count;
        }
        RegCloseKey(props);
    }
    return count;
}

} // namespace cnx
