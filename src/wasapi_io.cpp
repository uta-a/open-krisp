#include "wasapi_io.h"
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>
#include <algorithm>

#pragma comment(lib, "avrt.lib")

static const int kDstSr = 48000;

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
    cb_ = std::move(onFrames);
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_))) {
        if (err) *err = L"capture Activate 失敗"; return false;
    }
    if (FAILED(ac_->GetMixFormat(&mix_))) { if (err) *err = L"capture GetMixFormat 失敗"; return false; }
    ev_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    REFERENCE_TIME dur = 2000000; // 200ms
    HRESULT hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix_, nullptr);
    if (FAILED(hr)) { if (err) *err = L"capture Initialize 失敗"; return false; }
    ac_->SetEventHandle(ev_);
    if (FAILED(ac_->GetService(__uuidof(IAudioCaptureClient), (void**)&cap_))) {
        if (err) *err = L"capture GetService 失敗"; return false;
    }
    run_ = true;
    ac_->Start();
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    return true;
}

DWORD WINAPI WasapiCapture::threadProc(void* self) {
    reinterpret_cast<WasapiCapture*>(self)->loop();
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

    while (run_) {
        if (WaitForSingleObject(ev_, 200) != WAIT_OBJECT_0) continue;
        UINT32 packet = 0;
        while (SUCCEEDED(cap_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data; UINT32 frames; DWORD flags;
            if (FAILED(cap_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT);
            total_ += frames;
            if (silent) silent_ += frames;
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
    run_ = false;
    if (thread_) { WaitForSingleObject(thread_, 1000); CloseHandle(thread_); thread_ = nullptr; }
    if (ac_) ac_->Stop();
    if (cap_) { cap_->Release(); cap_ = nullptr; }
    if (ac_) { ac_->Release(); ac_ = nullptr; }
    if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
    if (ev_) { CloseHandle(ev_); ev_ = nullptr; }
}

// ================= Render =================

bool WasapiRender::start(IMMDevice* dev, std::function<void(float*, size_t)> pull,
                         std::wstring* err) {
    pull_ = std::move(pull);
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_))) {
        if (err) *err = L"render Activate 失敗"; return false;
    }
    if (FAILED(ac_->GetMixFormat(&mix_))) { if (err) *err = L"render GetMixFormat 失敗"; return false; }
    ev_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    REFERENCE_TIME dur = 2000000; // 200ms
    HRESULT hr = ac_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix_, nullptr);
    if (FAILED(hr)) { if (err) *err = L"render Initialize 失敗"; return false; }
    ac_->SetEventHandle(ev_);
    ac_->GetBufferSize(&bufFrames_);
    if (FAILED(ac_->GetService(__uuidof(IAudioRenderClient), (void**)&ren_))) {
        if (err) *err = L"render GetService 失敗"; return false;
    }
    // 事前に無音で満たしてから開始（アンダーラン回避）
    BYTE* buf = nullptr;
    if (SUCCEEDED(ren_->GetBuffer(bufFrames_, &buf)))
        ren_->ReleaseBuffer(bufFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    run_ = true;
    ac_->Start();
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    return true;
}

DWORD WINAPI WasapiRender::threadProc(void* self) {
    reinterpret_cast<WasapiRender*>(self)->loop();
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

    while (run_) {
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
    run_ = false;
    if (thread_) { WaitForSingleObject(thread_, 1000); CloseHandle(thread_); thread_ = nullptr; }
    if (ac_) ac_->Stop();
    if (ren_) { ren_->Release(); ren_ = nullptr; }
    if (ac_) { ac_->Release(); ac_ = nullptr; }
    if (mix_) { CoTaskMemFree(mix_); mix_ = nullptr; }
    if (ev_) { CloseHandle(ev_); ev_ = nullptr; }
}
