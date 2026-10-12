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

void RegisterProtocolAndFilter(const std::wstring& exePath, const std::wstring& dllPath) {
    // 1. Register lancam:// protocol handler
    HKEY hKey = NULL;
    RegCreateKeyExW(HKEY_CLASSES_ROOT, L"lancam", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"URL:LAN Camera Protocol", sizeof(L"URL:LAN Camera Protocol"));
    RegSetValueExW(hKey, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", sizeof(L""));
    RegCloseKey(hKey);

    std::wstring cmdKeyPath = L"lancam\\shell\\open\\command";
    RegCreateKeyExW(HKEY_CLASSES_ROOT, cmdKeyPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    std::wstring cmdVal = L"\"" + exePath + L"\" \"%1\"";
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)cmdVal.c_str(), (DWORD)(cmdVal.length() + 1) * sizeof(wchar_t));
    RegCloseKey(hKey);

    // 2. Register DirectShow Filter via regsvr32
    std::wstring regCmd = L"regsvr32.exe /s \"" + dllPath + L"\"";
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
