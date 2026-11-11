#pragma once

#include <QString>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDeviceEnumerator;

namespace ks {

/// WASAPI 共享模式低延迟播放器（事件驱动）。内部维护环形缓冲，抵挡 USB 传输抖动。
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    /// 打开默认播放设备，输出格式由系统混音格式决定（通常 48kHz float32 立体声）。
    bool start(QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_running.load(); }

    int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }

    /// 写入交错 float32 PCM（长度 = frames * channels）。
    void push(const float *interleaved, int frames);

    /// 清空缓冲（重连时使用）。
    void flush();

private:
    void threadLoop();

    IMMDeviceEnumerator *m_enumerator = nullptr;
    IAudioClient *m_client = nullptr;
    IAudioRenderClient *m_render = nullptr;
    void *m_event = nullptr;
    int m_sampleRate = 48000;
    int m_channels = 2;
    unsigned int m_bufferFrames = 0;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<float> m_ring;
    size_t m_readIndex = 0;
    size_t m_writeIndex = 0;
    size_t m_available = 0;
};

} // namespace ks
