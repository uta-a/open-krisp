// WASAPI 共有モードのキャプチャ／レンダを、48kHz mono float を境界にラップする。
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <vector>
#include <functional>

// フレンドリ名の部分一致でデバイスを探す。substr が空なら既定を返す。
IMMDevice* findDevice(IMMDeviceEnumerator* en, EDataFlow flow,
                      const std::wstring& substr, std::wstring* chosenName);

std::wstring deviceName(IMMDevice* dev);

struct DeviceInfo { std::wstring name; };
std::vector<DeviceInfo> listDevices(IMMDeviceEnumerator* en, EDataFlow flow);

// キャプチャ用クライアント：任意フォーマットのマイクを 48kHz mono float に整えて callback へ渡す。
class WasapiCapture {
public:
    // onFrames(const float* mono48, size_t count) が処理スレッドから呼ばれる。
    bool start(IMMDevice* dev, std::function<void(const float*, size_t)> onFrames,
               std::wstring* err);
    void stop();
    ~WasapiCapture() { stop(); }

    // 診断用: これまでに受け取った全フレーム数と、SILENT フラグ付きフレーム数。
    unsigned long long totalFrames() const { return total_; }
    unsigned long long silentFrames() const { return silent_; }

private:
    IAudioClient* ac_ = nullptr;
    IAudioCaptureClient* cap_ = nullptr;
    WAVEFORMATEX* mix_ = nullptr;
    HANDLE ev_ = nullptr;
    HANDLE thread_ = nullptr;
    volatile bool run_ = false;
    volatile unsigned long long total_ = 0, silent_ = 0;
    std::function<void(const float*, size_t)> cb_;

    static DWORD WINAPI threadProc(void* self);
    void loop();
};

// レンダ用クライアント：48kHz mono float を要求元(pull)から取得し、デバイス形式へ変換して出力。
class WasapiRender {
public:
    // pull(float* mono48, size_t need) は need サンプルを埋めて返す（不足分は 0 埋め）。
    bool start(IMMDevice* dev, std::function<void(float*, size_t)> pull, std::wstring* err);
    void stop();
    int sampleRate() const { return mix_ ? mix_->nSamplesPerSec : 0; }
    ~WasapiRender() { stop(); }

private:
    IAudioClient* ac_ = nullptr;
    IAudioRenderClient* ren_ = nullptr;
    WAVEFORMATEX* mix_ = nullptr;
    HANDLE ev_ = nullptr;
    HANDLE thread_ = nullptr;
    UINT32 bufFrames_ = 0;
    volatile bool run_ = false;
    std::function<void(float*, size_t)> pull_;

    static DWORD WINAPI threadProc(void* self);
    void loop();
};
