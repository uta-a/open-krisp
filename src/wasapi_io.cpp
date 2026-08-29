#include "wasapi_io.h"
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "avrt.lib")

static const int kDstSr = 48000;

// エラー文字列に HRESULT を添える。AUDCLNT_E_DEVICE_IN_USE(0x8889000A) や
// AUDCLNT_E_DEVICE_INVALIDATED(0x88890004) を TUI 上で切り分けられるようにする。
static void setErr(std::wstring* err, const wchar_t* what, HRESULT hr) {
    if (!err) return;
    wchar_t buf[160];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s (hr=0x%08X)", what, (unsigned)hr);
    *err = buf;
}

std::wstring deviceName(IMMDevice* dev) {
    IPropertyStore* ps = nullptr;
    std::wstring name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    return name;
}

std::vector<DeviceInfo> listDevices(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<DeviceInfo> out;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        out.push_back({ deviceName(d) });
        d->Release();
    }
    col->Release();
    return out;
}

IMMDevice* findDevice(IMMDeviceEnumerator* en, EDataFlow flow,
                      const std::wstring& substr, std::wstring* chosenName) {
    if (substr.empty()) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) {
            if (chosenName) *chosenName = deviceName(d);
            return d;
        }
        return nullptr;
    }
    // 部分一致（大文字小文字無視）
    std::wstring needle = substr;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0; col->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        std::wstring nm = deviceName(d);
        std::wstring low = nm;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (low.find(needle) != std::wstring::npos) {
            found = d; if (chosenName) *chosenName = nm;
        } else {
            d->Release();
        }
    }
    col->Release();
    return found;
}

static bool formatIsFloat(WAVEFORMATEX* w) {
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<WAVEFORMATEXTENSIBLE*>(w)->SubFormat ==
               KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return false;
}

// ================= Capture =================

bool WasapiCapture::start(IMMDevice* dev,
                          std::function<void(const float*, size_t)> onFrames,
                          std::wstring* err) {
    stop();  // 直前の状態が残っていても安全に作り直せるようにする
    cb_ = std::move(onFrames);
    total_.store(0, std::memory_order_relaxed);
    silent_.store(0, std::memory_order_relaxed);

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_);
    if (FAILED(hr)) { setErr(err, L"capture Activate 失敗", hr); stop(); return false; }
    hr = ac_->GetMixFormat(&mix_);
    if (FAILED(hr)) { setErr(err, L"capture GetMixFormat 失敗", hr); stop(); return false; }
    ev_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    REFERENCE_TIME dur = 2000000; // 200ms
    hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix_, nullptr);
    if (FAILED(hr)) { setErr(err, L"capture Initialize 失敗", hr); stop(); return false; }
    ac_->SetEventHandle(ev_);
    hr = ac_->GetService(__uuidof(IAudioCaptureClient), (void**)&cap_);
    if (FAILED(hr)) { setErr(err, L"capture GetService 失敗", hr); stop(); return false; }

    run_.store(true, std::memory_order_release);
    ac_->Start();
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) {
        setErr(err, L"capture スレッド生成失敗", HRESULT_FROM_WIN32(GetLastError()));
        stop(); return false;
    }
    return true;
}

DWORD WINAPI WasapiCapture::threadProc(void* self) {
    // WASAPI を触るスレッドは COM を初期化しておく（デバイス切替で何度も作り直すため）
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    reinterpret_cast<WasapiCapture*>(self)->loop();
    CoUninitialize();
    return 0;
}

void WasapiCapture::loop() {
    DWORD idx = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    const int srcSr = mix_->nSamplesPerSec;
    const int ch = mix_->nChannels;
    const bool isFloat = formatIsFloat(mix_);
    const int bytesPerSample = mix_->wBitsPerSample / 8;

    // 線形リサンプル用の位相保持
    double resamplePos = 0.0;
    float prevMono = 0.f;
    bool havePrev = false;
    std::vector<float> monoChunk;
    std::vector<float> out48;

    while (run_.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(ev_, 200) != WAIT_OBJECT_0) continue;
        UINT32 packet = 0;
        while (SUCCEEDED(cap_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data; UINT32 frames; DWORD flags;
            if (FAILED(cap_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT);
            total_.fetch_add(frames, std::memory_order_relaxed);
            if (silent) silent_.fetch_add(frames, std::memory_order_relaxed);
            monoChunk.clear();
            monoChunk.reserve(frames);
            for (UINT32 i = 0; i < frames; i++) {
                float acc = 0.f;
                if (!silent) {
                    const BYTE* p = data + (size_t)i * ch * bytesPerSample;
                    if (isFloat) {
                        const float* fp = reinterpret_cast<const float*>(p);
                        for (int c = 0; c < ch; c++) acc += fp[c];
                    } else if (bytesPerSample == 2) {
                        const int16_t* sp = reinterpret_cast<const int16_t*>(p);
                        for (int c = 0; c < ch; c++) acc += sp[c] / 32768.f;
                    }
                    acc /= ch;
                }
                monoChunk.push_back(acc);
            }
            cap_->ReleaseBuffer(frames);

            // srcSr -> 48000 線形リサンプル（位相連続）
            if (srcSr == kDstSr) {
                if (!monoChunk.empty()) cb_(monoChunk.data(), monoChunk.size());
            } else {
                double ratio = (double)kDstSr / srcSr;
                out48.clear();
                for (size_t i = 0; i < monoChunk.size(); i++) {
                    float cur = monoChunk[i];
                    if (!havePrev) { prevMono = cur; havePrev = true; }
                    // prevMono..cur 間を出力サンプル位置で補間
                    while (resamplePos < 1.0) {
                        float s = prevMono + (cur - prevMono) * (float)resamplePos;
                        out48.push_back(s);
                        resamplePos += 1.0 / ratio;
                    }
                    resamplePos -= 1.0;
                    prevMono = cur;
                }
                if (!out48.empty()) cb_(out48.data(), out48.size());
            }
        }
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
}

void WasapiCapture::stop() {
    run_.store(false, std::memory_order_release);
    // スレッドが確実に抜けるまで待つ。ここで諦めると、解放済みの cap_/ac_ を
    // まだ動いているスレッドが触ることになる（ev_ は 200ms タイムアウトなのですぐ返る）。
    if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); thread_ = nullptr; }
    if (ac_) ac_->Stop();
    if (cap_) { cap_->Release(); cap_ = nullptr; }
    if (ac_) { ac_->Release(); ac_ = nullptr; }
    if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
    if (ev_) { CloseHandle(ev_); ev_ = nullptr; }
    cb_ = nullptr;  // 再起動時に古いコールバックを使い回さない
}

// ================= Render =================

bool WasapiRender::start(IMMDevice* dev, std::function<void(float*, size_t)> pull,
                         std::wstring* err) {
    stop();  // 直前の状態が残っていても安全に作り直せるようにする
    pull_ = std::move(pull);

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_);
    if (FAILED(hr)) { setErr(err, L"render Activate 失敗", hr); stop(); return false; }
    hr = ac_->GetMixFormat(&mix_);
    if (FAILED(hr)) { setErr(err, L"render GetMixFormat 失敗", hr); stop(); return false; }
    ev_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    REFERENCE_TIME dur = 2000000; // 200ms
    hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix_, nullptr);
    if (FAILED(hr)) { setErr(err, L"render Initialize 失敗", hr); stop(); return false; }
    ac_->SetEventHandle(ev_);
    ac_->GetBufferSize(&bufFrames_);
    hr = ac_->GetService(__uuidof(IAudioRenderClient), (void**)&ren_);
    if (FAILED(hr)) { setErr(err, L"render GetService 失敗", hr); stop(); return false; }

    // 事前に無音で満たしてから開始（アンダーラン回避）
    BYTE* buf = nullptr;
    if (SUCCEEDED(ren_->GetBuffer(bufFrames_, &buf)))
        ren_->ReleaseBuffer(bufFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    run_.store(true, std::memory_order_release);
    ac_->Start();
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) {
        setErr(err, L"render スレッド生成失敗", HRESULT_FROM_WIN32(GetLastError()));
        stop(); return false;
    }
    return true;
}

DWORD WINAPI WasapiRender::threadProc(void* self) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    reinterpret_cast<WasapiRender*>(self)->loop();
    CoUninitialize();
    return 0;
}

void WasapiRender::loop() {
    DWORD idx = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    const int srcSr = mix_->nSamplesPerSec;  // レンダデバイスのSR
    const int ch = mix_->nChannels;
    const bool isFloat = formatIsFloat(mix_);
    const int bytesPerSample = mix_->wBitsPerSample / 8;
    std::vector<float> mono;

    // レンダSRが48k以外なら 48k→srcSr へ線形リサンプル
    double resamplePos = 0.0;
    float prevMono = 0.f;
    std::vector<float> src48;  // 48kの供給を貯める

    while (run_.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(ev_, 200) != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        if (FAILED(ac_->GetCurrentPadding(&padding))) continue;
        UINT32 avail = bufFrames_ - padding;
        if (avail == 0) continue;

        BYTE* buf = nullptr;
        if (FAILED(ren_->GetBuffer(avail, &buf))) continue;

        // avail フレーム分の mono(デバイスSR) を用意
        mono.resize(avail);
        if (srcSr == kDstSr) {
            pull_(mono.data(), avail);
        } else {
            // 必要な 48k サンプル数を見積もって取得し、srcSr にリサンプル
            double ratio = (double)srcSr / kDstSr;  // 48k1個 -> ratio個
            size_t need48 = (size_t)(avail / ((double)srcSr / kDstSr)) + 2;
            src48.resize(need48);
            pull_(src48.data(), need48);
            size_t si = 0;
            for (UINT32 i = 0; i < avail; i++) {
                size_t i0 = (size_t)resamplePos;
                float a = (i0 < src48.size()) ? src48[i0] : 0.f;
                float b = (i0 + 1 < src48.size()) ? src48[i0 + 1] : a;
                float frac = (float)(resamplePos - i0);
                mono[i] = a + (b - a) * frac;
                resamplePos += (double)kDstSr / srcSr;
            }
            // 位相を次バッファへ持ち越し（消費した整数分を戻す）
            resamplePos -= (double)((size_t)resamplePos);
        }

        // mono -> デバイス形式へ書き込み
        for (UINT32 i = 0; i < avail; i++) {
            float v = mono[i];
            if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
            BYTE* p = buf + (size_t)i * ch * bytesPerSample;
            if (isFloat) {
                float* fp = reinterpret_cast<float*>(p);
                for (int c = 0; c < ch; c++) fp[c] = v;
            } else if (bytesPerSample == 2) {
                int16_t s = (int16_t)(v * 32767.f);
                int16_t* sp = reinterpret_cast<int16_t*>(p);
                for (int c = 0; c < ch; c++) sp[c] = s;
            }
        }
        ren_->ReleaseBuffer(avail, 0);
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
}

void WasapiRender::stop() {
    run_.store(false, std::memory_order_release);
    // capture 側と同じ理由で無期限待ち（wasapi_io.h の冒頭コメント参照）。
    if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); thread_ = nullptr; }
    if (ac_) ac_->Stop();
    if (ren_) { ren_->Release(); ren_ = nullptr; }
    if (ac_) { ac_->Release(); ac_ = nullptr; }
    if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
    if (ev_) { CloseHandle(ev_); ev_ = nullptr; }
    bufFrames_ = 0;
    pull_ = nullptr;  // 再起動時に古いコールバックを使い回さない
}
