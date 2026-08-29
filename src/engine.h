// AudioEngine : Krisp セッション・リングバッファ・AGC・WASAPI 入出力の所有者。
//
// 第1版はこれらすべてが wmain のスタックフレームに置かれ、キャプチャ／レンダの
// ラムダがそれらを参照キャプチャしていた。その形では「動かしたまま作り直す」ことが
// できないため、TUI 化にあたって寿命の管理をこのクラスへ移した。
//
// 【スレッド安全性の約束】
//   sess_ / frame_ / fin_ / fout_ / acc_ / agc_ は、キャプチャスレッドが join 済みの
//   ときにのみ UI スレッドから触ってよい。この境界を守るために、再構成を伴う操作は
//   すべて ctlMx_ の下で stop() → 変更 → start() の順に行う。
//   音を止めずに変えられる値（bypass / mute / AGC の ON・目標）は atomic で受け渡す。
#pragma once
#include "krisp_shim.h"
#include "wasapi_io.h"
#include "ring_buffer.h"
#include "agc.h"
#include <mmdeviceapi.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// 起動に必要な設定一式。ini / コマンドライン / TUI 操作の共通表現。
struct EngineConfig {
    std::wstring inMatch;                    // 入力デバイスの部分一致キー（空=自動選択）
    std::wstring outMatch = L"CABLE Input";  // 出力デバイスの部分一致キー
    int   durationMs  = 10;                  // Krisp フレーム長（10/15/20/30/32 のみ）
    float suppression = 100.f;               // ノイズ抑制の強さ 0..100
    bool  bypass      = false;               // true = Krisp を素通し（切り分け用）
    bool  muted       = false;               // true ならマイクを止める（出力に無音を流す）
    bool  agcEnabled  = true;                // 音量の自動調整
    float agcTarget   = 0.12f;               // AGC の目標音量 0..1
};

// Krisp が受け付けるフレーム長はこの5種のみ。
bool validDuration(int ms);
// 10/15/20/30/32 を dir(+1/-1) 方向に巡回する。
int  cycleDuration(int ms, int dir);

// 画面表示用の一貫したスナップショット。ピークは取得時にリセットされる。
struct EngineStats {
    bool  running = false;
    float inPeak = 0.f, outPeak = 0.f;   // 0..1 の区間ピーク
    float agcGain = 1.f;
    int   fifoMs = 0;
    unsigned long long underruns = 0, drops = 0;
    unsigned long long totalFrames = 0, silentFrames = 0;
    std::wstring inName, outName;        // 実際に開けたデバイスの表示名
    std::wstring notice;                 // 指定と違うデバイスを使った等の注意書き
};

class AudioEngine {
public:
    ~AudioEngine();

    // COM のデバイス列挙子を作る。--list だけならこれだけでよい。
    bool initDevices(std::wstring* err);
    // discord_krisp.node をロードして初期化する。プロセスで一度だけ呼べる。
    bool initKrisp(std::wstring* err);

    bool start(const EngineConfig& cfg, std::wstring* err);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // --- 音を止めずに変えられるもの ---
    void setBypass(bool v);
    void setMuted(bool v);
    // 毎フレーム描画から呼ばれても ctlMx_ を待たずに済むよう、atomic を直接読む。
    bool muted() const { return pMuted_.load(std::memory_order_relaxed); }
    void setSuppression(float v);   // 0..100 にクランプ
    void setAgcEnabled(bool v);
    void setAgcTarget(float v);     // 0.01..0.50 にクランプ

    // --- 停止→再構成→再開が必要なもの。失敗時は直前の設定へ巻き戻す ---
    bool setDevices(const std::wstring& inMatch, const std::wstring& outMatch,
                    std::wstring* err);
    bool setDuration(int ms, std::wstring* err);

    EngineStats  stats();
    EngineConfig config() const;
    std::vector<DeviceInfo> devices(EDataFlow flow) const;
    // 部分一致でデバイスが実在するか。CLI が「指定が見つからない」を
    // フォールバック前に検出するために使う。
    bool hasDevice(EDataFlow flow, const std::wstring& match) const;
    const std::wstring& moduleDir() const { return krisp_.moduleDir(); }

private:
    void onCapture(const float* p, size_t n);  // キャプチャスレッドからのみ
    void onRender(float* dst, size_t need);    // レンダスレッドからのみ

    // ctlMx_ を保持した状態で呼ぶこと。
    bool startLocked(const EngineConfig& cfg, std::wstring* err);
    void stopLocked();
    bool openDevices(const EngineConfig& cfg, std::wstring* err);
    void closeDevices();
    void resetStreamState();                   // 停止中にのみ呼ぶ

    IMMDeviceEnumerator* en_ = nullptr;
    bool        krispReady_ = false;
    KrispShim   krisp_;
    void*       sess_ = nullptr;
    IMMDevice*  inDev_ = nullptr;
    IMMDevice*  outDev_ = nullptr;
    std::wstring inName_, outName_, notice_;

    EngineConfig cfg_;

    // --- キャプチャスレッド専有（停止中は UI スレッドから触ってよい） ---
    size_t frame_ = 480;
    std::vector<float> acc_, fin_, fout_;
    Agc agc_{48000};

    // --- 両スレッドで共有 ---
    RingBuffer fifo_{48000};                   // 最大1秒。フレーム長を変えても作り直し不要
    std::atomic<bool> primed_{false};

    // UI → オーディオ
    std::atomic<bool>  pBypass_{false};
    std::atomic<bool>  pMuted_{false};
    std::atomic<bool>  pAgcOn_{true};
    std::atomic<float> pAgcTarget_{0.12f};
    // オーディオ → UI
    std::atomic<int>   inPeakMicro_{0}, outPeakMicro_{0};   // ピーク×1e6
    std::atomic<float> agcGainPub_{1.f};
    std::atomic<unsigned long long> underruns_{0}, drops_{0};

    std::atomic<bool> running_{false};
    mutable std::mutex ctlMx_;                 // start/stop/再構成の直列化

    // capture_/render_ は最後に宣言する。破棄がこれらから先に走り、
    // デストラクタ内の stop() でスレッドが join されてから上の状態が壊れる。
    WasapiCapture capture_;
    WasapiRender  render_;
};
