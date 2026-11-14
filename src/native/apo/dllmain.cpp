// COM-Rahmen des Audio-Moduls: Klassenfabrik, Registrierung, DLL-Einstiegspunkte.
#include "CompressorApo.h"

#include <new>
#include <string>

static HINSTANCE g_instance = nullptr;

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

namespace {

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // statisches Objekt
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        // Aggregiert (so erzeugt die Audio-Engine Effekt-Module) darf nur IUnknown verlangt werden.
        if (outer && riid != IID_IUnknown) return E_INVALIDARG;
        CompressorApo* apo = new (std::nothrow) CompressorApo(outer);
        if (!apo) return E_OUTOFMEMORY;
        const HRESULT hr = apo->NonDelegatingQueryInterface(riid, ppv);
        apo->NonDelegatingRelease();
        Trace(cnx::kTraceCreate, hr, outer ? 1 : 0, 0, &riid);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_lockCount); else InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring clsidKey() { return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + cnx::kApoClsidString; }

LSTATUS setString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

} // namespace

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    const HRESULT hr = rclsid != CLSID_CompressorNextApo ? CLASS_E_CLASSNOTAVAILABLE : g_factory.QueryInterface(riid, ppv);
    Trace(cnx::kTraceGetClassObject, hr, 0, 0, &rclsid);
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t module[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_instance, module, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    HKEY key = nullptr;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKey().c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = setString(key, nullptr, L"Compressor NEXT");
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (clsidKey() + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = setString(key, nullptr, module);
    if (rc == ERROR_SUCCESS) rc = setString(key, L"ThreadingModel", L"Both");
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);

    return RegisterAPO(GetCompressorApoRegProperties());
}

STDAPI DllUnregisterServer() {
    const HRESULT hr = UnregisterAPO(CLSID_CompressorNextApo);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey().c_str());
    return hr;
}
