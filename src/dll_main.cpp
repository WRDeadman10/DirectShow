#include "lancam_filter.h"
#include <olectl.h>

class LanCamClassFactory : public IClassFactory {
public:
    LanCamClassFactory() : m_cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG u = InterlockedDecrement(&m_cRef);
        if (u == 0) delete this;
        return u;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        LanCamFilter* pFilter = new LanCamFilter();
        HRESULT hr = pFilter->QueryInterface(riid, ppv);
        pFilter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override { return S_OK; }

private:
    LONG m_cRef;
};

static HMODULE g_hModule = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_LanCamFilter) return CLASS_E_CLASSNOTAVAILABLE;
    LanCamClassFactory* pFactory = new LanCamClassFactory();
    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return S_OK;
}

// -------------------------------------------------------------
// COM & DirectShow VideoInputDeviceCategory Registration
// -------------------------------------------------------------
STDAPI DllRegisterServer() {
    wchar_t szModulePath[MAX_PATH];
    GetModuleFileNameW(g_hModule, szModulePath, MAX_PATH);

    HKEY hKey = NULL;
    // 1. Register CLSID in InprocServer32
    std::wstring clsidPath = L"CLSID\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}";
    RegCreateKeyExW(HKEY_CLASSES_ROOT, clsidPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"LAN Phone Camera", sizeof(L"LAN Phone Camera"));
    RegCloseKey(hKey);

    std::wstring inprocPath = clsidPath + L"\\InprocServer32";
    RegCreateKeyExW(HKEY_CLASSES_ROOT, inprocPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)szModulePath, (DWORD)(wcslen(szModulePath) + 1) * sizeof(wchar_t));
    RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Both", sizeof(L"Both"));
    RegCloseKey(hKey);

    // 2. Register under DirectShow Video Input Category: CLSID_VideoInputDeviceCategory {860BB310-5D01-11d0-BD3B-00A0C911CE86}
    std::wstring catPath = L"CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}";
    RegCreateKeyExW(HKEY_CLASSES_ROOT, catPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (const BYTE*)L"LAN Phone Camera", sizeof(L"LAN Phone Camera"));
    RegSetValueExW(hKey, L"CLSID", 0, REG_SZ, (const BYTE*)L"{708B7B2B-191B-4E3F-8C7E-F037A32F5112}", sizeof(L"{708B7B2B-191B-4E3F-8C7E-F037A32F5112}"));
    RegCloseKey(hKey);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}\\InprocServer32");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}");
    return S_OK;
}
