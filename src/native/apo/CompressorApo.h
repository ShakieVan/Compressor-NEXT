// Compressor NEXT – Audio-Modul (APO) für die Windows-Audio-Engine.
#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>

#include "Maximizer.h"
#include "SharedState.h"

// {CE339205-A997-4182-B027-A9637647F8CA}
extern const CLSID CLSID_CompressorNextApo;
extern const APO_REG_PROPERTIES* GetCompressorApoRegProperties();

extern LONG g_objectCount;
extern LONG g_lockCount;

// Eingeblendete Zustandsdatei, von allen Instanzen im Prozess gemeinsam genutzt.
class StateMap {
public:
    static cnx::SharedState* acquire(bool* writable);  // nullptr, wenn die Datei fehlt
    static void release();
};

// Schreibt einen Eintrag ins Spurprotokoll der Zustandsdatei (nicht aus Echtzeit-Code aufrufen).
void Trace(uint32_t event, HRESULT hr, uint32_t arg = 0, uint32_t arg2 = 0, const GUID* guid = nullptr, uint32_t endpointHash = 0);

// Eigenes IUnknown des Objekts. Gleicher Aufbau wie IUnknown, aber ohne Weiterleitung an ein Hüllobjekt.
struct INonDelegatingUnknown {
    virtual HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG STDMETHODCALLTYPE NonDelegatingAddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE NonDelegatingRelease() = 0;
};

// Die Windows-Audio-Engine erzeugt Effekt-Module aggregiert, also als Teil eines eigenen Hüllobjekts
// (CSystemEffectWrapper). Deshalb leiten QueryInterface/AddRef/Release aller Schnittstellen an das
// Hüllobjekt weiter; nur das "nicht delegierende" IUnknown gehört dem Modul selbst.
class CompressorApo final : public CBaseAudioProcessingObject, public IAudioSystemEffects2, public INonDelegatingUnknown {
public:
    explicit CompressorApo(IUnknown* outer);
    ~CompressorApo();

    // IUnknown (leitet an das Hüllobjekt weiter; ohne Hüllobjekt an sich selbst)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // INonDelegatingUnknown
    HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE NonDelegatingAddRef() override;
    ULONG STDMETHODCALLTYPE NonDelegatingRelease() override;

    // IAudioProcessingObject
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP GetLatency(HNSTIME* pTime) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pOutputFormat, IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* pInputFormat, IAudioMediaType* pRequestedOutputFormat, IAudioMediaType** ppSupportedOutputFormat) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

    // IAudioProcessingObjectRT
    STDMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                   UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    // IAudioSystemEffects2
    STDMETHODIMP GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override;

private:
    void processBlock(APO_CONNECTION_PROPERTY* in, APO_CONNECTION_PROPERTY* out) noexcept;
    void pollSettings() noexcept;
    void publishMeter(bool force) noexcept;
    void claimSlot() noexcept;
    void releaseSlot() noexcept;

    LONG ref_ = 1;
    IUnknown* outer_ = nullptr;  // steuerndes IUnknown: Hüllobjekt oder das eigene nicht delegierende
    cnx::Maximizer max_;
    cnx::SharedState* state_ = nullptr;
    bool stateWritable_ = false;
    bool stateAcquired_ = false;
    uint32_t lastSeq_ = 0xFFFFFFFFu;
    uint32_t endpointHash_ = 0;
    cnx::MeterSlot* slot_ = nullptr;
    uint32_t slotToken_ = 0;
    uint32_t channels_ = 2;
    double sampleRate_ = 48000.0;
    uint32_t framesSincePublish_ = 0;
    uint32_t publishInterval_ = 1440;
    uint32_t tailFrames_ = 0;
    cnx::MeterBlock pending_;
    bool faulted_ = false;
};
