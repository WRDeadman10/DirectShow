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

static HRESULT CreateMfCamera(IMFVirtualCamera** ppCamera) {
    return MFCreateVirtualCamera(
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
    }

    MFShutdown();
    return hr;
}

static HRESULT RemoveMfVirtualCamera() {
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

static HRESULT g_mfInstallResult = E_FAIL;
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
    _wsystem(regCmd.c_str());

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

    // --remove-mf: delete the Media Foundation virtual camera and its COM registration (needs elevation)
    if (cmdLine.find(L"--remove-mf") != std::wstring::npos) {
        if (!IsProcessElevated()) {
            if (!RelaunchElevated(szExePath, L"--remove-mf")) {
                MessageBoxW(NULL, L"Administrator permission is required to remove the camera.", L"LAN Camera Driver Setup", MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        HRESULT removeHr = RemoveMfVirtualCamera();
        wchar_t text[200];
        swprintf_s(text, L"LAN Phone Camera (Media Foundation) removal finished: 0x%08lx", (unsigned long)removeHr);
        MessageBoxW(NULL, text, L"LAN Camera Driver Setup", MB_OK | MB_ICONINFORMATION);
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

    RegisterProtocolAndFilter(szExePath, dllPath);

    std::wstring summary = L"LAN Phone Camera driver installed.\n\n";
    summary += L"- DirectShow camera (OBS, Zoom, Chrome, ffmpeg): registered\n";
    if (SUCCEEDED(g_mfInstallResult)) {
        summary += L"- Media Foundation camera (Windows Camera app, Teams, Unity): registered\n\n";
        summary += L"Reopen any camera app. Click 'Set as Webcam' in the web viewer to choose the room.";
    } else {
        wchar_t detail[200];
        swprintf_s(detail, L"- Media Foundation camera: FAILED (0x%08lx)\n  Details: %%ProgramData%%\\LanCamera\\setup.log\n\n",
                   (unsigned long)g_mfInstallResult);
        summary += detail;
        summary += L"The DirectShow camera still works. The Windows Camera app will not list the camera.";
    }
    SetupLog(L"install finished: MF result 0x%08lx", (unsigned long)g_mfInstallResult);
    if (!silent) {
        MessageBoxW(NULL, summary.c_str(), L"LAN Camera Driver Setup", MB_OK | (SUCCEEDED(g_mfInstallResult) ? MB_ICONINFORMATION : MB_ICONWARNING));
    }

    return 0;
}
