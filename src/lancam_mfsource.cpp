// LanCamMfSource.dll
// Media Foundation virtual camera source ("LAN Phone Camera"). Unlike the DirectShow filter, a Media Foundation
// source registered through MFCreateVirtualCamera is enumerated by the Windows Frame Server, so it also appears in
// the Windows Camera app, Teams, Zoom (new), Chrome/Edge and any other Media Foundation / WinRT capture consumer.
//
// The Frame Server hosts this DLL (as LocalService). Frames come from the LAN Camera server's MJPEG endpoint
// (http://<host>:<port>/stream/<room>?pin=<pin>), are letterboxed into 1280x720 and delivered as NV12 at 30 fps.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <new>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../include/config_manager.h"
#include "../include/mjpeg_decoder.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// {B8D0A5E7-6C31-4F0A-9E8B-2D4F7A1C5E93}
static const CLSID CLSID_LanCamMfSource =
    { 0xb8d0a5e7, 0x6c31, 0x4f0a, { 0x9e, 0x8b, 0x2d, 0x4f, 0x7a, 0x1c, 0x5e, 0x93 } };

// PINNAME_VIDEO_CAPTURE
static const GUID LanCamPinNameVideoCapture =
    { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };

static const UINT32 kFrameWidth = 1280;
static const UINT32 kFrameHeight = 720;
static const UINT32 kFrameRate = 30;
static const LONGLONG kFrameDuration100ns = 10000000LL / kFrameRate;

static HMODULE g_hModule = NULL;
static std::atomic<LONG> g_objectCount(0);

// ---------------------------------------------------------------------------------------------
// Diagnostics: the source runs inside a service, so a small log file is the only way to see what happened.
// ---------------------------------------------------------------------------------------------
static void LanCamLog(const char* format, ...) {
    std::wstring dir = GetLanCamMachineDir(false);
    if (dir.empty()) {
        return;
    }
    std::wstring path = dir + L"\\mfsource.log";

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 256 * 1024) {
        DeleteFileW(path.c_str());
    }

    char message[512];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[640];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02d:%02d:%02d.%03d [pid %lu] %s\r\n",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), message);
    if (len <= 0) {
        return;
    }

    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, line, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }
}

// ---------------------------------------------------------------------------------------------
// Frame conversion: BGR24 (top-down, 4-byte aligned rows) -> letterboxed NV12 1280x720
// ---------------------------------------------------------------------------------------------
static void FillNv12Black(std::vector<BYTE>& nv12) {
    const size_t ySize = (size_t)kFrameWidth * kFrameHeight;
    nv12.assign(ySize + ySize / 2, 128);
    std::fill(nv12.begin(), nv12.begin() + ySize, (BYTE)16);
}

static void ConvertBgrToNv12Letterbox(const BYTE* bgr, int srcW, int srcH, size_t srcStride, std::vector<BYTE>& nv12) {
    const int dstW = (int)kFrameWidth;
    const int dstH = (int)kFrameHeight;
    const size_t ySize = (size_t)dstW * dstH;
    nv12.assign(ySize + ySize / 2, 128);
    std::fill(nv12.begin(), nv12.begin() + ySize, (BYTE)16);

    if (srcW <= 0 || srcH <= 0) {
        return;
    }

    const double scale = min((double)dstW / srcW, (double)dstH / srcH);
    const int outW = max(2, (int)(srcW * scale) & ~1);
    const int outH = max(2, (int)(srcH * scale) & ~1);
    const int offX = ((dstW - outW) / 2) & ~1;
    const int offY = ((dstH - outH) / 2) & ~1;

    BYTE* yPlane = nv12.data();
    BYTE* uvPlane = nv12.data() + ySize;

    for (int y = 0; y < outH; ++y) {
        const int srcY = min(srcH - 1, (int)((long long)y * srcH / outH));
        const BYTE* srcRow = bgr + (size_t)srcY * srcStride;
        BYTE* yRow = yPlane + (size_t)(offY + y) * dstW + offX;
        const bool evenRow = ((offY + y) & 1) == 0;
        BYTE* uvRow = evenRow ? uvPlane + (size_t)((offY + y) / 2) * dstW + offX : NULL;

        for (int x = 0; x < outW; ++x) {
            const int srcX = min(srcW - 1, (int)((long long)x * srcW / outW));
            const BYTE* p = srcRow + (size_t)srcX * 3;
            const int b = p[0];
            const int g = p[1];
            const int r = p[2];

            yRow[x] = (BYTE)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (uvRow && ((offX + x) & 1) == 0) {
                uvRow[x] = (BYTE)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                uvRow[x + 1] = (BYTE)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
}

static HRESULT CreateVideoMediaType(IMFMediaType** ppType) {
    IMFMediaType* pType = NULL;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return hr;

    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, kFrameWidth, kFrameHeight);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, kFrameRate, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_DEFAULT_STRIDE, kFrameWidth);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_SAMPLE_SIZE, kFrameWidth * kFrameHeight * 3 / 2);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_AVG_BITRATE, kFrameWidth * kFrameHeight * 3 / 2 * 8 * kFrameRate);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (SUCCEEDED(hr)) hr = pType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);

    if (FAILED(hr)) {
        pType->Release();
        return hr;
    }
    *ppType = pType;
    return S_OK;
}

static void LogIid(const char* where, REFIID riid);
static void LogIidFwd(REFIID riid) { LogIid("Source QueryInterface rejected", riid); }

class LanCamMfSource;

// ---------------------------------------------------------------------------------------------
// Media stream: one NV12 video stream. A pacing thread answers RequestSample calls at 30 fps.
// ---------------------------------------------------------------------------------------------
class LanCamMfStream : public IMFMediaStream2 {
public:
    LanCamMfStream(LanCamMfSource* pSource, IMFStreamDescriptor* pDescriptor)
        : m_cRef(1), m_pSource(pSource), m_pDescriptor(pDescriptor), m_pEventQueue(NULL), m_pAttributes(NULL),
          m_running(false), m_shutdown(false), m_state(MF_STREAM_STATE_STOPPED), m_framesSent(0) {
        m_pDescriptor->AddRef();
        MFCreateEventQueue(&m_pEventQueue);
        MFCreateAttributes(&m_pAttributes, 4);
        if (m_pAttributes) {
            m_pAttributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, LanCamPinNameVideoCapture);
            m_pAttributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
            m_pAttributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
            m_pAttributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
        }
        FillNv12Black(m_lastFrame);
        g_objectCount++;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
            *ppv = static_cast<IMFMediaStream2*>(this);
        } else {
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = (ULONG)InterlockedDecrement(&m_cRef);
        if (count == 0) delete this;
        return count;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->GetEvent(dwFlags, ppEvent);
        pQueue->Release();
        return hr;
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->BeginGetEvent(pCallback, pState);
        pQueue->Release();
        return hr;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->EndGetEvent(pResult, ppEvent);
        pQueue->Release();
        return hr;
    }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
        pQueue->Release();
        return hr;
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override {
        if (!ppStreamDescriptor) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        *ppStreamDescriptor = m_pDescriptor;
        m_pDescriptor->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* pToken) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (!m_running) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        if (pToken) pToken->AddRef();
        m_tokens.push_back(pToken);
        m_cv.notify_all();
        return S_OK;
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        m_state = value;
        return S_OK;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override {
        if (!value) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        *value = m_state;
        return S_OK;
    }

    IMFAttributes* GetAttributes() const { return m_pAttributes; }

    void Start(const LanCamSession& session) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown || m_running) return;
            m_running = true;
            m_state = MF_STREAM_STATE_RUNNING;
        }
        m_decoder.Start(session.host, session.port, session.roomId, session.pin);
        LanCamLog("stream start: %ls:%d room=%ls", session.host.c_str(), session.port, session.roomId.c_str());
        PROPVARIANT empty;
        PropVariantInit(&empty);
        QueueEvent(MEStreamStarted, GUID_NULL, S_OK, &empty);
        m_thread = std::thread(&LanCamMfStream::PacingLoop, this);
    }

    void Stop() {
        StopWorker();
        PROPVARIANT empty;
        PropVariantInit(&empty);
        QueueEvent(MEStreamStopped, GUID_NULL, S_OK, &empty);
    }

    void Shutdown() {
        StopWorker();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        if (m_pEventQueue) {
            m_pEventQueue->Shutdown();
            m_pEventQueue->Release();
            m_pEventQueue = NULL;
        }
        m_pSource = NULL;
    }

private:
    ~LanCamMfStream() {
        StopWorker();
        if (m_pEventQueue) m_pEventQueue->Release();
        if (m_pAttributes) m_pAttributes->Release();
        if (m_pDescriptor) m_pDescriptor->Release();
        g_objectCount--;
    }

    HRESULT AcquireQueue(IMFMediaEventQueue** ppQueue) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown || !m_pEventQueue) return MF_E_SHUTDOWN;
        *ppQueue = m_pEventQueue;
        m_pEventQueue->AddRef();
        return S_OK;
    }

    void StopWorker() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = false;
            m_state = MF_STREAM_STATE_STOPPED;
            m_cv.notify_all();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_decoder.Stop();

        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_tokens.empty()) {
            IUnknown* pToken = m_tokens.front();
            m_tokens.pop_front();
            if (pToken) pToken->Release();
        }
    }

    void PacingLoop() {
        LONGLONG nextDue = MFGetSystemTime();
        std::vector<BYTE> bgr;
        int srcW = 0;
        int srcH = 0;
        bool loggedFirstFrame = false;

        for (;;) {
            IUnknown* pToken = NULL;
            bool hasToken = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(33), [this] { return !m_running || !m_tokens.empty(); });
                if (!m_running) return;
                if (m_tokens.empty()) {
                    continue;
                }
                pToken = m_tokens.front();
                m_tokens.pop_front();
                hasToken = true;
            }

            // Pace to 30 fps
            LONGLONG now = MFGetSystemTime();
            if (nextDue > now) {
                Sleep((DWORD)((nextDue - now) / 10000));
            }
            nextDue = max(nextDue + kFrameDuration100ns, MFGetSystemTime() - kFrameDuration100ns);

            if (m_decoder.GetLatestFrameRgb24(bgr, srcW, srcH) && !bgr.empty() && srcH > 0) {
                size_t stride = bgr.size() / (size_t)srcH;
                if (stride >= (size_t)srcW * 3) {
                    ConvertBgrToNv12Letterbox(bgr.data(), srcW, srcH, stride, m_lastFrame);
                    if (!loggedFirstFrame) {
                        loggedFirstFrame = true;
                        LanCamLog("first live frame %dx%d", srcW, srcH);
                    }
                }
            }

            HRESULT hr = DeliverSample(pToken);
            if (pToken) pToken->Release();
            if (FAILED(hr) && hasToken) {
                LanCamLog("DeliverSample failed 0x%08lx", (unsigned long)hr);
            }
        }
    }

    HRESULT DeliverSample(IUnknown* pToken) {
        IMFSample* pSample = NULL;
        IMFMediaBuffer* pBuffer = NULL;
        HRESULT hr = MFCreateSample(&pSample);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer((DWORD)m_lastFrame.size(), &pBuffer);

        if (SUCCEEDED(hr)) {
            BYTE* pData = NULL;
            hr = pBuffer->Lock(&pData, NULL, NULL);
            if (SUCCEEDED(hr)) {
                CopyMemory(pData, m_lastFrame.data(), m_lastFrame.size());
                pBuffer->Unlock();
                hr = pBuffer->SetCurrentLength((DWORD)m_lastFrame.size());
            }
        }

        if (SUCCEEDED(hr)) hr = pSample->AddBuffer(pBuffer);
        if (SUCCEEDED(hr)) hr = pSample->SetSampleTime(MFGetSystemTime());
        if (SUCCEEDED(hr)) hr = pSample->SetSampleDuration(kFrameDuration100ns);
        if (SUCCEEDED(hr) && pToken) hr = pSample->SetUnknown(MFSampleExtension_Token, pToken);

        if (SUCCEEDED(hr)) {
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_UNKNOWN;
            value.punkVal = pSample;
            pSample->AddRef();
            hr = QueueEvent(MEMediaSample, GUID_NULL, S_OK, &value);
            PropVariantClear(&value);
            if (SUCCEEDED(hr)) {
                m_framesSent++;
                if (m_framesSent == 1 || m_framesSent % 300 == 0) {
                    LanCamLog("samples delivered: %llu", (unsigned long long)m_framesSent);
                }
            }
        }

        if (pBuffer) pBuffer->Release();
        if (pSample) pSample->Release();
        return hr;
    }

    LONG m_cRef;
    LanCamMfSource* m_pSource;
    IMFStreamDescriptor* m_pDescriptor;
    IMFMediaEventQueue* m_pEventQueue;
    IMFAttributes* m_pAttributes;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<IUnknown*> m_tokens;
    std::thread m_thread;
    bool m_running;
    bool m_shutdown;
    MF_STREAM_STATE m_state;
    unsigned long long m_framesSent;

    MjpegDecoder m_decoder;
    std::vector<BYTE> m_lastFrame;
};

// ---------------------------------------------------------------------------------------------
// Media source
// ---------------------------------------------------------------------------------------------
class LanCamMfSource : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
public:
    LanCamMfSource()
        : m_cRef(1), m_pEventQueue(NULL), m_pPresentationDescriptor(NULL), m_pAttributes(NULL),
          m_pStream(NULL), m_shutdown(false), m_streamAnnounced(false) {
        g_objectCount++;
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
    }

    HRESULT Initialize() {
        HRESULT hr = MFCreateEventQueue(&m_pEventQueue);
        if (FAILED(hr)) return hr;

        hr = MFCreateAttributes(&m_pAttributes, 2);
        if (FAILED(hr)) return hr;

        IMFMediaType* pType = NULL;
        hr = CreateVideoMediaType(&pType);
        if (FAILED(hr)) return hr;

        IMFStreamDescriptor* pDescriptor = NULL;
        hr = MFCreateStreamDescriptor(0, 1, &pType, &pDescriptor);
        if (SUCCEEDED(hr)) {
            IMFMediaTypeHandler* pHandler = NULL;
            hr = pDescriptor->GetMediaTypeHandler(&pHandler);
            if (SUCCEEDED(hr)) {
                hr = pHandler->SetCurrentMediaType(pType);
                pHandler->Release();
            }
        }
        pType->Release();
        if (FAILED(hr)) {
            if (pDescriptor) pDescriptor->Release();
            return hr;
        }

        hr = MFCreatePresentationDescriptor(1, &pDescriptor, &m_pPresentationDescriptor);
        if (SUCCEEDED(hr)) hr = m_pPresentationDescriptor->SelectStream(0);

        if (SUCCEEDED(hr)) {
            m_pStream = new (std::nothrow) LanCamMfStream(this, pDescriptor);
            if (!m_pStream) hr = E_OUTOFMEMORY;
        }
        pDescriptor->Release();
        return hr;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        } else if (riid == IID_IMFGetService) {
            *ppv = static_cast<IMFGetService*>(this);
        } else if (riid == __uuidof(IKsControl)) {
            *ppv = static_cast<IKsControl*>(this);
        } else {
            LogIidFwd(riid);
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = (ULONG)InterlockedDecrement(&m_cRef);
        if (count == 0) delete this;
        return count;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->GetEvent(dwFlags, ppEvent);
        pQueue->Release();
        return hr;
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->BeginGetEvent(pCallback, pState);
        pQueue->Release();
        return hr;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->EndGetEvent(pResult, ppEvent);
        pQueue->Release();
        return hr;
    }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override {
        IMFMediaEventQueue* pQueue = NULL;
        HRESULT hr = AcquireQueue(&pQueue);
        if (FAILED(hr)) return hr;
        hr = pQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
        pQueue->Release();
        return hr;
    }

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override {
        if (!pdwCharacteristics) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override {
        if (!ppPresentationDescriptor) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_pPresentationDescriptor->Clone(ppPresentationDescriptor);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override {
        if (!pPresentationDescriptor || !pvarStartPosition) return E_INVALIDARG;
        if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

        bool announced = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            announced = m_streamAnnounced;
            m_streamAnnounced = true;
        }

        LanCamLog("source Start (announced=%d)", announced ? 1 : 0);

        PROPVARIANT streamValue;
        PropVariantInit(&streamValue);
        streamValue.vt = VT_UNKNOWN;
        streamValue.punkVal = static_cast<IMFMediaStream2*>(m_pStream);
        m_pStream->AddRef();
        QueueEvent(announced ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, &streamValue);
        PropVariantClear(&streamValue);

        PROPVARIANT start = *pvarStartPosition;
        if (start.vt == VT_EMPTY) {
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = 0;
        }
        QueueEvent(MESourceStarted, GUID_NULL, S_OK, &start);

        m_pStream->Start(LoadLanCamSessionMachine());
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
        }
        LanCamLog("source Stop");
        m_pStream->Stop();
        PROPVARIANT empty;
        PropVariantInit(&empty);
        QueueEvent(MESourceStopped, GUID_NULL, S_OK, &empty);
        return S_OK;
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        LanCamLog("source Shutdown");
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return S_OK;
            m_shutdown = true;
        }
        if (m_pStream) {
            m_pStream->Shutdown();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pEventQueue) {
            m_pEventQueue->Shutdown();
            m_pEventQueue->Release();
            m_pEventQueue = NULL;
        }
        return S_OK;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override {
        if (!ppAttributes) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        *ppAttributes = m_pAttributes;
        m_pAttributes->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override {
        if (!ppAttributes) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (dwStreamIdentifier != 0) return MF_E_INVALIDSTREAMNUMBER;
        *ppAttributes = m_pStream->GetAttributes();
        (*ppAttributes)->AddRef();
        return S_OK;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* ppvObject) override {
        if (ppvObject) *ppvObject = NULL;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    // IKsControl: the Frame Server probes camera controls; this camera exposes none.
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

private:
    ~LanCamMfSource() {
        Shutdown();
        if (m_pStream) m_pStream->Release();
        if (m_pAttributes) m_pAttributes->Release();
        if (m_pPresentationDescriptor) m_pPresentationDescriptor->Release();
        MFShutdown();
        g_objectCount--;
    }

    HRESULT AcquireQueue(IMFMediaEventQueue** ppQueue) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown || !m_pEventQueue) return MF_E_SHUTDOWN;
        *ppQueue = m_pEventQueue;
        m_pEventQueue->AddRef();
        return S_OK;
    }

    LONG m_cRef;
    std::mutex m_mutex;
    IMFMediaEventQueue* m_pEventQueue;
    IMFPresentationDescriptor* m_pPresentationDescriptor;
    IMFAttributes* m_pAttributes;
    LanCamMfStream* m_pStream;
    bool m_shutdown;
    bool m_streamAnnounced;
};

STDMETHODIMP LanCamMfStream::GetMediaSource(IMFMediaSource** ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || !m_pSource) return MF_E_SHUTDOWN;
    *ppMediaSource = static_cast<IMFMediaSourceEx*>(m_pSource);
    (*ppMediaSource)->AddRef();
    return S_OK;
}

// ---------------------------------------------------------------------------------------------
// Activation object: the Frame Server creates the registered CLSID as an IMFActivate and calls ActivateObject to
// obtain the media source. All IMFAttributes calls forward to an internal attribute store.
// ---------------------------------------------------------------------------------------------
static void LogIid(const char* where, REFIID riid) {
    wchar_t text[64] = {};
    StringFromGUID2(riid, text, 64);
    LanCamLog("%s: iid %ls", where, text);
}

class LanCamMfActivate : public IMFActivate {
public:
    LanCamMfActivate() : m_cRef(1), m_pAttrs(NULL) {
        MFCreateAttributes(&m_pAttrs, 4);
        g_objectCount++;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFActivate || riid == IID_IMFAttributes) {
            *ppv = static_cast<IMFActivate*>(this);
            AddRef();
            return S_OK;
        }
        LogIid("Activate QueryInterface rejected", riid);
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = (ULONG)InterlockedDecrement(&m_cRef);
        if (count == 0) delete this;
        return count;
    }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        LogIid("ActivateObject", riid);
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        LanCamMfSource* pSource = new (std::nothrow) LanCamMfSource();
        if (!pSource) return E_OUTOFMEMORY;
        HRESULT hr = pSource->Initialize();
        if (SUCCEEDED(hr)) hr = pSource->QueryInterface(riid, ppv);
        LanCamLog("ActivateObject -> 0x%08lx", (unsigned long)hr);
        pSource->Release();
        return hr;
    }
    STDMETHODIMP ShutdownObject() override { return S_OK; }
    STDMETHODIMP DetachObject() override { return S_OK; }

    // IMFAttributes (forwarded)
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return m_pAttrs->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return m_pAttrs->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return m_pAttrs->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return m_pAttrs->Compare(o, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return m_pAttrs->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return m_pAttrs->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return m_pAttrs->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return m_pAttrs->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return m_pAttrs->GetStringLength(k, l); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR b, UINT32 n, UINT32* l) override { return m_pAttrs->GetString(k, b, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* b, UINT32* l) override { return m_pAttrs->GetAllocatedString(k, b, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return m_pAttrs->GetBlobSize(k, s); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* s) override { return m_pAttrs->GetBlob(k, b, n, s); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return m_pAttrs->GetAllocatedBlob(k, b, s); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* o) override { return m_pAttrs->GetUnknown(k, r, o); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return m_pAttrs->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return m_pAttrs->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return m_pAttrs->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return m_pAttrs->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return m_pAttrs->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return m_pAttrs->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return m_pAttrs->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return m_pAttrs->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 s) override { return m_pAttrs->SetBlob(k, b, s); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return m_pAttrs->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return m_pAttrs->LockStore(); }
    STDMETHODIMP UnlockStore() override { return m_pAttrs->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* c) override { return m_pAttrs->GetCount(c); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return m_pAttrs->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return m_pAttrs->CopyAllItems(d); }

private:
    ~LanCamMfActivate() {
        if (m_pAttrs) m_pAttrs->Release();
        g_objectCount--;
    }

    LONG m_cRef;
    IMFAttributes* m_pAttrs;
};

// ---------------------------------------------------------------------------------------------
// COM plumbing
// ---------------------------------------------------------------------------------------------
class LanCamMfSourceFactory : public IClassFactory {
public:
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
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (pOuter) return CLASS_E_NOAGGREGATION;

        LogIid("CreateInstance requested", riid);

        // The Frame Server asks for an activation object; direct media-source requests are served as well.
        if (riid == IID_IMFActivate || riid == IID_IMFAttributes) {
            LanCamMfActivate* pActivate = new (std::nothrow) LanCamMfActivate();
            if (!pActivate) return E_OUTOFMEMORY;
            HRESULT activateHr = pActivate->QueryInterface(riid, ppv);
            pActivate->Release();
            LanCamLog("CreateInstance(activate) -> 0x%08lx", (unsigned long)activateHr);
            return activateHr;
        }

        LanCamMfSource* pSource = new (std::nothrow) LanCamMfSource();
        if (!pSource) return E_OUTOFMEMORY;

        HRESULT hr = pSource->Initialize();
        if (SUCCEEDED(hr)) hr = pSource->QueryInterface(riid, ppv);
        LanCamLog("CreateInstance(source) -> 0x%08lx", (unsigned long)hr);
        pSource->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) g_objectCount++; else g_objectCount--;
        return S_OK;
    }
};

static LanCamMfSourceFactory g_factory;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_LanCamMfSource) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return g_objectCount.load() == 0 ? S_OK : S_FALSE;
}
