#include "engine.h"
#include <algorithm>
#include <cmath>

static const int kSr = 48000;
static const int kDurations[] = { 10, 15, 20, 30, 32 };

bool validDuration(int ms) {
    for (int d : kDurations) if (d == ms) return true;
    return false;
}

int cycleDuration(int ms, int dir) {
    const int n = (int)(sizeof(kDurations) / sizeof(kDurations[0]));
    int i = 0;
    for (int k = 0; k < n; k++) if (kDurations[k] == ms) { i = k; break; }
    i = (i + (dir >= 0 ? 1 : n - 1)) % n;
    return kDurations[i];
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

AudioEngine::~AudioEngine() {
    stop();                       // スレッドを止めてから状態を壊す
    std::lock_guard<std::mutex> lk(ctlMx_);
    closeDevices();
    if (en_) { en_->Release(); en_ = nullptr; }
}

bool AudioEngine::initDevices(std::wstring* err) {
    if (en_) return true;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en_);
    if (FAILED(hr) || !en_) {
        if (err) *err = L"オーディオデバイスの列挙に失敗しました。";
        return false;
    }
    return true;
}

bool AudioEngine::initKrisp(std::wstring* err) {
    if (krispReady_) return true;
    if (!krisp_.load(err)) return false;
    krispReady_ = true;
    return true;
}

std::vector<DeviceInfo> AudioEngine::devices(EDataFlow flow) const {
    if (!en_) return {};
    return listDevices(en_, flow);
}

EngineConfig AudioEngine::config() const {
    std::lock_guard<std::mutex> lk(ctlMx_);
    return cfg_;
}

bool AudioEngine::hasDevice(EDataFlow flow, const std::wstring& match) const {
    if (!en_) return false;
    IMMDevice* d = findDevice(en_, flow, match, nullptr);
    if (!d) return false;
    d->Release();
    return true;
}

// --- デバイス解決 ---------------------------------------------------------
// 指定が空／見つからない場合に段階的にフォールバックし、何を使ったかを notice_ に残す。
// 「保存していたマイクが消えて、黙って別のマイクが使われる」のが一番困るため。
bool AudioEngine::openDevices(const EngineConfig& cfg, std::wstring* err) {
    notice_.clear();

    // 入力: 指定 → 「CABLE を含まない最初のマイク」→ 既定エンドポイント
    IMMDevice* in = nullptr;
    std::wstring inName;
    if (!cfg.inMatch.empty()) {
        in = findDevice(en_, eCapture, cfg.inMatch, &inName);
        if (!in) notice_ = L"入力『" + cfg.inMatch + L"』が見つからないため既定を使用しています";
    }
    if (!in) {
        for (auto& d : listDevices(en_, eCapture)) {
            if (d.name.find(L"CABLE") == std::wstring::npos) {
                in = findDevice(en_, eCapture, d.name, &inName);
                if (in) break;
            }
        }
    }
    if (!in) in = findDevice(en_, eCapture, L"", &inName);
    if (!in) {
        if (err) *err = L"入力デバイスが見つかりません。";
        return false;
    }

    // 出力: 指定 → CABLE Input → 既定エンドポイント
    IMMDevice* out = nullptr;
    std::wstring outName;
    if (!cfg.outMatch.empty()) {
        out = findDevice(en_, eRender, cfg.outMatch, &outName);
        if (!out && !notice_.empty()) notice_ += L" / ";
        if (!out) notice_ += L"出力『" + cfg.outMatch + L"』が見つからないため既定を使用しています";
    }
    if (!out) out = findDevice(en_, eRender, L"CABLE Input", &outName);
    if (!out) out = findDevice(en_, eRender, L"", &outName);
    if (!out) {
        in->Release();
        if (err) *err = L"出力デバイスが見つかりません。";
        return false;
    }

    closeDevices();
    inDev_ = in;   inName_ = inName;
    outDev_ = out; outName_ = outName;
    return true;
}

void AudioEngine::closeDevices() {
    if (inDev_)  { inDev_->Release();  inDev_ = nullptr; }
    if (outDev_) { outDev_->Release(); outDev_ = nullptr; }
}

void AudioEngine::resetStreamState() {
    acc_.clear();
    fifo_.drop(fifo_.readAvailable());
    primed_.store(false, std::memory_order_release);
    agc_.reset();
    inPeakMicro_.store(0, std::memory_order_relaxed);
    outPeakMicro_.store(0, std::memory_order_relaxed);
}

// --- 起動と停止 -----------------------------------------------------------
bool AudioEngine::start(const EngineConfig& cfg, std::wstring* err) {
    std::lock_guard<std::mutex> lk(ctlMx_);
    return startLocked(cfg, err);
}

bool AudioEngine::startLocked(const EngineConfig& cfg, std::wstring* err) {
    if (!en_ && !initDevices(err)) return false;
    if (!krispReady_) { if (err) *err = L"Krisp が初期化されていません。"; return false; }
    if (!validDuration(cfg.durationMs)) { if (err) *err = L"フレーム長が不正です。"; return false; }

    if (!openDevices(cfg, err)) return false;

    // セッションは「新しいものを確保してから古いものを捨てる」。
    // 途中で失敗しても、呼び出し元が古いセッションのまま再開できる。
    void* ns = krisp_.ncSetup(kSr, cfg.durationMs);
    if (!ns) { if (err) *err = L"Krisp NC セッションの確立に失敗しました。"; return false; }
    if (sess_) krisp_.ncReset(sess_);
    sess_ = ns;

    cfg_ = cfg;
    frame_ = (size_t)(kSr / 1000) * cfg.durationMs;
    fin_.assign(frame_, 0.f);
    fout_.assign(frame_, 0.f);

    krisp_.setSuppression(clampf(cfg.suppression, 0.f, 100.f));
    pBypass_.store(cfg.bypass, std::memory_order_relaxed);
    pAgcOn_.store(cfg.agcEnabled, std::memory_order_relaxed);
    pAgcTarget_.store(clampf(cfg.agcTarget, 0.01f, 0.50f), std::memory_order_relaxed);
    agc_.setTarget(clampf(cfg.agcTarget, 0.01f, 0.50f));
    agc_.setEnabled(cfg.agcEnabled);

    underruns_.store(0, std::memory_order_relaxed);
    drops_.store(0, std::memory_order_relaxed);
    resetStreamState();

    // レンダを先に上げる。primed ゲートが 60ms 溜まるまで無音を出して待つので、
    // キャプチャが立ち上がるまでの間もアンダーランを数えずに済む。
    if (!render_.start(outDev_, [this](float* d, size_t n) { onRender(d, n); }, err)) {
        return false;
    }
    if (!capture_.start(inDev_, [this](const float* p, size_t n) { onCapture(p, n); }, err)) {
        render_.stop();
        return false;
    }
    running_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> lk(ctlMx_);
    stopLocked();
}

void AudioEngine::stopLocked() {
    running_.store(false, std::memory_order_release);
    // 消費側 → 生産側の順に止める。生産側を止めた時点で ncProcess の呼び出し元が
    // 消えるので、この後にセッションを触ってよい（join は wasapi_io 側で無期限待ち）。
    render_.stop();
    capture_.stop();
    if (sess_) { krisp_.ncReset(sess_); sess_ = nullptr; }
}

// --- ライブ変更 -----------------------------------------------------------
void AudioEngine::setBypass(bool v) {
    std::lock_guard<std::mutex> lk(ctlMx_);
    cfg_.bypass = v;
    pBypass_.store(v, std::memory_order_relaxed);
}

void AudioEngine::setSuppression(float v) {
    v = clampf(v, 0.f, 100.f);
    std::lock_guard<std::mutex> lk(ctlMx_);
    cfg_.suppression = v;
    // ProcessFloat が毎フレーム読むモジュールのグローバル。x64 の整列した
    // 4 バイト書き込みなので、処理中に書き換えても裂けない。
    if (krispReady_) krisp_.setSuppression(v);
}

void AudioEngine::setAgcEnabled(bool v) {
    std::lock_guard<std::mutex> lk(ctlMx_);
    cfg_.agcEnabled = v;
    pAgcOn_.store(v, std::memory_order_relaxed);
}

void AudioEngine::setAgcTarget(float v) {
    v = clampf(v, 0.01f, 0.50f);
    std::lock_guard<std::mutex> lk(ctlMx_);
    cfg_.agcTarget = v;
    pAgcTarget_.store(v, std::memory_order_relaxed);
}

// --- 再構成 ---------------------------------------------------------------
bool AudioEngine::setDevices(const std::wstring& inMatch, const std::wstring& outMatch,
                             std::wstring* err) {
    std::lock_guard<std::mutex> lk(ctlMx_);
    EngineConfig prev = cfg_;
    EngineConfig next = cfg_;
    next.inMatch = inMatch;
    next.outMatch = outMatch;

    stopLocked();
    if (startLocked(next, err)) return true;

    // 失敗したら元の設定で開き直す。切替に失敗してもアプリは動き続ける。
    std::wstring ignored;
    stopLocked();
    startLocked(prev, &ignored);
    return false;
}

bool AudioEngine::setDuration(int ms, std::wstring* err) {
    if (!validDuration(ms)) { if (err) *err = L"フレーム長が不正です。"; return false; }
    std::lock_guard<std::mutex> lk(ctlMx_);
    if (cfg_.durationMs == ms) return true;

    EngineConfig prev = cfg_;
    EngineConfig next = cfg_;
    next.durationMs = ms;

    stopLocked();
    if (startLocked(next, err)) return true;

    std::wstring ignored;
    stopLocked();
    startLocked(prev, &ignored);
    return false;
}

// --- 統計 -----------------------------------------------------------------
EngineStats AudioEngine::stats() {
    EngineStats s;
    s.running   = running_.load(std::memory_order_acquire);
    s.inPeak    = inPeakMicro_.exchange(0, std::memory_order_relaxed) / 1e6f;
    s.outPeak   = outPeakMicro_.exchange(0, std::memory_order_relaxed) / 1e6f;
    s.agcGain   = agcGainPub_.load(std::memory_order_relaxed);
    s.fifoMs    = (int)(fifo_.readAvailable() * 1000 / kSr);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.drops     = drops_.load(std::memory_order_relaxed);
    s.totalFrames  = capture_.totalFrames();
    s.silentFrames = capture_.silentFrames();
    {
        std::lock_guard<std::mutex> lk(ctlMx_);
        s.inName = inName_; s.outName = outName_; s.notice = notice_;
    }
    return s;
}

// --- オーディオ経路 -------------------------------------------------------
static float peakOf(const float* p, size_t n) {
    float m = 0.f;
    for (size_t i = 0; i < n; i++) { float a = fabsf(p[i]); if (a > m) m = a; }
    return m;
}

void AudioEngine::onCapture(const float* p, size_t n) {
    if (!running_.load(std::memory_order_acquire)) return;

    // UI から届いた AGC の設定をフレーム先頭で取り込む（agc.h は atomic を持たない）
    agc_.setEnabled(pAgcOn_.load(std::memory_order_relaxed));
    agc_.setTarget(pAgcTarget_.load(std::memory_order_relaxed));
    const bool bypass = pBypass_.load(std::memory_order_relaxed);

    acc_.insert(acc_.end(), p, p + n);
    size_t off = 0;
    while (acc_.size() - off >= frame_) {
        for (size_t i = 0; i < frame_; i++) fin_[i] = acc_[off + i];
        if (bypass) { for (size_t i = 0; i < frame_; i++) fout_[i] = fin_[i]; }
        else        { krisp_.ncProcess(sess_, fin_.data(), frame_, fout_.data(), frame_); }
        agc_.process(fout_.data(), frame_);   // ノイズ除去後に音量を一定化
        agcGainPub_.store(agc_.gain(), std::memory_order_relaxed);

        int ipk = (int)(peakOf(fin_.data(), frame_) * 1e6f);
        int opk = (int)(peakOf(fout_.data(), frame_) * 1e6f);
        if (ipk > inPeakMicro_.load(std::memory_order_relaxed))
            inPeakMicro_.store(ipk, std::memory_order_relaxed);
        if (opk > outPeakMicro_.load(std::memory_order_relaxed))
            outPeakMicro_.store(opk, std::memory_order_relaxed);

        // 過充填ならドリフト補正で古いデータを1フレーム捨てる
        const size_t kMaxFill = (size_t)kSr * 200 / 1000;
        if (fifo_.readAvailable() > kMaxFill) {
            fifo_.drop(frame_);
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
        fifo_.push(fout_.data(), frame_);
        off += frame_;
    }
    acc_.erase(acc_.begin(), acc_.begin() + off);
}

void AudioEngine::onRender(float* dst, size_t need) {
    // 起動直後や枯渇後は目標充填(60ms)まで無音で待ち、レイテンシの緩衝を作る。
    const size_t kTargetFill = (size_t)kSr * 60 / 1000;
    if (!primed_.load(std::memory_order_acquire)) {
        if (fifo_.readAvailable() >= kTargetFill) primed_.store(true, std::memory_order_release);
        else { for (size_t i = 0; i < need; i++) dst[i] = 0.f; return; }
    }
    size_t got = fifo_.pop(dst, need);
    if (got < need) {
        for (size_t i = got; i < need; i++) dst[i] = 0.f;
        underruns_.fetch_add(1, std::memory_order_relaxed);
        if (fifo_.readAvailable() == 0) primed_.store(false, std::memory_order_release);
    }
}
