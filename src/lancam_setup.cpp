#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <iostream>
#include <thread>
#include <vector>
#include <cstdarg>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include "../include/config_manager.h"
#include "../include/shm_bridge.h"

#pragma comment(lib, "ws2_32.lib")
bool ParseUriAndSaveSession(const std::wstring& uri) {
    size_t queryPos = uri.find(L'?');
    if (queryPos == std::wstring::npos) {
        return false;
    }

    LanCamSession session;
    std::wstring query = uri.substr(queryPos + 1);

    size_t start = 0;
    while (start < query.length()) {
        size_t end = query.find(L'&', start);
        if (end == std::wstring::npos) end = query.length();

        std::wstring pair = query.substr(start, end - start);
        size_t eqPos = pair.find(L'=');
        if (eqPos != std::wstring::npos) {
            std::wstring key = pair.substr(0, eqPos);
            std::wstring val = pair.substr(eqPos + 1);

            while (!val.empty() && (val.back() == L'"' || val.back() == L'\'' || val.back() == L'/' || val.back() == L'\\' || val.back() == L' ')) {
                val.pop_back();
            }

            if (_wcsicmp(key.c_str(), L"room") == 0) {
                session.roomId = val;
            } else if (_wcsicmp(key.c_str(), L"pin") == 0) {
                session.pin = val;
            } else if (_wcsicmp(key.c_str(), L"host") == 0) {
                session.host = val;
            } else if (_wcsicmp(key.c_str(), L"port") == 0) {
                session.port = _wtoi(val.c_str());
            }
        }
        start = end + 1;
    }

    return SaveLanCamSession(session);
}

static const BYTE kFilterData[] = {
    0x02, 0x00, 0x00, 0x00, // dwVersion = 2
    0x00, 0x00, 0x20, 0x00, // dwMerit = 0x00200000
    0x01, 0x00, 0x00, 0x00, // cPins = 1
    0x00, 0x00, 0x00, 0x00, // cInstance = 0
    0x30, 0x70, 0x69, 0x33, // '3ip0' pin identifier
    0x08, 0x00, 0x00, 0x00, // dwFlags = REG_PINFLAG_B_OUTPUT (8)
    0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, // nMediaTypes = 1
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x30, 0x74, 0x79, 0x33, // '3yt0' mediatype identifier
    0x00, 0x00, 0x00, 0x00,
    0x38, 0x00, 0x00, 0x00, // offset to major type (56)
    0x48, 0x00, 0x00, 0x00, // offset to sub type (72)
    // MEDIATYPE_Video: {73646976-0000-0010-8000-00AA00389B71}
    0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    // MEDIASUBTYPE_None: {00000000-0000-0000-0000-000000000000}
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void RegisterKeysInHive(HKEY rootKey, const std::wstring& exePath, const std::wstring& dllPath) {
    HKEY hKey = NULL;

    // 1. Register lancam:// protocol handler
    if (RegCreateKeyExW(rootKey, L"lancam", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"URL:LAN Camera Protocol", sizeof(L"URL:LAN Camera Protocol"));
        RegSetValueExW(hKey, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", sizeof(L""));
        RegCloseKey(hKey);
    }

    std::wstring cmdKeyPath = L"lancam\\shell\\open\\command";
    if (RegCreateKeyExW(rootKey, cmdKeyPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring cmdVal = L"\"" + exePath + L"\" \"%1\"";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)cmdVal.c_str(), (DWORD)(cmdVal.length() + 1) * sizeof(wchar_t));
        RegCloseKey(hKey);
    }

    // 2. Register CLSID InprocServer32
    std::wstring clsidPath = L"CLSID\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}";
    if (RegCreateKeyExW(rootKey, clsidPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"LAN Phone Camera", sizeof(L"LAN Phone Camera"));
        RegCloseKey(hKey);
    }

    std::wstring inprocPath = clsidPath + L"\\InprocServer32";
    if (RegCreateKeyExW(rootKey, inprocPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)dllPath.c_str(), (DWORD)(dllPath.length() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Both", sizeof(L"Both"));
        RegCloseKey(hKey);
    }

    // 3. Register under DirectShow Video Input Category with FilterData
    std::wstring catPath = L"CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}";
    if (RegCreateKeyExW(rootKey, catPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (const BYTE*)L"LAN Phone Camera", sizeof(L"LAN Phone Camera"));
        RegSetValueExW(hKey, L"CLSID", 0, REG_SZ, (const BYTE*)L"{708B7B2B-191B-4E3F-8C7E-F037A32F5112}", sizeof(L"{708B7B2B-191B-4E3F-8C7E-F037A32F5112}"));
        RegSetValueExW(hKey, L"FilterData", 0, REG_BINARY, kFilterData, sizeof(kFilterData));
        RegCloseKey(hKey);
    }
}

// ---------------------------------------------------------------------------------------------
// Media Foundation virtual camera (visible to the Windows Camera app and other Media Foundation / WinRT apps)
// ---------------------------------------------------------------------------------------------
static const wchar_t kMfSourceClsid[] = L"{B8D0A5E7-6C31-4F0A-9E8B-2D4F7A1C5E93}";
static const wchar_t kMfCameraName[] = L"LAN Phone Camera";
// KSCATEGORY_VIDEO_CAMERA {E5323777-F976-4f5b-9B55-B94699C46E44}
static const GUID kKsCategoryVideoCamera =
    { 0xe5323777, 0xf976, 0x4f5b, { 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44 } };

static bool IsProcessElevated() {
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION el;
        DWORD cb = sizeof(el);
        if (GetTokenInformation(hToken, TokenElevation, &el, sizeof(el), &cb)) {
            isElevated = (el.TokenIsElevated != 0);
        }
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

static void SetupLog(const wchar_t* format, ...) {
    std::wstring dir = GetLanCamMachineDir(true);
    if (dir.empty()) return;
    std::wstring path = dir + L"\\setup.log";

    wchar_t message[600];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[700];
    int len = _snwprintf_s(line, _countof(line), _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d %s\r\n",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, message);
    if (len <= 0) return;

    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        // UTF-16 text; write a BOM when creating the file
        if (GetFileSize(hFile, NULL) == 0) {
            WORD bom = 0xFEFF;
            DWORD w = 0;
            WriteFile(hFile, &bom, sizeof(bom), &w, NULL);
        }
        DWORD written = 0;
        WriteFile(hFile, line, (DWORD)(len * sizeof(wchar_t)), &written, NULL);
        CloseHandle(hFile);
    }
}

// The Frame Server (LocalService) reads the session and writes the log under %ProgramData%\LanCamera, and the
// logged-in user's lancam:// handler writes the session there, so both need modify access.
static void PrepareMachineDirectory() {
    std::wstring dir = GetLanCamMachineDir(true);
    if (dir.empty()) return;
    std::wstring cmd = L"icacls.exe \"" + dir + L"\" /grant *S-1-5-32-545:(OI)(CI)M *S-1-5-19:(OI)(CI)M /Q >nul 2>&1";
    _wsystem(cmd.c_str());
}

static bool SetRegString(HKEY root, const std::wstring& subKey, const wchar_t* valueName, const std::wstring& data) {
    HKEY hKey = NULL;
    LSTATUS status = RegCreateKeyExW(root, subKey.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (status != ERROR_SUCCESS) {
        SetupLog(L"MF: RegCreateKeyEx(%s) failed: %ld", subKey.c_str(), (long)status);
        return false;
    }
    status = RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE*)data.c_str(), (DWORD)((data.length() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    if (status != ERROR_SUCCESS) {
        SetupLog(L"MF: RegSetValueEx(%s) failed: %ld", subKey.c_str(), (long)status);
        return false;
    }
    return true;
}

// HKEY_CLASSES_ROOT is the merged classes view: an elevated process writes into HKLM\SOFTWARE\Classes, which is
// where the Frame Server (LocalService) looks the class up. (Opening HKLM\SOFTWARE\Classes directly with write
// access is denied on some machines even when elevated.)
static void RegisterMfSourceClass(const std::wstring& dllPath) {
    SetupLog(L"MF: registering source class (elevated=%d) -> %s", IsProcessElevated() ? 1 : 0, dllPath.c_str());

    std::wstring clsidPath = std::wstring(L"CLSID\\") + kMfSourceClsid;
    SetRegString(HKEY_CLASSES_ROOT, clsidPath, NULL, kMfCameraName);
    std::wstring inprocPath = clsidPath + L"\\InprocServer32";
    SetRegString(HKEY_CLASSES_ROOT, inprocPath, NULL, dllPath);
    SetRegString(HKEY_CLASSES_ROOT, inprocPath, L"ThreadingModel", L"Both");
}

static void UnregisterMfSourceClass() {
    std::wstring clsidPath = std::wstring(L"CLSID\\") + kMfSourceClsid;
    LSTATUS status = RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidPath.c_str());
    SetupLog(L"MF: RegDeleteTree -> %ld", (long)status);
}

typedef HRESULT (STDAPICALLTYPE *PFN_MFCreateVirtualCamera)(
    MFVirtualCameraType type,
    MFVirtualCameraLifetime lifetime,
    MFVirtualCameraAccess access,
    LPCWSTR pwzFriendlyName,
    LPCWSTR pwzCLSID,
    const GUID* pCategories,
    ULONG cCategoryCount,
    IMFVirtualCamera** ppVirtualCamera
);

static PFN_MFCreateVirtualCamera GetMFCreateVirtualCamera() {
    static PFN_MFCreateVirtualCamera s_pfn = NULL;
    static bool s_probed = false;
    if (!s_probed) {
        s_probed = true;
        // MFCreateVirtualCamera (Windows 11) is exported by mfsensorgroup.dll, NOT by mfplat.dll. Looking it up in
        // mfplat.dll always fails and made the installer skip the Media Foundation camera on every machine.
        const wchar_t* modules[] = { L"mfsensorgroup.dll", L"mfplat.dll" };
        for (const wchar_t* moduleName : modules) {
            HMODULE hModule = LoadLibraryW(moduleName);
            if (hModule) {
                s_pfn = (PFN_MFCreateVirtualCamera)GetProcAddress(hModule, "MFCreateVirtualCamera");
                if (s_pfn) {
                    SetupLog(L"MF: MFCreateVirtualCamera found in %s", moduleName);
                    break;
                }
            }
        }
        if (!s_pfn) {
            SetupLog(L"MF: MFCreateVirtualCamera not found (Windows 11 build 22000 or newer is required)");
        }
    }
    return s_pfn;
}

// True when the Windows camera stack (what the Camera app uses) lists the virtual camera.
static bool IsMfCameraListed() {
    IMFAttributes* attrs = NULL;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return false;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    bool found = false;
    IMFActivate** devices = NULL;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = NULL;
            UINT32 length = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) && name) {
                if (wcsstr(name, kMfCameraName) != NULL) found = true;
                CoTaskMemFree(name);
            }
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    attrs->Release();
    return found;
}

static bool g_mfListed = false;

static HRESULT CreateMfCamera(IMFVirtualCamera** ppCamera) {
    PFN_MFCreateVirtualCamera pfn = GetMFCreateVirtualCamera();
    if (!pfn) {
        SetupLog(L"MF: MFCreateVirtualCamera not available on this Windows version (Windows 11 build 22000+ required)");
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    return pfn(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers,
        kMfCameraName,
        kMfSourceClsid,
        &kKsCategoryVideoCamera,
        1,
        ppCamera);
}

// Returns S_OK when the camera was created and started. Needs an elevated process.
static HRESULT InstallMfVirtualCamera(const std::wstring& dllPath) {
    PFN_MFCreateVirtualCamera pfn = GetMFCreateVirtualCamera();
    if (!pfn) {
        SetupLog(L"MF: MFCreateVirtualCamera not supported on this Windows version; skipping Media Foundation virtual camera registration (DirectShow virtual camera will be used).");
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    PrepareMachineDirectory();
    RegisterMfSourceClass(dllPath);

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        SetupLog(L"MF: MFStartup failed 0x%08lx", (unsigned long)hr);
        return hr;
    }

    // Drop any previous registration so that reinstalling never leaves duplicates.
    IMFVirtualCamera* pOld = NULL;
    if (SUCCEEDED(CreateMfCamera(&pOld)) && pOld) {
        pOld->Remove();
        pOld->Release();
    }

    IMFVirtualCamera* pCamera = NULL;
    hr = CreateMfCamera(&pCamera);
    SetupLog(L"MF: MFCreateVirtualCamera -> 0x%08lx", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = pCamera->Start(NULL);
        SetupLog(L"MF: IMFVirtualCamera::Start -> 0x%08lx", (unsigned long)hr);
        pCamera->Release();

        if (SUCCEEDED(hr)) {
            // Confirm Windows really lists the camera (it can take a moment after Start).
            for (int attempt = 0; attempt < 12 && !g_mfListed; ++attempt) {
                g_mfListed = IsMfCameraListed();
                if (!g_mfListed) Sleep(500);
            }
            SetupLog(L"MF: camera listed by Windows: %d", g_mfListed ? 1 : 0);
        }
    }

    MFShutdown();
    return hr;
}

static HRESULT RemoveMfVirtualCamera() {
    PFN_MFCreateVirtualCamera pfn = GetMFCreateVirtualCamera();
    if (!pfn) {
        UnregisterMfSourceClass();
        return S_OK;
    }

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) return hr;

    IMFVirtualCamera* pCamera = NULL;
    hr = CreateMfCamera(&pCamera);
    if (SUCCEEDED(hr)) {
        hr = pCamera->Remove();
        SetupLog(L"MF: IMFVirtualCamera::Remove -> 0x%08lx", (unsigned long)hr);
        pCamera->Release();
    }
    MFShutdown();
    UnregisterMfSourceClass();
    return hr;
}

// Copies a file. Chromium/Electron apps (Chrome, Claude, VS Code ...) and the Windows Frame Server keep the camera
// DLLs mapped, which blocks overwriting them; Windows does allow renaming a mapped DLL, so the old file is moved
// aside (and removed at the next reboot) and the new one is copied into place.
static bool CopyFileWithRetry(const std::wstring& from, const std::wstring& to) {
    if (_wcsicmp(from.c_str(), to.c_str()) == 0) {
        return true;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            return true;
        }
        Sleep(300);
    }

    DWORD lastError = GetLastError();
    std::wstring aside = to + L".old" + std::to_wstring(GetTickCount());
    if (MoveFileExW(to.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(aside.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            SetupLog(L"copy: replaced in-use file via rename: %s", to.c_str());
            return true;
        }
        lastError = GetLastError();
    }

    SetupLog(L"copy failed: %s -> %s (error %lu)", from.c_str(), to.c_str(), (unsigned long)lastError);
    return false;
}

// Removes everything RegisterProtocolAndFilter and InstallMfVirtualCamera created. Files are deleted by the
// Inno Setup uninstaller afterwards. Needs an elevated process.
static int UninstallCamera() {
    SetupLog(L"uninstall: begin");

    // Media Foundation camera + its COM class
    HRESULT mfHr = RemoveMfVirtualCamera();
    SetupLog(L"uninstall: MF removal -> 0x%08lx", (unsigned long)mfHr);

    // DirectShow filter: unregister through its own entry point, then delete the registry entries the installer wrote
    std::wstring dllPath;
    wchar_t progFiles[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles))) {
        dllPath = std::wstring(progFiles) + L"\\LanCamera\\LanCamFilter.dll";
    }
    if (!dllPath.empty() && GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring unregCmd = L"regsvr32.exe /u /s \"" + dllPath + L"\"";
        int rc = _wsystem(unregCmd.c_str());
        SetupLog(L"uninstall: regsvr32 /u exit code %d", rc);
    }

    const wchar_t* keys[] = {
        L"CLSID\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}",
        L"CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\{708B7B2B-191B-4E3F-8C7E-F037A32F5112}",
        L"lancam"
    };
    HKEY hkcuClasses = NULL;
    bool haveHkcu = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_WRITE, &hkcuClasses) == ERROR_SUCCESS;
    for (const wchar_t* key : keys) {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
        if (haveHkcu) RegDeleteTreeW(hkcuClasses, key);
    }
    if (haveHkcu) RegCloseKey(hkcuClasses);

    // Let the Frame Server release the DLLs so the uninstaller can delete them
    _wsystem(L"net stop FrameServer >nul 2>&1");
    SetupLog(L"uninstall: done");
    return 0;
}

static HRESULT g_mfInstallResult = E_FAIL;
static int g_dsRegisterResult = -1;
static bool g_mfDllFound = false;

void RegisterProtocolAndFilter(const std::wstring& exePath, const std::wstring& dllPath) {
    // The Windows Frame Server keeps LanCamMfSource.dll loaded after a camera app used it, which blocks replacing
    // the file. It restarts on demand, so stop it first (best effort).
    _wsystem(L"net stop FrameServer >nul 2>&1");

    // 1. Copy to Program Files so Windows UWP Camera / FrameServer service has access
    std::wstring installedDllPath = dllPath;
    std::wstring installedExePath = exePath;

    wchar_t progFiles[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles))) {
        std::wstring destDir = std::wstring(progFiles) + L"\\LanCamera";
        CreateDirectoryW(destDir.c_str(), NULL);

        std::wstring destDll = destDir + L"\\LanCamFilter.dll";
        if (CopyFileWithRetry(dllPath, destDll)) {
            installedDllPath = destDll;
        }

        std::wstring destExe = destDir + L"\\lancam-setup.exe";
        if (CopyFileWithRetry(exePath, destExe)) {
            installedExePath = destExe;
        }
    }

    // 2. Register directly in HKEY_LOCAL_MACHINE\SOFTWARE\Classes
    HKEY hklmSoftware = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0, KEY_WRITE, &hklmSoftware) == ERROR_SUCCESS) {
        RegisterKeysInHive(hklmSoftware, installedExePath, installedDllPath);
        RegCloseKey(hklmSoftware);
    }

    // 3. Register in HKEY_CLASSES_ROOT
    RegisterKeysInHive(HKEY_CLASSES_ROOT, installedExePath, installedDllPath);

    // 4. Register per-user in HKCU
    HKEY hkcuClasses = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_WRITE, &hkcuClasses) == ERROR_SUCCESS) {
        RegisterKeysInHive(hkcuClasses, installedExePath, installedDllPath);
        RegCloseKey(hkcuClasses);
    }

    // 5. Call regsvr32
    std::wstring regCmd = L"regsvr32.exe /s \"" + installedDllPath + L"\"";
    g_dsRegisterResult = _wsystem(regCmd.c_str());
    SetupLog(L"DirectShow: regsvr32 exit code %d", g_dsRegisterResult);

    // 6. Media Foundation virtual camera (Windows Camera app, Teams, Chrome, Unity ...)
    std::wstring sourceDir = dllPath.substr(0, dllPath.find_last_of(L"\\/"));
    std::wstring mfSourceDll = sourceDir + L"\\LanCamMfSource.dll";
    g_mfDllFound = GetFileAttributesW(mfSourceDll.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_mfDllFound) {
        std::wstring mfInstalledDll = mfSourceDll;
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles))) {
            std::wstring destMf = std::wstring(progFiles) + L"\\LanCamera\\LanCamMfSource.dll";
            if (CopyFileWithRetry(mfSourceDll, destMf)) {
                mfInstalledDll = destMf;
            }
        }
        g_mfInstallResult = InstallMfVirtualCamera(mfInstalledDll);
    } else {
        SetupLog(L"MF: LanCamMfSource.dll not found next to the installer");
    }
}

// Background thread listening on 127.0.0.1:7779 for raw WebRTC frames from browser
static void RunShmBridgeServer() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(7779);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listenSock, 1) != 0) {
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    LanCamShmWriter shmWriter;
    shmWriter.Create();

    while (true) {
        SOCKET clientSock = accept(listenSock, NULL, NULL);
        if (clientSock == INVALID_SOCKET) break;

        // Header: uint32 width, uint32 height, uint32 dataSize
        uint32_t header[3];
        std::vector<BYTE> frameBuffer;

        while (true) {
            int recvd = 0;
            while (recvd < (int)sizeof(header)) {
                int n = recv(clientSock, (char*)header + recvd, (int)sizeof(header) - recvd, 0);
                if (n <= 0) goto client_disconnected;
                recvd += n;
            }

            uint32_t width = header[0];
            uint32_t height = header[1];
            uint32_t dataSize = header[2];

            if (dataSize == 0 || dataSize > (1920 * 1080 * 3)) {
                goto client_disconnected;
            }

            if (frameBuffer.size() < dataSize) {
                frameBuffer.resize(dataSize);
            }

            uint32_t bytesGot = 0;
            while (bytesGot < dataSize) {
                int n = recv(clientSock, (char*)frameBuffer.data() + bytesGot, dataSize - bytesGot, 0);
                if (n <= 0) goto client_disconnected;
                bytesGot += n;
            }

            shmWriter.WriteFrame(width, height, frameBuffer.data(), dataSize);
        }

client_disconnected:
        closesocket(clientSock);
    }

    closesocket(listenSock);
    WSACleanup();
}

// Relaunches this exe through UAC. Returns true when the elevated copy was started.
static bool RelaunchElevated(const wchar_t* exePath, const wchar_t* parameters) {
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = parameters;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    wchar_t szExePath[MAX_PATH];
    GetModuleFileNameW(NULL, szExePath, MAX_PATH);

    std::wstring exeDir = szExePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    std::wstring dllPath = exeDir + L"\\LanCamFilter.dll";

    std::wstring cmdLine = pCmdLine ? pCmdLine : L"";

    // --uninstall: remove every registration (used by the Inno Setup uninstaller; needs elevation)
    if (cmdLine.find(L"--uninstall") != std::wstring::npos) {
        if (!IsProcessElevated()) {
            if (!RelaunchElevated(szExePath, cmdLine.c_str())) {
                MessageBoxW(NULL, L"Administrator permission is required to remove the camera.", L"LAN Camera Driver Setup", MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        return UninstallCamera();
    }

    // --remove-mf: delete the Media Foundation virtual camera and its COM registration (needs elevation)
    if (cmdLine.find(L"--remove-mf") != std::wstring::npos) {
        if (!IsProcessElevated()) {
            if (!RelaunchElevated(szExePath, cmdLine.c_str())) {
                MessageBoxW(NULL, L"Administrator permission is required to remove the camera.", L"LAN Camera Driver Setup", MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        HRESULT removeHr = RemoveMfVirtualCamera();
        wchar_t text[200];
        swprintf_s(text, L"LAN Phone Camera (Media Foundation) removal finished: 0x%08lx", (unsigned long)removeHr);
        if (cmdLine.find(L"--silent") == std::wstring::npos) {
            MessageBoxW(NULL, text, L"LAN Camera Driver Setup", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    // If launched with --bridge, run the shared memory bridge in background
    if (cmdLine.find(L"--bridge") != std::wstring::npos) {
        RunShmBridgeServer();
        return 0;
    }

    if (cmdLine.find(L"lancam://") != std::wstring::npos) {
        // Deeplink triggered: only remember the room/PIN. This runs as the normal user (no UAC prompt).
        // The Windows Camera app is intentionally NOT launched: it is a UWP/Media Foundation app and cannot
        // see DirectShow virtual cameras, so it would only report "We can't find your camera".
        ParseUriAndSaveSession(cmdLine);
        return 0;
    }

    // Default execution: Install & Register Filter
    // Registration writes to HKLM and Program Files, so only this path needs elevation (the manifest is
    // asInvoker so that deeplink launches and --bridge do not trigger UAC).
    const bool silent = cmdLine.find(L"--silent") != std::wstring::npos;
    if (!IsProcessElevated()) {
        if (!RelaunchElevated(szExePath, cmdLine.empty() ? NULL : cmdLine.c_str())) {
            MessageBoxW(NULL, L"Installation was cancelled: administrator permission is required to register the camera.", L"LAN Camera Driver Setup", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }

    // Fail early and clearly when the driver files are missing (typical when lancam-setup.exe is run from inside the
    // zip without extracting it).
    const std::wstring mfDllPath = exeDir + L"\\LanCamMfSource.dll";
    const bool haveFilterDll = GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool haveMfDll = GetFileAttributesW(mfDllPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!haveFilterDll || !haveMfDll) {
        SetupLog(L"preflight: missing files next to installer (LanCamFilter.dll=%d, LanCamMfSource.dll=%d) in %s",
                 haveFilterDll ? 1 : 0, haveMfDll ? 1 : 0, exeDir.c_str());
        if (!silent) {
            MessageBoxW(NULL,
                L"The driver files were not found next to lancam-setup.exe.\n\n"
                L"Extract the whole zip into a folder first (right-click the zip > Extract All), then run "
                L"lancam-setup.exe from that folder.",
                L"LAN Camera Driver Setup", MB_OK | MB_ICONERROR);
        }
        return 2;
    }

    RegisterProtocolAndFilter(szExePath, dllPath);

    std::wstring summary;
    const bool mfOk = SUCCEEDED(g_mfInstallResult);
    const bool dsOk = (g_dsRegisterResult == 0);

    if (dsOk) {
        summary += L"- DirectShow camera (OBS, Zoom, Chrome, ffmpeg): registered\n";
    } else {
        wchar_t detail[160];
        swprintf_s(detail, L"- DirectShow camera: FAILED to register (regsvr32 exit code %d)\n", g_dsRegisterResult);
        summary += detail;
    }

    if (mfOk && g_mfListed) {
        summary += L"- Windows camera (Camera app, Teams, Zoom, Chrome, Unity): registered and listed by Windows\n";
    } else if (mfOk) {
        summary += L"- Windows camera: registered, but Windows does not list it yet. Restart the PC, or see mfsource.log\n";
    } else if (g_mfInstallResult == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
        summary += L"- Windows camera: NOT available. It needs Windows 11 (build 22000 or newer)\n";
    } else if (!g_mfDllFound) {
        summary += L"- Windows camera: NOT installed. LanCamMfSource.dll was not next to this installer\n";
    } else {
        wchar_t detail[200];
        swprintf_s(detail, L"- Windows camera: FAILED (0x%08lx)\n", (unsigned long)g_mfInstallResult);
        summary += detail;
    }

    const bool allGood = dsOk && mfOk && g_mfListed;
    if (allGood) {
        summary = L"LAN Phone Camera installed.\n\n" + summary + L"\nReopen any camera app, then click 'Set as Webcam' in the web viewer.";
    } else {
        summary = L"LAN Phone Camera installation finished with problems.\n\n" + summary +
                  L"\nDetails are in %ProgramData%\\LanCamera\\setup.log";
    }

    SetupLog(L"install finished: directshow=%d mf=0x%08lx listed=%d", dsOk ? 1 : 0, (unsigned long)g_mfInstallResult, g_mfListed ? 1 : 0);
    if (!silent) {
        MessageBoxW(NULL, summary.c_str(), L"LAN Camera Driver Setup", MB_OK | (allGood ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    return allGood ? 0 : 1;
}
