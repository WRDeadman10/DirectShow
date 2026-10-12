#pragma once

#include <windows.h>
#include <string>
#include <vector>

#define LANCAM_SHM_NAME L"Local\\LanCamSharedMemory_Video"
#define LANCAM_SHM_MUTEX L"Local\\LanCamSharedMemory_Mutex"
#define LANCAM_SHM_HEADER_MAGIC 0x4C43414D // "LCAM"

#pragma pack(push, 1)
struct LanCamShmHeader {
    DWORD magic;          // 0x4C43414D
    DWORD version;        // 1
    DWORD width;          // e.g. 1280
    DWORD height;         // e.g. 720
    DWORD format;         // 1 = RGB24
    DWORD dataSize;       // width * height * 3
    ULONGLONG frameIndex; // monotonic counter
    ULONGLONG timestamp;  // epoch ms
};
#pragma pack(pop)

// Max supported resolution: 1920x1080x3 bytes + 64 bytes header (~6.2 MB)
#define LANCAM_SHM_MAX_SIZE (sizeof(LanCamShmHeader) + (1920 * 1080 * 3))

class LanCamShmReader {
public:
    LanCamShmReader() : m_hMapFile(NULL), m_hMutex(NULL), m_pBuf(NULL) {}

    ~LanCamShmReader() {
        Close();
    }

    bool Open() {
        if (m_pBuf) return true;

        m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, LANCAM_SHM_NAME);
        if (!m_hMapFile) return false;

        m_pBuf = MapViewOfFile(m_hMapFile, FILE_MAP_READ, 0, 0, LANCAM_SHM_MAX_SIZE);
        if (!m_pBuf) {
            CloseHandle(m_hMapFile);
            m_hMapFile = NULL;
            return false;
        }

        m_hMutex = OpenMutexW(SYNCHRONIZE, FALSE, LANCAM_SHM_MUTEX);
        return true;
    }

    void Close() {
        if (m_pBuf) {
            UnmapViewOfFile(m_pBuf);
            m_pBuf = NULL;
        }
        if (m_hMapFile) {
            CloseHandle(m_hMapFile);
            m_hMapFile = NULL;
        }
        if (m_hMutex) {
            CloseHandle(m_hMutex);
            m_hMutex = NULL;
        }
    }

    bool ReadLatestFrame(std::vector<BYTE>& outRgb, int& outWidth, int& outHeight, ULONGLONG& inOutFrameIndex) {
        if (!m_pBuf && !Open()) {
            return false;
        }

        if (m_hMutex) {
            WaitForSingleObject(m_hMutex, 15);
        }

        const LanCamShmHeader* pHeader = reinterpret_cast<const LanCamShmHeader*>(m_pBuf);
        if (pHeader->magic != LANCAM_SHM_HEADER_MAGIC || pHeader->dataSize == 0) {
            if (m_hMutex) ReleaseMutex(m_hMutex);
            return false;
        }

        // Check if frame is fresh
        if (pHeader->frameIndex == inOutFrameIndex) {
            if (m_hMutex) ReleaseMutex(m_hMutex);
            return false; // same frame
        }

        outWidth = pHeader->width;
        outHeight = pHeader->height;
        inOutFrameIndex = pHeader->frameIndex;

        DWORD copySize = pHeader->dataSize;
        if (outRgb.size() != copySize) {
            outRgb.resize(copySize);
        }

        const BYTE* pPixelData = reinterpret_cast<const BYTE*>(m_pBuf) + sizeof(LanCamShmHeader);
        CopyMemory(outRgb.data(), pPixelData, copySize);

        if (m_hMutex) {
            ReleaseMutex(m_hMutex);
        }
        return true;
    }

private:
    HANDLE m_hMapFile;
    HANDLE m_hMutex;
    LPVOID m_pBuf;
};

class LanCamShmWriter {
public:
    LanCamShmWriter() : m_hMapFile(NULL), m_hMutex(NULL), m_pBuf(NULL), m_frameCounter(0) {}

    ~LanCamShmWriter() {
        Close();
    }

    bool Create() {
        if (m_pBuf) return true;

        m_hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            LANCAM_SHM_MAX_SIZE,
            LANCAM_SHM_NAME
        );
        if (!m_hMapFile) return false;

        m_pBuf = MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, LANCAM_SHM_MAX_SIZE);
        if (!m_pBuf) {
            CloseHandle(m_hMapFile);
            m_hMapFile = NULL;
            return false;
        }

        m_hMutex = CreateMutexW(NULL, FALSE, LANCAM_SHM_MUTEX);
        return true;
    }

    void Close() {
        if (m_pBuf) {
            UnmapViewOfFile(m_pBuf);
            m_pBuf = NULL;
        }
        if (m_hMapFile) {
            CloseHandle(m_hMapFile);
            m_hMapFile = NULL;
        }
        if (m_hMutex) {
            CloseHandle(m_hMutex);
            m_hMutex = NULL;
        }
    }

    bool WriteFrame(int width, int height, const BYTE* pRgbData, DWORD dataSize) {
        if (!m_pBuf && !Create()) return false;
        if (dataSize == 0 || !pRgbData) return false;

        if (m_hMutex) {
            WaitForSingleObject(m_hMutex, 20);
        }

        LanCamShmHeader* pHeader = reinterpret_cast<LanCamShmHeader*>(m_pBuf);
        pHeader->magic = LANCAM_SHM_HEADER_MAGIC;
        pHeader->version = 1;
        pHeader->width = width;
        pHeader->height = height;
        pHeader->format = 1; // RGB24
        pHeader->dataSize = dataSize;
        pHeader->frameIndex = ++m_frameCounter;
        pHeader->timestamp = GetTickCount64();

        BYTE* pPixelDest = reinterpret_cast<BYTE*>(m_pBuf) + sizeof(LanCamShmHeader);
        CopyMemory(pPixelDest, pRgbData, dataSize);

        if (m_hMutex) {
            ReleaseMutex(m_hMutex);
        }
        return true;
    }

private:
    HANDLE m_hMapFile;
    HANDLE m_hMutex;
    LPVOID m_pBuf;
    ULONGLONG m_frameCounter;
};

