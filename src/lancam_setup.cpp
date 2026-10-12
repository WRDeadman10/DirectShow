#include <windows.h>
#include <string>
#include <iostream>
#include "../include/config_manager.h"

// Parse lancam://stream?room=123&pin=456&host=127.0.0.1&port=7777
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

    if (cmdLine.find(L"lancam://") != std::wstring::npos) {
        // Deeplink triggered!
        ParseUriAndSaveSession(cmdLine);

        // Notify user
        LanCamSession s = LoadLanCamSession();
        std::wstring msg = L"LAN Phone Camera linked to Room: " + s.roomId + L"\nOpen Windows Camera or any app to view!";
        MessageBoxW(NULL, msg.c_str(), L"LAN Camera Linked", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Default execution: Install & Register Filter
    RegisterProtocolAndFilter(szExePath, dllPath);
    MessageBoxW(
        NULL,
        L"LAN Phone Camera Driver installed and registered successfully!\n\n"
        L"You can now click 'Set as Webcam' in the web viewer to tune the camera to any room.",
        L"LAN Camera Driver Setup",
        MB_OK | MB_ICONINFORMATION
    );

    return 0;
}
