#include "lancam_filter.h"
#include <uuids.h>
#include <ks.h>
#include <ksproxy.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// -------------------------------------------------------------
// Helper: Enumerator for Pins
// -------------------------------------------------------------
class CEnumPins : public IEnumPins {
public:
    CEnumPins(IPin* pPin) : m_cRef(1), m_pPin(pPin), m_iPos(0) {
        if (m_pPin) m_pPin->AddRef();
    }
    virtual ~CEnumPins() {
        if (m_pPin) m_pPin->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
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

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (cPins == 0) return S_OK;
        if (m_iPos == 0 && m_pPin) {
            ppPins[0] = m_pPin;
            m_pPin->AddRef();
            m_iPos = 1;
            if (pcFetched) *pcFetched = 1;
            return (cPins == 1) ? S_OK : S_FALSE;
        }
        if (pcFetched) *pcFetched = 0;
        return S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cPins) override {
        m_iPos += cPins;
        return (m_iPos <= 1) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        m_iPos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new CEnumPins(m_pPin);
        return S_OK;
    }

private:
    LONG m_cRef;
    IPin* m_pPin;
    ULONG m_iPos;
};

// -------------------------------------------------------------
// Helper: Enumerator for Media Types
// -------------------------------------------------------------
class CEnumMediaTypes : public IEnumMediaTypes {
public:
    CEnumMediaTypes(const AM_MEDIA_TYPE& mt) : m_cRef(1), m_iPos(0) {
        CopyMediaType(&m_mt, &mt);
    }
    virtual ~CEnumMediaTypes() {
        FreeMediaType(m_mt);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
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

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        if (cMediaTypes == 0) return S_OK;
        if (m_iPos == 0) {
            ppMediaTypes[0] = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            CopyMediaType(ppMediaTypes[0], &m_mt);
            m_iPos = 1;
            if (pcFetched) *pcFetched = 1;
            return (cMediaTypes == 1) ? S_OK : S_FALSE;
        }
        if (pcFetched) *pcFetched = 0;
        return S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        m_iPos += cMediaTypes;
        return (m_iPos <= 1) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        m_iPos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new CEnumMediaTypes(m_mt);
        return S_OK;
    }

    static void CopyMediaType(AM_MEDIA_TYPE* pDest, const AM_MEDIA_TYPE* pSrc) {
        *pDest = *pSrc;
        if (pSrc->cbFormat > 0) {
            pDest->pbFormat = (BYTE*)CoTaskMemAlloc(pSrc->cbFormat);
            CopyMemory(pDest->pbFormat, pSrc->pbFormat, pSrc->cbFormat);
        }
        if (pDest->pUnk) pDest->pUnk->AddRef();
    }

    static void FreeMediaType(AM_MEDIA_TYPE& mt) {
        if (mt.cbFormat != 0) {
            CoTaskMemFree(mt.pbFormat);
            mt.cbFormat = 0;
            mt.pbFormat = NULL;
        }
        if (mt.pUnk) {
            mt.pUnk->Release();
            mt.pUnk = NULL;
        }
    }

private:
    LONG m_cRef;
    ULONG m_iPos;
    AM_MEDIA_TYPE m_mt;
};

// -------------------------------------------------------------
// LanCamPin Implementation
// -------------------------------------------------------------
LanCamPin::LanCamPin(LanCamFilter* pFilter)
    : m_cRef(1),
      m_pFilter(pFilter),
      m_pConnectedPin(NULL),
      m_pMemInputPin(NULL),
      m_pAllocator(NULL),
      m_bConnected(false),
      m_bStreaming(false),
      m_hStreamThread(NULL),
      m_hStopEvent(CreateEvent(NULL, TRUE, FALSE, NULL))
{
    ZeroMemory(&m_mt, sizeof(m_mt));
    BuildMediaType(&m_mt, 1280, 720, 30);
}

LanCamPin::~LanCamPin() {
    StopStreaming();
    if (m_hStopEvent) CloseHandle(m_hStopEvent);
    CEnumMediaTypes::FreeMediaType(m_mt);
}

void LanCamPin::BuildMediaType(AM_MEDIA_TYPE* pmt, int width, int height, int fps) {
    pmt->majortype = MEDIATYPE_Video;
    pmt->subtype = MEDIASUBTYPE_RGB24;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->lSampleSize = ((width * 3 + 3) & ~3) * height;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->pUnk = NULL;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);

    VIDEOINFOHEADER* pvih = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    ZeroMemory(pvih, sizeof(VIDEOINFOHEADER));
    pvih->AvgTimePerFrame = (10000000 / fps);
    pvih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pvih->bmiHeader.biWidth = width;
    pvih->bmiHeader.biHeight = height;
    pvih->bmiHeader.biPlanes = 1;
    pvih->bmiHeader.biBitCount = 24;
    pvih->bmiHeader.biCompression = BI_RGB;
    pvih->bmiHeader.biSizeImage = pmt->lSampleSize;
    pmt->pbFormat = (BYTE*)pvih;
}

STDMETHODIMP LanCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IQualityControl) {
        *ppv = static_cast<IQualityControl*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) LanCamPin::AddRef() { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) LanCamPin::Release() {
    ULONG u = InterlockedDecrement(&m_cRef);
    if (u == 0) delete this;
    return u;
}

STDMETHODIMP LanCamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (m_bConnected) return VFW_E_ALREADY_CONNECTED;

    const AM_MEDIA_TYPE* pMediaTypeToUse = pmt ? pmt : &m_mt;
    HRESULT hr = pReceivePin->ReceiveConnection(this, pMediaTypeToUse);
    if (FAILED(hr)) return hr;

    hr = pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&m_pMemInputPin);
    if (FAILED(hr)) {
        pReceivePin->Disconnect();
        return hr;
    }

    hr = m_pMemInputPin->GetAllocator(&m_pAllocator);
    if (FAILED(hr) || !m_pAllocator) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, NULL, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&m_pAllocator);
        if (FAILED(hr)) {
            m_pMemInputPin->Release();
            m_pMemInputPin = NULL;
            pReceivePin->Disconnect();
            return hr;
        }
    }

    ALLOCATOR_PROPERTIES propReq, propAct;
    ZeroMemory(&propReq, sizeof(propReq));
    propReq.cBuffers = 2;
    propReq.cbBuffer = m_mt.lSampleSize;
    propReq.cbAlign = 1;

    hr = m_pAllocator->SetProperties(&propReq, &propAct);
    if (SUCCEEDED(hr)) {
        hr = m_pMemInputPin->NotifyAllocator(m_pAllocator, FALSE);
    }

    m_pConnectedPin = pReceivePin;
    m_pConnectedPin->AddRef();
    m_bConnected = true;

    return S_OK;
}

STDMETHODIMP LanCamPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
    return E_UNEXPECTED; // Output pin does not accept input connections
}

STDMETHODIMP LanCamPin::Disconnect() {
    if (!m_bConnected) return S_FALSE;
    StopStreaming();

    if (m_pAllocator) {
        m_pAllocator->Decommit();
        m_pAllocator->Release();
        m_pAllocator = NULL;
    }
    if (m_pMemInputPin) {
        m_pMemInputPin->Release();
        m_pMemInputPin = NULL;
    }
    if (m_pConnectedPin) {
        m_pConnectedPin->Release();
        m_pConnectedPin = NULL;
    }
    m_bConnected = false;
    return S_OK;
}

STDMETHODIMP LanCamPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    if (!m_bConnected || !m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    *pPin = m_pConnectedPin;
    m_pConnectedPin->AddRef();
    return S_OK;
}

STDMETHODIMP LanCamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!m_bConnected) return VFW_E_NOT_CONNECTED;
    CEnumMediaTypes::CopyMediaType(pmt, &m_mt);
    return S_OK;
}

STDMETHODIMP LanCamPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_pFilter;
    if (m_pFilter) m_pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Capture");
    return S_OK;
}

STDMETHODIMP LanCamPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP LanCamPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = (LPWSTR)CoTaskMemAlloc(sizeof(L"Capture"));
    wcscpy_s(*Id, sizeof(L"Capture") / sizeof(wchar_t), L"Capture");
    return S_OK;
}

STDMETHODIMP LanCamPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype == MEDIATYPE_Video && pmt->subtype == MEDIASUBTYPE_RGB24) {
        return S_OK;
    }
    return S_FALSE;
}

STDMETHODIMP LanCamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumMediaTypes(m_mt);
    return S_OK;
}

STDMETHODIMP LanCamPin::QueryInternalConnections(IPin** apPin, ULONG* nPin) { return E_NOTIMPL; }
STDMETHODIMP LanCamPin::EndOfStream() { return S_OK; }
STDMETHODIMP LanCamPin::BeginFlush() { return S_OK; }
STDMETHODIMP LanCamPin::EndFlush() { return S_OK; }
STDMETHODIMP LanCamPin::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) { return S_OK; }

// IQualityControl
STDMETHODIMP LanCamPin::Notify(IBaseFilter* pSelf, Quality q) { return S_OK; }
STDMETHODIMP LanCamPin::SetSink(IQualityControl* piqc) { return S_OK; }

// IAMStreamConfig
STDMETHODIMP LanCamPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (m_bConnected) return VFW_E_ALREADY_CONNECTED;
    CEnumMediaTypes::FreeMediaType(m_mt);
    CEnumMediaTypes::CopyMediaType(&m_mt, pmt);
    return S_OK;
}

STDMETHODIMP LanCamPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CEnumMediaTypes::CopyMediaType(*ppmt, &m_mt);
    return S_OK;
}

STDMETHODIMP LanCamPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 1;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP LanCamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex != 0) return S_FALSE;

    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CEnumMediaTypes::CopyMediaType(*ppmt, &m_mt);

    VIDEO_STREAM_CONFIG_CAPS* pCaps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    ZeroMemory(pCaps, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    pCaps->guid = MEDIATYPE_Video;
    pCaps->VideoStandard = 0;
    pCaps->InputSize.cx = 1280;
    pCaps->InputSize.cy = 720;
    pCaps->MinCroppingSize.cx = 320;
    pCaps->MinCroppingSize.cy = 240;
    pCaps->MaxCroppingSize.cx = 1920;
    pCaps->MaxCroppingSize.cy = 1080;
    pCaps->MinOutputSize = pCaps->MinCroppingSize;
    pCaps->MaxOutputSize = pCaps->MaxCroppingSize;
    pCaps->MinFrameInterval = 10000000 / 60;
    pCaps->MaxFrameInterval = 10000000 / 5;

    return S_OK;
}

// IKsPropertySet (Required by Windows Camera app and Skype/Teams)
STDMETHODIMP LanCamPin::Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) {
    return E_NOTIMPL;
}

STDMETHODIMP LanCamPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet == AMPROPSETID_Pin) {
        if (dwPropID == AMPROPERTY_PIN_CATEGORY) {
            if (!pPropData || cbPropData < sizeof(GUID)) return E_INVALIDARG;
            *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
            if (pcbReturned) *pcbReturned = sizeof(GUID);
            return S_OK;
        }
    }
    return E_PROP_SET_UNSUPPORTED;
}

STDMETHODIMP LanCamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
        if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_PROP_SET_UNSUPPORTED;
}

void LanCamPin::StartStreaming() {
    if (m_bStreaming) return;
    ResetEvent(m_hStopEvent);
    m_bStreaming = true;
    if (m_pAllocator) m_pAllocator->Commit();

    // Start MJPEG stream consumer
    LanCamSession session = LoadLanCamSession();
    m_pFilter->GetDecoder().Start(session.host, session.port, session.roomId, session.pin);

    m_hStreamThread = CreateThread(NULL, 0, StreamThreadProc, this, 0, NULL);
}

void LanCamPin::StopStreaming() {
    if (!m_bStreaming) return;
    m_bStreaming = false;
    SetEvent(m_hStopEvent);
    if (m_hStreamThread) {
        WaitForSingleObject(m_hStreamThread, 2000);
        CloseHandle(m_hStreamThread);
        m_hStreamThread = NULL;
    }
    m_pFilter->GetDecoder().Stop();
    if (m_pAllocator) m_pAllocator->Decommit();
}

DWORD WINAPI LanCamPin::StreamThreadProc(LPVOID lpParam) {
    LanCamPin* pPin = (LanCamPin*)lpParam;
    pPin->StreamLoop();
    return 0;
}

void LanCamPin::StreamLoop() {
    REFERENCE_TIME rtStart = 0;
    const REFERENCE_TIME rtFrameDuration = 10000000 / 30; // 30 FPS

    std::vector<BYTE> rgbBuffer;
    int frameW = 1280;
    int frameH = 720;

    while (m_bStreaming && WaitForSingleObject(m_hStopEvent, 33) == WAIT_TIMEOUT) {
        if (!m_pAllocator || !m_pMemInputPin) continue;

        IMediaSample* pSample = NULL;
        HRESULT hr = m_pAllocator->GetBuffer(&pSample, NULL, NULL, 0);
        if (FAILED(hr) || !pSample) continue;

        BYTE* pBuffer = NULL;
        pSample->GetPointer(&pBuffer);
        long cbBuffer = pSample->GetSize();

        if (pBuffer && cbBuffer > 0) {
            static ULONGLONG lastShmFrameIdx = 0;
            // 1. Prioritize WebRTC Shared Memory Bridge (pure P2P WebRTC from browser)
            bool hasFrame = m_pFilter->GetShmReader().ReadLatestFrame(rgbBuffer, frameW, frameH, lastShmFrameIdx);

            // 2. Fallback to MJPEG if shared memory is inactive
            if (!hasFrame && rgbBuffer.empty()) {
                hasFrame = m_pFilter->GetDecoder().GetLatestFrameRgb24(rgbBuffer, frameW, frameH);
            }

            if (hasFrame && !rgbBuffer.empty()) {
                // Copy frame into sample buffer
                size_t copyBytes = min((size_t)cbBuffer, rgbBuffer.size());
                CopyMemory(pBuffer, rgbBuffer.data(), copyBytes);
                pSample->SetActualDataLength((long)copyBytes);
            } else {
                // Blank dark test pattern while waiting for WebRTC stream
                ZeroMemory(pBuffer, cbBuffer);
                pSample->SetActualDataLength(cbBuffer);
            }

            REFERENCE_TIME rtEnd = rtStart + rtFrameDuration;
            pSample->SetTime(&rtStart, &rtEnd);
            pSample->SetSyncPoint(TRUE);

            m_pMemInputPin->Receive(pSample);
            rtStart = rtEnd;
        }

        pSample->Release();
    }
}

// -------------------------------------------------------------
// LanCamFilter Implementation
// -------------------------------------------------------------
LanCamFilter::LanCamFilter()
    : m_cRef(1),
      m_state(State_Stopped),
      m_pClock(NULL),
      m_pGraph(NULL)
{
    m_pPin = new LanCamPin(this);
}

LanCamFilter::~LanCamFilter() {
    if (m_pPin) {
        m_pPin->Release();
        m_pPin = NULL;
    }
    if (m_pClock) m_pClock->Release();
}

STDMETHODIMP LanCamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IPersist) {
        *ppv = static_cast<IPersist*>(this);
    } else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) LanCamFilter::AddRef() { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) LanCamFilter::Release() {
    ULONG u = InterlockedDecrement(&m_cRef);
    if (u == 0) delete this;
    return u;
}

STDMETHODIMP LanCamFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_LanCamFilter;
    return S_OK;
}

STDMETHODIMP LanCamFilter::Stop() {
    m_state = State_Stopped;
    if (m_pPin) m_pPin->StopStreaming();
    return S_OK;
}

STDMETHODIMP LanCamFilter::Pause() {
    m_state = State_Paused;
    return S_OK;
}

STDMETHODIMP LanCamFilter::Run(REFERENCE_TIME tStart) {
    m_state = State_Running;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

STDMETHODIMP LanCamFilter::GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    *State = m_state;
    return S_OK;
}

STDMETHODIMP LanCamFilter::SetSyncSource(IReferenceClock* pClock) {
    if (m_pClock) m_pClock->Release();
    m_pClock = pClock;
    if (m_pClock) m_pClock->AddRef();
    return S_OK;
}

STDMETHODIMP LanCamFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    *pClock = m_pClock;
    if (m_pClock) m_pClock->AddRef();
    return S_OK;
}

STDMETHODIMP LanCamFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumPins(m_pPin);
    return S_OK;
}

STDMETHODIMP LanCamFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    if (Id && wcscmp(Id, L"Capture") == 0 && m_pPin) {
        *ppPin = m_pPin;
        m_pPin->AddRef();
        return S_OK;
    }
    *ppPin = NULL;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP LanCamFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    wcscpy_s(pInfo->achName, L"LAN Phone Camera");
    pInfo->pGraph = m_pGraph;
    if (m_pGraph) m_pGraph->AddRef();
    return S_OK;
}

STDMETHODIMP LanCamFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    m_pGraph = pGraph; // weak ref
    return S_OK;
}

STDMETHODIMP LanCamFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = (LPWSTR)CoTaskMemAlloc(sizeof(L"LAN Camera Streaming"));
    wcscpy_s(*pVendorInfo, sizeof(L"LAN Camera Streaming") / sizeof(wchar_t), L"LAN Camera Streaming");
    return S_OK;
}
