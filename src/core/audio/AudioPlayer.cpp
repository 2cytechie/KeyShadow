#include "core/audio/AudioPlayer.h"

#include <qt_windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstring>

namespace ks {
namespace {

constexpr int kTargetBufferMs = 60;
constexpr size_t kRingSecondsMs = 500;

QString hresultText(HRESULT hr) {
    return QStringLiteral("HRESULT 0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

template <typename T> void release(T *&ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

} // namespace

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start(QString *error) {
    if (m_running.load()) {
        return true;
    }
    m_stopRequested.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { threadLoop(); });

    // 等待线程完成设备初始化（最多 500ms）
    for (int i = 0; i < 50; ++i) {
        if (!m_running.load() || m_client) {
            break;
        }
        Sleep(10);
    }

    if (!m_client) {
        if (error) {
            *error = QStringLiteral("无法初始化 WASAPI 音频输出");
        }
        stop();
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

void AudioPlayer::stop() {
    m_stopRequested.store(true);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
}

void AudioPlayer::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readIndex = 0;
    m_writeIndex = 0;
    m_available = 0;
}

void AudioPlayer::push(const float *interleaved, int frames) {
    if (!interleaved || frames <= 0) {
        return;
    }
    const size_t samples = static_cast<size_t>(frames) * static_cast<size_t>(m_channels);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ring.empty()) {
            m_ring.resize(static_cast<size_t>(m_sampleRate) * m_channels * kRingSecondsMs / 1000);
        }
        // 缓冲不足时丢弃最旧数据，保证低延迟（音频为主时钟，不允许无限堆积）
        if (m_available + samples > m_ring.size()) {
            const size_t drop = m_available + samples - m_ring.size();
            m_readIndex = (m_readIndex + drop) % m_ring.size();
            m_available -= drop;
        }
        for (size_t i = 0; i < samples; ++i) {
            m_ring[(m_writeIndex + i) % m_ring.size()] = interleaved[i];
        }
        m_writeIndex = (m_writeIndex + samples) % m_ring.size();
        m_available += samples;
    }
    m_cv.notify_one();
}

void AudioPlayer::threadLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(hr);

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&m_enumerator));
        if (FAILED(hr)) {
            break;
        }

        IMMDevice *device = nullptr;
        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) {
            break;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&m_client));
        release(device);
        if (FAILED(hr) || !m_client) {
            break;
        }

        WAVEFORMATEX *mixFormat = nullptr;
        hr = m_client->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            break;
        }

        // 仅支持 float32 混音格式（Windows 默认即为该格式）
        bool isFloat = mixFormat->wBitsPerSample == 32;
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto *ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mixFormat);
            isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        if (!isFloat) {
            CoTaskMemFree(mixFormat);
            break;
        }

        m_sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
        m_channels = mixFormat->nChannels;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ring.assign(static_cast<size_t>(m_sampleRate) * m_channels * kRingSecondsMs / 1000,
                          0.0f);
            m_readIndex = 0;
            m_writeIndex = 0;
            m_available = 0;
        }

        const REFERENCE_TIME bufferDuration =
            static_cast<REFERENCE_TIME>(kTargetBufferMs) * 10000; // 100ns 单位
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 bufferDuration, 0, mixFormat, nullptr);
        CoTaskMemFree(mixFormat);
        if (FAILED(hr)) {
            break;
        }

        hr = m_client->GetBufferSize(&m_bufferFrames);
        if (FAILED(hr)) {
            break;
        }

        hr = m_client->GetService(IID_PPV_ARGS(&m_render));
        if (FAILED(hr) || !m_render) {
            break;
        }

        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_event) {
            break;
        }
        hr = m_client->SetEventHandle(static_cast<HANDLE>(m_event));
        if (FAILED(hr)) {
            break;
        }

        hr = m_client->Start();
        if (FAILED(hr)) {
            break;
        }

        while (!m_stopRequested.load()) {
            const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(m_event), 200);
            if (m_stopRequested.load()) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;
            }

            UINT32 padding = 0;
            if (FAILED(m_client->GetCurrentPadding(&padding))) {
                break;
            }
            const UINT32 available = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
            if (!available) {
                continue;
            }

            BYTE *buffer = nullptr;
            if (FAILED(m_render->GetBuffer(available, &buffer)) || !buffer) {
                continue;
            }

            const size_t need = static_cast<size_t>(available) * m_channels;
            std::vector<float> temp(need, 0.0f);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_ring.empty()) {
                    const size_t take = std::min(need, m_available);
                    for (size_t i = 0; i < take; ++i) {
                        temp[i] = m_ring[(m_readIndex + i) % m_ring.size()];
                    }
                    m_readIndex = (m_readIndex + take) % m_ring.size();
                    m_available -= take;
                }
            }
            std::memcpy(buffer, temp.data(), need * sizeof(float));
            m_render->ReleaseBuffer(available, 0);
        }

        m_client->Stop();
    } while (false);

    if (m_event) {
        CloseHandle(static_cast<HANDLE>(m_event));
        m_event = nullptr;
    }
    release(m_render);
    release(m_client);
    release(m_enumerator);
    if (comInitialized) {
        CoUninitialize();
    }
    m_running.store(false);
}

} // namespace ks
