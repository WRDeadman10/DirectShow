#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <iostream>
#include <thread>
#include <vector>
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

void RegisterProtocolAndFilter(const std::wstring& exePath, const std::wstring& dllPath) {
    // 1. Copy to Program Files so Windows UWP Camera / FrameServer service has access
    std::wstring installedDllPath = dllPath;
    std::wstring installedExePath = exePath;

    wchar_t progFiles[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles))) {
        std::wstring destDir = std::wstring(progFiles) + L"\\LanCamera";
        CreateDirectoryW(destDir.c_str(), NULL);

        std::wstring destDll = destDir + L"\\LanCamFilter.dll";
        if (CopyFileW(dllPath.c_str(), destDll.c_str(), FALSE)) {
            installedDllPath = destDll;
        }

        std::wstring destExe = destDir + L"\\lancam-setup.exe";
        if (CopyFileW(exePath.c_str(), destExe.c_str(), FALSE)) {
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

    // If launched with --bridge, run the shared memory bridge in background
    if (cmdLine.find(L"--bridge") != std::wstring::npos) {
        RunShmBridgeServer();
        return 0;
    }

    if (cmdLine.find(L"lancam://") != std::wstring::npos) {
        // Deeplink triggered!
        ParseUriAndSaveSession(cmdLine);

        // Automatically open the native Windows Camera app
        ShellExecuteW(NULL, L"open", L"microsoft.windows.camera:", NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }

    // Default execution: Install & Register Filter
    // Check elevation to ensure system-wide registration for Windows Camera app
    if (cmdLine.empty()) {
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

        if (!isElevated) {
            SHELLEXECUTEINFOW sei;
            ZeroMemory(&sei, sizeof(sei));
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"runas";
            sei.lpFile = szExePath;
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) {
                return 0; // Elevated installer launched
            }
        }
    }

    RegisterProtocolAndFilter(szExePath, dllPath);
    MessageBoxW(
        NULL,
        L"LAN Phone Camera Driver installed and registered successfully!\n\n"
        L"You can now click 'Set as Webcam' in the web viewer to tune the camera to any room via WebRTC.",
        L"LAN Camera Driver Setup",
        MB_OK | MB_ICONINFORMATION
    );

    return 0;
}
