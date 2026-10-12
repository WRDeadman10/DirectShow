#pragma once

#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")

class MjpegDecoder {
public:
    MjpegDecoder() : m_pFactory(nullptr), m_running(false), m_hasNewFrame(false), m_width(1280), m_height(720) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        CoCreateInstance(
            CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_pFactory)
        );
    }

    ~MjpegDecoder() {
        Stop();
        if (m_pFactory) {
            m_pFactory->Release();
            m_pFactory = nullptr;
        }
    }

    void Start(const std::wstring& host, int port, const std::wstring& roomId, const std::wstring& pin) {
        Stop();
        m_running = true;
        m_streamThread = std::thread(&MjpegDecoder::StreamWorker, this, host, port, roomId, pin);
    }

    void Stop() {
        m_running = false;
        if (m_streamThread.joinable()) {
            m_streamThread.join();
        }
    }

    bool GetLatestFrameRgb24(std::vector<BYTE>& outRgb, int& outWidth, int& outHeight) {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_rgbBuffer.empty()) {
            return false;
        }
        outRgb = m_rgbBuffer;
        outWidth = m_width;
        outHeight = m_height;
        return true;
    }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    IWICImagingFactory* m_pFactory;
    std::atomic<bool> m_running;
    std::atomic<bool> m_hasNewFrame;
    int m_width;
    int m_height;
    std::vector<BYTE> m_rgbBuffer;
    std::mutex m_frameMutex;
    std::thread m_streamThread;

    bool DecodeJpegToRgb24(const BYTE* jpegData, DWORD jpegSize) {
        if (!m_pFactory || jpegSize == 0) return false;

        IWICStream* pStream = nullptr;
        HRESULT hr = m_pFactory->CreateStream(&pStream);
        if (FAILED(hr)) return false;

        hr = pStream->InitializeFromMemory(const_cast<BYTE*>(jpegData), jpegSize);
        if (FAILED(hr)) {
            pStream->Release();
            return false;
        }

        IWICBitmapDecoder* pDecoder = nullptr;
        hr = m_pFactory->CreateDecoderFromStream(
            pStream,
            NULL,
            WICDecodeMetadataCacheOnDemand,
            &pDecoder
        );

        if (FAILED(hr)) {
            pStream->Release();
            return false;
        }

        IWICBitmapFrameDecode* pFrame = nullptr;
        hr = pDecoder->GetFrame(0, &pFrame);
        if (FAILED(hr)) {
            pDecoder->Release();
            pStream->Release();
            return false;
        }

        UINT w = 0, h = 0;
        pFrame->GetSize(&w, &h);

        IWICFormatConverter* pConverter = nullptr;
        hr = m_pFactory->CreateFormatConverter(&pConverter);
        if (SUCCEEDED(hr)) {
            hr = pConverter->Initialize(
                pFrame,
                GUID_WICPixelFormat24bppBGR,
                WICBitmapDitherTypeNone,
                NULL,
                0.0,
                WICBitmapPaletteTypeCustom
            );

            if (SUCCEEDED(hr)) {
                UINT stride = ((w * 3) + 3) & ~3;
                UINT bufferSize = stride * h;

                std::vector<BYTE> tempRgb(bufferSize);
                hr = pConverter->CopyPixels(NULL, stride, bufferSize, tempRgb.data());
                if (SUCCEEDED(hr)) {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_rgbBuffer = std::move(tempRgb);
                    m_width = w;
                    m_height = h;
                    m_hasNewFrame = true;
                }
            }
            pConverter->Release();
        }

        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        return SUCCEEDED(hr);
    }

    void StreamWorker(std::wstring host, int port, std::wstring roomId, std::wstring pin) {
        std::wstring path = L"/stream/" + roomId + L"?pin=" + pin;

        while (m_running) {
            HINTERNET hSession = WinHttpOpen(
                L"LanCamFilter/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0
            );

            if (!hSession) {
                Sleep(1000);
                continue;
            }

            HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
            if (!hConnect) {
                WinHttpCloseHandle(hSession);
                Sleep(1000);
                continue;
            }

            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect,
                L"GET",
                path.c_str(),
                NULL,
                NULL, // no referrer
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                0
            );

            if (!hRequest) {
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                Sleep(1000);
                continue;
            }

            // Set read timeouts to 5 seconds
            WinHttpSetTimeouts(hRequest, 3000, 3000, 5000, 5000);

            BOOL bSend = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (!bSend || !WinHttpReceiveResponse(hRequest, NULL)) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                Sleep(1000);
                continue;
            }

            std::vector<BYTE> streamBuffer;
            streamBuffer.reserve(1024 * 1024);

            BYTE readChunk[8192];
            DWORD bytesRead = 0;

            while (m_running && WinHttpReadData(hRequest, readChunk, sizeof(readChunk), &bytesRead) && bytesRead > 0) {
                size_t oldSize = streamBuffer.size();
                streamBuffer.insert(streamBuffer.end(), readChunk, readChunk + bytesRead);

                // Search for JPEG Start of Image (0xFF, 0xD8) and End of Image (0xFF, 0xD9)
                while (streamBuffer.size() >= 4) {
                    size_t soi = (size_t)-1;
                    for (size_t i = 0; i + 1 < streamBuffer.size(); ++i) {
                        if (streamBuffer[i] == 0xFF && streamBuffer[i + 1] == 0xD8) {
                            soi = i;
                            break;
                        }
                    }

                    if (soi == (size_t)-1) {
                        // Keep last byte in case FF was at end
                        BYTE last = streamBuffer.back();
                        streamBuffer.clear();
                        if (last == 0xFF) streamBuffer.push_back(last);
                        break;
                    }

                    size_t eoi = (size_t)-1;
                    for (size_t i = soi + 2; i + 1 < streamBuffer.size(); ++i) {
                        if (streamBuffer[i] == 0xFF && streamBuffer[i + 1] == 0xD9) {
                            eoi = i + 2; // includes 0xD9
                            break;
                        }
                    }

                    if (eoi != (size_t)-1) {
                        // Found complete JPEG frame!
                        const BYTE* frameData = streamBuffer.data() + soi;
                        DWORD frameLen = (DWORD)(eoi - soi);
                        DecodeJpegToRgb24(frameData, frameLen);

                        // Discard consumed frame and data preceding it
                        streamBuffer.erase(streamBuffer.begin(), streamBuffer.begin() + eoi);
                    } else {
                        // Incomplete frame, discard prefix before SOI if any
                        if (soi > 0) {
                            streamBuffer.erase(streamBuffer.begin(), streamBuffer.begin() + soi);
                        }
                        break;
                    }
                }
            }

            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);

            if (m_running) {
                Sleep(500); // Retry backoff
            }
        }
    }
};
