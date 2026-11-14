// Funktionsprüfung: Läuft Ton auf einem Ausgabegerät wirklich durch das Audio-Modul?
//
// Öffnet einen gewöhnlichen (geteilten) Audiostrom auf dem Gerät, spielt Stille ab und wartet darauf, dass
// sich das Modul für dieses Gerät in der Zustandsdatei meldet. So prüft auch Equalizer APO seine Einrichtung.
#include "Binding.h"

#include <objbase.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstdio>

#include "SharedState.h"

namespace cnx {

namespace {

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Meldet sich gerade ein Audiostrom des Moduls für dieses Gerät?
bool moduleReports(uint32_t endpointHash, ULONGLONG since, bool* readable) {
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(kStateFileTemplate, path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *readable = false;
        return false;
    }
    std::vector<BYTE> buf(kStateFileSize, 0);
    DWORD got = 0;
    ReadFile(f, buf.data(), kStateFileSize, &got, nullptr);
    CloseHandle(f);
    *readable = got >= kStateFileSize;
    const auto* st = reinterpret_cast<const SharedState*>(buf.data());
    // Nur Meldungen zählen, die NACH dem Start des Teststroms geschrieben wurden. Ein Platz, den ein gerade
    // beendeter Audiodienst hinterlassen hat, trägt einen älteren Zeitstempel.
    for (const MeterSlot& slot : st->meters)
        if (slot.owner != 0 && slot.endpointHash == endpointHash && slot.tick > since) return true;
    return false;
}

} // namespace

VerifyResult verifyEndpoint(const std::wstring& guid, std::wstring* detail, void (*whileRunning)()) {
    const uint32_t hash = hashEndpoint(guid.c_str());
    const std::wstring id = L"{0.0.0.00000000}." + guid;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    // Nach einem Neustart des Audiodienstes braucht das Gerät einen Moment: einige Sekunden lang versuchen.
    for (int attempt = 0; SUCCEEDED(hr) && attempt < 20; ++attempt) {
        hr = enumerator->GetDevice(id.c_str(), &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
        if (SUCCEEDED(hr)) break;
        if (mix) { CoTaskMemFree(mix); mix = nullptr; }
        release(client);
        release(device);
        if (attempt == 19) break;
        Sleep(500);
        hr = S_OK;
    }
    VerifyResult result = VerifyResult::Unknown;
    if (!client || FAILED(hr)) {
        wchar_t code[16];
        swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
        if (detail) *detail = std::wstring(L"Gerät lässt sich nicht öffnen (") + code + L")";
    } else {
        UINT32 frames = 0;
        client->GetBufferSize(&frames);
        if (SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)))) {
            BYTE* data = nullptr;
            if (SUCCEEDED(render->GetBuffer(frames, &data))) {
                // Kein "Stille"-Merker, sondern ein unhörbar leises Signal (-100 dBFS): So ist sicher, dass die
                // Audio-Engine den Strom wirklich durch die Effekt-Module rechnet.
                const bool isFloat = mix->wBitsPerSample == 32 &&
                    (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                     (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22 &&
                      reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
                if (isFloat) {
                    auto* samples = reinterpret_cast<float*>(data);
                    const size_t count = static_cast<size_t>(frames) * mix->nChannels;
                    for (size_t i = 0; i < count; ++i) samples[i] = ((i / mix->nChannels) & 64) ? 1.0e-5f : -1.0e-5f;
                    render->ReleaseBuffer(frames, 0);
                } else {
                    render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
        }
        Sleep(150);  // Abstand zu allem, was vor dem Teststrom lag
        const ULONGLONG since = GetTickCount64();
        if (SUCCEEDED(client->Start())) {
            bool seen = false, readable = true;
            for (int i = 0; i < 30 && !seen && readable; ++i) {  // bis zu 3 s
                Sleep(100);
                seen = moduleReports(hash, since, &readable);
            }
            if (whileRunning) whileRunning();
            client->Stop();
            if (!readable) {
                if (detail) *detail = L"Zustandsdatei nicht lesbar";
            } else {
                result = seen ? VerifyResult::Works : VerifyResult::NotLoaded;
            }
        } else if (detail) {
            *detail = L"Teststrom ließ sich nicht starten";
        }
    }
    if (mix) CoTaskMemFree(mix);
    release(render);
    release(client);
    release(device);
    release(enumerator);
    return result;
}

} // namespace cnx
