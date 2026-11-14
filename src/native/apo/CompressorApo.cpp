// Compressor NEXT – Audio-Modul (APO).
//
// Läuft im Windows-Audiodienst (audiodg.exe) je Audiostrom, vor dem Lautstärkeregler.
// Oberstes Gebot: Der Ton darf nie ausfallen. Fehlt die Zustandsdatei oder geht etwas schief,
// reicht das Modul das Audio unverändert durch.
#include "CompressorApo.h"

#include <propidl.h>
#include <cmath>
#include <cstring>
#include <new>

// {CE339205-A997-4182-B027-A9637647F8CA}
const CLSID CLSID_CompressorNextApo = {0xce339205, 0xa997, 0x4182, {0xb0, 0x27, 0xa9, 0x63, 0x76, 0x47, 0xf8, 0xca}};

// PKEY_AudioEndpoint_GUID
static const PROPERTYKEY kPkeyEndpointGuid = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};
// AUDIO_EFFECT_TYPE_DYNAMIC_RANGE_COMPRESSION
static const GUID kEffectDynamicRangeCompression = {0x6f64adce, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}};

LONG g_objectCount = 0;
LONG g_lockCount = 0;

#pragma warning(suppress : 4815)  // Hilfsklasse des SDK mit leerem Feld bei genau einer Schnittstelle
static const CRegAPOProperties<1> g_regProperties(
    CLSID_CompressorNextApo, L"Compressor NEXT", L"Copyright (c) 2026 ShakieVan", 1, 0,
    __uuidof(IAudioProcessingObject),
    static_cast<APO_FLAG>(APO_FLAG_INPLACE | APO_FLAG_SAMPLESPERFRAME_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH));

const APO_REG_PROPERTIES* GetCompressorApoRegProperties() { return g_regProperties; }

// ---------------------------------------------------------------------------------------------
// Zustandsdatei
// ---------------------------------------------------------------------------------------------
static SRWLOCK g_stateLock = SRWLOCK_INIT;
static int g_stateRefs = 0;
static cnx::SharedState* g_state = nullptr;
static bool g_stateWritable = false;

static bool statePath(wchar_t* path, DWORD cch) {
    const DWORD n = ExpandEnvironmentStringsW(cnx::kStateFileTemplate, path, cch);
    if (n > 0 && n <= cch && path[0] != L'%') return true;
    // Umgebungsvariable fehlt: Laufwerk des Windows-Ordners verwenden.
    wchar_t win[MAX_PATH];
    const UINT w = GetSystemWindowsDirectoryW(win, MAX_PATH);
    if (w < 3 || w >= MAX_PATH) return false;
    win[3] = 0;  // "C:\"
    return swprintf_s(path, cch, L"%sProgramData\\CompressorNext\\state.bin", win) > 0;
}

static void openStateLocked() {
    wchar_t path[MAX_PATH];
    if (!statePath(path, MAX_PATH)) return;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    bool writable = true;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        writable = false;  // nur lesbar: Einstellungen wirken, die Pegelanzeige bleibt leer
        file = CreateFileW(path, GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart >= cnx::kStateFileSize) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, cnx::kStateFileSize, nullptr);
        if (mapping) {
            void* view = MapViewOfFile(mapping, writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ, 0, 0, cnx::kStateFileSize);
            CloseHandle(mapping);  // die Einblendung hält den Abschnitt am Leben
            if (view) {
                auto* st = static_cast<cnx::SharedState*>(view);
                if (st->settings.magic == cnx::kStateMagic && st->settings.version == cnx::kStateVersion) {
                    g_state = st;
                    g_stateWritable = writable;
                } else {
                    UnmapViewOfFile(view);
                }
            }
        }
    }
    CloseHandle(file);
}

cnx::SharedState* StateMap::acquire(bool* writable) {
    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_state) openStateLocked();
    cnx::SharedState* st = g_state;
    if (st) ++g_stateRefs;
    if (writable) *writable = g_stateWritable;
    ReleaseSRWLockExclusive(&g_stateLock);
    return st;
}

void StateMap::release() {
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_stateRefs > 0 && --g_stateRefs == 0 && g_state) {
        UnmapViewOfFile(g_state);
        g_state = nullptr;
        g_stateWritable = false;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

static inline uint32_t readVolatile(const uint32_t* p) { return *static_cast<const volatile uint32_t*>(p); }

// ---------------------------------------------------------------------------------------------
// Spurprotokoll
// ---------------------------------------------------------------------------------------------
static SRWLOCK g_traceLock = SRWLOCK_INIT;
static cnx::SharedState* g_traceState = nullptr;
static int g_traceAttempts = 0;

void Trace(uint32_t event, HRESULT hr, uint32_t arg, uint32_t arg2, const GUID* guid, uint32_t endpointHash) {
    AcquireSRWLockExclusive(&g_traceLock);
    if (!g_traceState && g_traceAttempts < 20) {
        ++g_traceAttempts;
        bool writable = false;
        cnx::SharedState* st = StateMap::acquire(&writable);  // bleibt bis zum Prozessende eingeblendet
        if (st && writable) g_traceState = st;
        else if (st) StateMap::release();
    }
    cnx::SharedState* st = g_traceState;
    ReleaseSRWLockExclusive(&g_traceLock);
    if (!st) return;
    const uint32_t index = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&st->traceCount))) - 1;
    cnx::TraceEntry* e = &st->trace[index % cnx::kTraceEntries];
    e->tick = GetTickCount64();
    e->pid = GetCurrentProcessId();
    e->event = event;
    e->hr = static_cast<uint32_t>(hr);
    e->arg = arg;
    e->arg2 = arg2;
    e->endpointHash = endpointHash;
    if (guid) std::memcpy(e->guid, guid, 16); else std::memset(e->guid, 0, 16);
}

static void formatSummary(IAudioMediaType* type, uint32_t* channels, uint32_t* rate) {
    *channels = 0;
    *rate = 0;
    UNCOMPRESSEDAUDIOFORMAT f{};
    if (type && SUCCEEDED(type->GetUncompressedAudioFormat(&f))) {
        // Kanäle plus Bytes je Abtastwert (in den oberen 16 Bit), damit auch Nicht-Gleitkomma-Anfragen erkennbar sind
        *channels = f.dwSamplesPerFrame | (f.dwBytesPerSampleContainer << 16);
        *rate = static_cast<uint32_t>(f.fFramesPerSecond);
    }
}

// ---------------------------------------------------------------------------------------------
// Objekt
// ---------------------------------------------------------------------------------------------
CompressorApo::CompressorApo(IUnknown* outer) : CBaseAudioProcessingObject(g_regProperties) {
    // Das nicht delegierende IUnknown hat denselben Aufbau wie IUnknown und dient ohne Hüllobjekt als solches.
    outer_ = outer ? outer : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    InterlockedIncrement(&g_objectCount);
}

CompressorApo::~CompressorApo() {
    Trace(cnx::kTraceDestroy, S_OK, 0, 0, nullptr, endpointHash_);
    releaseSlot();
    if (stateAcquired_) StateMap::release();
    InterlockedDecrement(&g_objectCount);
}

STDMETHODIMP CompressorApo::QueryInterface(REFIID riid, void** ppv) { return outer_->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CompressorApo::AddRef() { return outer_->AddRef(); }
STDMETHODIMP_(ULONG) CompressorApo::Release() { return outer_->Release(); }

HRESULT STDMETHODCALLTYPE CompressorApo::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
        // Das eigene IUnknown: zählt die Lebensdauer des Moduls, nie die des Hüllobjekts.
        *ppv = static_cast<INonDelegatingUnknown*>(this);
        NonDelegatingAddRef();
        return S_OK;
    }
    if (riid == __uuidof(IAudioProcessingObject)) *ppv = static_cast<IAudioProcessingObject*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectRT)) *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2)) *ppv = static_cast<IAudioSystemEffects2*>(this);
    else {
        *ppv = nullptr;
        // Jede unbekannte Schnittstelle nur einmal je Prozess vermerken, sonst läuft das Spurprotokoll über.
        static GUID seen[48];
        static LONG seenCount = 0;
        bool known = false;
        const LONG n = seenCount < 48 ? seenCount : 48;
        for (LONG i = 0; i < n; ++i)
            if (seen[i] == riid) { known = true; break; }
        if (!known) {
            const LONG slot = InterlockedIncrement(&seenCount) - 1;
            if (slot < 48) {
                seen[slot] = riid;
                Trace(cnx::kTraceUnknownInterface, E_NOINTERFACE, 0, 0, &riid, endpointHash_);
            }
        }
        return E_NOINTERFACE;
    }
    AddRef();  // über das steuernde IUnknown
    return S_OK;
}

ULONG STDMETHODCALLTYPE CompressorApo::NonDelegatingAddRef() { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }

ULONG STDMETHODCALLTYPE CompressorApo::NonDelegatingRelease() {
    const LONG n = InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

STDMETHODIMP CompressorApo::IsInputFormatSupported(IAudioMediaType* pOutputFormat, IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat) {
    const HRESULT hr = CBaseAudioProcessingObject::IsInputFormatSupported(pOutputFormat, pRequestedInputFormat, ppSupportedInputFormat);
    uint32_t channels, rate;
    formatSummary(pRequestedInputFormat, &channels, &rate);
    Trace(cnx::kTraceInputFormat, hr, channels, rate, nullptr, endpointHash_);
    return hr;
}

STDMETHODIMP CompressorApo::IsOutputFormatSupported(IAudioMediaType* pInputFormat, IAudioMediaType* pRequestedOutputFormat, IAudioMediaType** ppSupportedOutputFormat) {
    const HRESULT hr = CBaseAudioProcessingObject::IsOutputFormatSupported(pInputFormat, pRequestedOutputFormat, ppSupportedOutputFormat);
    uint32_t channels, rate;
    formatSummary(pRequestedOutputFormat, &channels, &rate);
    Trace(cnx::kTraceOutputFormat, hr, channels, rate, nullptr, endpointHash_);
    return hr;
}

STDMETHODIMP CompressorApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    if ((pbyData == nullptr) != (cbDataSize == 0)) { Trace(cnx::kTraceInitialize, E_INVALIDARG, cbDataSize); return E_INVALIDARG; }
    if (m_bIsInitialized) { Trace(cnx::kTraceInitialize, APOERR_ALREADY_INITIALIZED, cbDataSize); return APOERR_ALREADY_INITIALIZED; }

    // APOInitSystemEffects, ...2 und ...3 beginnen gleich; uns genügt die Endpunkt-Kennung.
    if (pbyData && cbDataSize >= sizeof(APOInitSystemEffects)) {
        const auto* init = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
        if (init->pAPOEndpointProperties) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(init->pAPOEndpointProperties->GetValue(kPkeyEndpointGuid, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                endpointHash_ = cnx::hashEndpoint(pv.pwszVal);
            PropVariantClear(&pv);
        }
    }
    if (!stateAcquired_) {
        state_ = StateMap::acquire(&stateWritable_);
        stateAcquired_ = state_ != nullptr;
    }
    // Diagnose: an der Zustandsdatei lässt sich ablesen, ob Windows das Modul überhaupt lädt.
    if (state_ && stateWritable_) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state_->apoInitCount));
    GUID mode{};
    uint32_t discovery = 0;
    if (pbyData && cbDataSize >= sizeof(APOInitSystemEffects2)) {
        const auto* init2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        mode = init2->AudioProcessingMode;
        discovery = init2->InitializeForDiscoveryOnly ? 1 : 0;
    }
    Trace(cnx::kTraceInitialize, S_OK, cbDataSize, discovery, &mode, endpointHash_);
    m_bIsInitialized = true;
    return S_OK;
}

STDMETHODIMP CompressorApo::GetLatency(HNSTIME* pTime) {
    if (!pTime) return E_POINTER;
    *pTime = static_cast<HNSTIME>(std::llround(max_.latencyFrames() * 1.0e7 / sampleRate_));
    return S_OK;
}

STDMETHODIMP CompressorApo::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                           UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {
    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr)) {
        uint32_t channels = 0, rate = 0;
        if (u32NumOutputConnections > 0 && ppOutputConnections && ppOutputConnections[0]) formatSummary(ppOutputConnections[0]->pFormat, &channels, &rate);
        Trace(cnx::kTraceLock, hr, channels, rate, nullptr, endpointHash_);
        return hr;
    }

    UNCOMPRESSEDAUDIOFORMAT fmt{};
    hr = ppOutputConnections[0]->pFormat->GetUncompressedAudioFormat(&fmt);
    if (SUCCEEDED(hr) && (fmt.dwSamplesPerFrame == 0 || fmt.dwBytesPerSampleContainer != 4 || !(fmt.fFramesPerSecond > 0.0f)))
        hr = APOERR_FORMAT_NOT_SUPPORTED;
    if (SUCCEEDED(hr)) {
        try {
            channels_ = fmt.dwSamplesPerFrame;
            sampleRate_ = fmt.fFramesPerSecond;
            if (!stateAcquired_) {
                state_ = StateMap::acquire(&stateWritable_);
                stateAcquired_ = state_ != nullptr;
            }
            lastSeq_ = 0xFFFFFFFFu;
            max_.setSettings(cnx::Settings{false, -18.0f, -1.0f});
            pollSettings();
            max_.prepare(sampleRate_, static_cast<int>(channels_), fmt.dwChannelMask);
            publishInterval_ = static_cast<uint32_t>(sampleRate_ * 0.030);
            framesSincePublish_ = 0;
            tailFrames_ = 0;
            faulted_ = false;
            claimSlot();
            if (state_ && stateWritable_) {
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state_->apoLockCount));
                state_->apoLastLockTick = GetTickCount64();
            }
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
    }
    Trace(cnx::kTraceLock, hr, fmt.dwSamplesPerFrame | (fmt.dwBytesPerSampleContainer << 16), static_cast<uint32_t>(fmt.fFramesPerSecond), nullptr, endpointHash_);
    if (FAILED(hr)) {
        CBaseAudioProcessingObject::UnlockForProcess();
        return hr;
    }
    InterlockedIncrement(&g_lockCount);
    return S_OK;
}

STDMETHODIMP CompressorApo::UnlockForProcess() {
    const HRESULT hr = CBaseAudioProcessingObject::UnlockForProcess();
    if (SUCCEEDED(hr)) {
        releaseSlot();
        InterlockedDecrement(&g_lockCount);
    }
    Trace(cnx::kTraceUnlock, hr, 0, 0, nullptr, endpointHash_);
    return hr;
}

STDMETHODIMP CompressorApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE) {
    if (!ppEffectsIds || !pcEffects) return E_POINTER;
    *ppEffectsIds = nullptr;
    *pcEffects = 0;
    if (state_ && readVolatile(&state_->settings.enabled) != 0) {
        auto* list = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
        if (!list) return E_OUTOFMEMORY;
        list[0] = kEffectDynamicRangeCompression;
        *ppEffectsIds = list;
        *pcEffects = 1;
    }
    return S_OK;
}

// ---------------------------------------------------------------------------------------------
// Pegelanzeige
// ---------------------------------------------------------------------------------------------
void CompressorApo::claimSlot() noexcept {
    slot_ = nullptr;
    if (!state_ || !stateWritable_) return;
    static LONG counter = 0;
    const uint32_t token = ((GetCurrentProcessId() << 8) ^ static_cast<uint32_t>(InterlockedIncrement(&counter))) | 1u;
    const uint64_t now = GetTickCount64();
    for (int pass = 0; pass < 2 && !slot_; ++pass) {
        for (int i = 0; i < cnx::kMeterSlots; ++i) {
            cnx::MeterSlot* s = &state_->meters[i];
            const LONG owner = static_cast<LONG>(readVolatile(&s->owner));
            // 1. Durchgang: freie Plätze; 2. Durchgang: verwaiste (z. B. nach einem Absturz des Audiodienstes)
            const bool usable = pass == 0 ? owner == 0 : (owner != 0 && now - s->tick > 5000);
            if (!usable) continue;
            if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&s->owner), static_cast<LONG>(token), owner) != owner) continue;
            s->seq = 0;
            s->endpointHash = endpointHash_;
            s->sampleRate = static_cast<uint32_t>(sampleRate_);
            s->channels = channels_;
            s->flags = 0;
            s->inL = s->inR = s->outL = s->outR = 0.0f;
            s->minGain = 1.0f;
            s->tick = now;
            slot_ = s;
            slotToken_ = token;
            break;
        }
    }
}

void CompressorApo::releaseSlot() noexcept {
    if (!slot_) return;
    if (readVolatile(&slot_->owner) == slotToken_) {
        slot_->tick = 0;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot_->owner), 0);
    }
    slot_ = nullptr;
}

void CompressorApo::publishMeter(bool force) noexcept {
    if (!slot_) return;
    if (!force && framesSincePublish_ < publishInterval_) return;
    framesSincePublish_ = 0;
    const cnx::MeterBlock m = max_.takeMeter();
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&slot_->seq));  // ungerade: wird geschrieben
    slot_->inL = m.inL;
    slot_->inR = m.inR;
    slot_->outL = m.outL;
    slot_->outR = m.outR;
    slot_->minGain = m.minGain;
    slot_->flags = max_.engaged() ? cnx::kMeterFlagEngaged : 0;
    slot_->tick = GetTickCount64();
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&slot_->seq));
}

// ---------------------------------------------------------------------------------------------
// Echtzeit
// ---------------------------------------------------------------------------------------------
void CompressorApo::pollSettings() noexcept {
    cnx::Settings st;
    st.enabled = false;
    if (!state_) {
        if (lastSeq_ != 0) { lastSeq_ = 0; max_.setSettings(st); }  // ohne Zustandsdatei: unverändert durchreichen
        return;
    }
    const cnx::SharedSettings& s = state_->settings;
    const uint32_t seq1 = readVolatile(&s.seq);
    if (seq1 == lastSeq_ || (seq1 & 1u)) return;
    const uint32_t enabled = readVolatile(&s.enabled);
    st.thresholdDb = s.thresholdDb;
    st.outputDb = s.outputDb;
    uint32_t count = readVolatile(&s.excludedCount);
    if (count > static_cast<uint32_t>(cnx::kMaxExcluded)) count = cnx::kMaxExcluded;
    bool excluded = false;
    for (uint32_t i = 0; i < count; ++i)
        if (readVolatile(&state_->excluded[i]) == endpointHash_) excluded = true;
    if (readVolatile(&s.seq) != seq1) return;  // währenddessen geändert: beim nächsten Block erneut
    lastSeq_ = seq1;
    st.enabled = enabled != 0 && !excluded && s.magic == cnx::kStateMagic;
    max_.setSettings(st);
}

void CompressorApo::processBlock(APO_CONNECTION_PROPERTY* in, APO_CONNECTION_PROPERTY* out) noexcept {
    const UINT32 frames = in->u32ValidFrameCount;
    auto* src = reinterpret_cast<const float*>(in->pBuffer);
    auto* dst = reinterpret_cast<float*>(out->pBuffer);
    pollSettings();
    framesSincePublish_ += frames;

    if (in->u32BufferFlags == BUFFER_SILENT) {
        if (tailFrames_ == 0) {
            // Nichts mehr in der Verzögerungsleitung: Stille bleibt Stille.
            out->u32ValidFrameCount = frames;
            out->u32BufferFlags = BUFFER_SILENT;
            publishMeter(false);
            return;
        }
        std::memset(dst, 0, static_cast<size_t>(frames) * channels_ * sizeof(float));
        max_.process(dst, dst, static_cast<int>(frames));
        tailFrames_ = tailFrames_ > frames ? tailFrames_ - frames : 0;
    } else {
        max_.process(src, dst, static_cast<int>(frames));
        tailFrames_ = static_cast<uint32_t>(max_.latencyFrames()) + 1;
    }
    out->u32ValidFrameCount = frames;
    out->u32BufferFlags = BUFFER_VALID;
    publishMeter(false);
}

static void passThrough(APO_CONNECTION_PROPERTY* in, APO_CONNECTION_PROPERTY* out, uint32_t channels) {
    if (in->u32BufferFlags == BUFFER_VALID && out->pBuffer != in->pBuffer)
        std::memcpy(reinterpret_cast<void*>(out->pBuffer), reinterpret_cast<const void*>(in->pBuffer),
                    static_cast<size_t>(in->u32ValidFrameCount) * channels * sizeof(float));
    out->u32ValidFrameCount = in->u32ValidFrameCount;
    out->u32BufferFlags = in->u32BufferFlags;
}

// Sicherheitsnetz: Ein Fehler im eigenen Code darf den Audiodienst nicht mitreißen.
static bool guardedProcess(void (*fn)(void*, APO_CONNECTION_PROPERTY*, APO_CONNECTION_PROPERTY*), void* self,
                           APO_CONNECTION_PROPERTY* in, APO_CONNECTION_PROPERTY* out) {
    __try {
        fn(self, in, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

STDMETHODIMP_(void) CompressorApo::APOProcess(UINT32, APO_CONNECTION_PROPERTY** ppInputConnections,
                                              UINT32, APO_CONNECTION_PROPERTY** ppOutputConnections) {
    APO_CONNECTION_PROPERTY* in = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];
    if (in->u32BufferFlags == BUFFER_INVALID) return;
    if (!faulted_) {
        const bool ok = guardedProcess([](void* self, APO_CONNECTION_PROPERTY* i, APO_CONNECTION_PROPERTY* o) {
            static_cast<CompressorApo*>(self)->processBlock(i, o);
        }, this, in, out);
        if (ok) return;
        faulted_ = true;  // ab jetzt nur noch durchreichen
        Trace(cnx::kTraceFault, E_FAIL, 0, 0, nullptr, endpointHash_);
    }
    passThrough(in, out, channels_);
}
