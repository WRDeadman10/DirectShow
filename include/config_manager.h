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

// Machine-wide copy used by the Media Foundation virtual camera, which runs inside the Windows Frame Server
// service (LocalService) and therefore cannot see %LOCALAPPDATA% of the logged-in user.
inline std::wstring GetLanCamMachineDir(bool create) {
    PWSTR programDataPath = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, NULL, &programDataPath))) {
        std::wstring fullPath = programDataPath;
        CoTaskMemFree(programDataPath);
        fullPath += L"\\LanCamera";
        if (create) CreateDirectoryW(fullPath.c_str(), NULL);
        return fullPath;
    }
    return L"";
}

inline std::wstring GetLanCamMachineConfigPath(bool create) {
    std::wstring dir = GetLanCamMachineDir(create);
    return dir.empty() ? L"" : dir + L"\\session.ini";
}

inline void WriteSessionFile(const LanCamSession& session, const std::wstring& path) {
    WritePrivateProfileStringW(L"Session", L"Host", session.host.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Session", L"Port", std::to_wstring(session.port).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Session", L"RoomId", session.roomId.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Session", L"Pin", session.pin.c_str(), path.c_str());
}

inline bool SaveLanCamSession(const LanCamSession& session) {
    std::wstring path = GetLanCamConfigPath();
    if (path.empty()) return false;

    WriteSessionFile(session, path);

    // Best effort: the machine-wide copy is writable by users once the installer has granted access.
    std::wstring machinePath = GetLanCamMachineConfigPath(false);
    if (!machinePath.empty() && GetFileAttributesW(GetLanCamMachineDir(false).c_str()) != INVALID_FILE_ATTRIBUTES) {
        WriteSessionFile(session, machinePath);
    }
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

// Used by the Media Foundation source: machine-wide file first, per-user file as a fallback.
inline LanCamSession LoadLanCamSessionMachine() {
    std::wstring path = GetLanCamMachineConfigPath(false);
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return LoadLanCamSession();
    }

    LanCamSession session;
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
