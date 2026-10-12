#pragma once

#include <windows.h>
#include <string>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

struct LanCamSession {
    std::wstring host = L"127.0.0.1";
    int port = 7777;
    std::wstring roomId = L"123";
    std::wstring pin = L"123456";
    bool valid = false;
};

inline std::wstring GetLanCamConfigPath() {
    PWSTR localAppDataPath = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppDataPath))) {
        std::wstring fullPath = localAppDataPath;
        CoTaskMemFree(localAppDataPath);
        fullPath += L"\\LanCamera";
        CreateDirectoryW(fullPath.c_str(), NULL);
        fullPath += L"\\session.ini";
        return fullPath;
    }
    return L"";
}

inline bool SaveLanCamSession(const LanCamSession& session) {
    std::wstring path = GetLanCamConfigPath();
    if (path.empty()) return false;

    WritePrivateProfileStringW(L"Session", L"Host", session.host.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Session", L"Port", std::to_wstring(session.port).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Session", L"RoomId", session.roomId.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Session", L"Pin", session.pin.c_str(), path.c_str());
    return true;
}

inline LanCamSession LoadLanCamSession() {
    LanCamSession session;
    std::wstring path = GetLanCamConfigPath();
    if (path.empty()) return session;

    wchar_t buf[256];
    GetPrivateProfileStringW(L"Session", L"Host", L"127.0.0.1", buf, 256, path.c_str());
    session.host = buf;

    session.port = GetPrivateProfileIntW(L"Session", L"Port", 7777, path.c_str());

    GetPrivateProfileStringW(L"Session", L"RoomId", L"123", buf, 256, path.c_str());
    session.roomId = buf;

    GetPrivateProfileStringW(L"Session", L"Pin", L"123456", buf, 256, path.c_str());
    session.pin = buf;

    session.valid = (!session.roomId.empty());
    return session;
}
