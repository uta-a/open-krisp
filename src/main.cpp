// OpenKrisp : Discord 同梱の Krisp NC を使い、任意マイクの入力をノイズ除去して
// 仮想オーディオケーブル(既定: VB-CABLE の "CABLE Input")へ流す常駐ブリッジ。
//
// チェーン: [実マイク] → 本アプリ(Krisp NC) → CABLE Input →(仮想)→ CABLE Output → 通話アプリ
//
// 使い方:
//   openkrisp --list                入出力デバイス一覧
//   openkrisp                       既定(入力=最初の非CABLEマイク, 出力=CABLE Input)
//   openkrisp --in fifine --out "CABLE Input"
//   openkrisp --suppression 100     抑制強度(0-100, 既定100)
//   openkrisp --duration 20         Krisp フレーム長ms(10/15/20/30/32, 既定10)
//                                    大きいほど文脈が増え発話保持が安定する場合がある
//                                    (その分レイテンシ増)。実音声で聴き比べて調整。
//   openkrisp --no-agc              AGC(音量自動調整)を無効化（既定は ON）
//   openkrisp --agc-target 0.10     AGC の目標音量(0..1, 既定0.10)。大きいほど大音量
//
// 法的注意: 本ツールは Krisp の署名検証を回避する。個人利用限定・再配布禁止。
#include "krisp_shim.h"
#include "wasapi_io.h"
#include "ring_buffer.h"
#include "agc.h"
#include <mmdeviceapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include <cwchar>
#include <cstdarg>
#include <cmath>

#pragma comment(lib, "ole32.lib")

// ワイド文字列を UTF-8 に変換して出力する（コンソール直・リダイレクト両対応）。
static void outw(const wchar_t* fmt, ...) {
    wchar_t wbuf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, fmt, ap);
    va_end(ap);
    char u8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8, sizeof(u8), nullptr, nullptr);
    if (n > 0) { fputs(u8, stdout); }
}

static const int kSr = 48000;

// 対応フレーム長(ms)。Krisp が受け付けるのはこの5種のみ。
static bool validDuration(int ms) {
    return ms == 10 || ms == 15 || ms == 20 || ms == 30 || ms == 32;
}

static std::atomic<bool> g_stop{false};
static BOOL WINAPI ctrlHandler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT) { g_stop = true; return TRUE; }
    return FALSE;
}

static std::wstring argVal(int argc, wchar_t** argv, const wchar_t* key, const wchar_t* def) {
    for (int i = 1; i < argc - 1; i++)
        if (wcscmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}
static bool hasFlag(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; i++) if (wcscmp(argv[i], key) == 0) return true;
    return false;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&en);

    if (hasFlag(argc, argv, L"--list")) {
        outw(L"=== 入力(マイク) ===\n");
        int i = 0; for (auto& d : listDevices(en, eCapture)) outw(L"  [%d] %s\n", i++, d.name.c_str());
        outw(L"=== 出力 ===\n");
        i = 0; for (auto& d : listDevices(en, eRender)) outw(L"  [%d] %s\n", i++, d.name.c_str());
        en->Release();
        return 0;
    }

    // --- Krisp ロード＆初期化 ---
    KrispShim krisp;
    std::wstring err;
    if (!krisp.load(&err)) {
        outw(L"[エラー] %s\n", err.c_str());
        return 1;
    }
    outw(L"[OK] Krisp モジュール: %s\n", krisp.moduleDir().c_str());

    // --- フレーム長・抑制レベル（実音声でのチューニング用） ---
    int durationMs = _wtoi(argVal(argc, argv, L"--duration", L"10").c_str());
    if (!validDuration(durationMs)) {
        outw(L"[警告] --duration は 10/15/20/30/32 のみ対応。10 に戻します。\n");
        durationMs = 10;
    }
    const size_t kFrame = (size_t)(kSr / 1000) * durationMs;  // 48 * ms サンプル

    std::wstring suppArg = argVal(argc, argv, L"--suppression", L"");
    if (!suppArg.empty()) {
        float lv = (float)_wtof(suppArg.c_str());
        if (lv < 0) lv = 0; if (lv > 100) lv = 100;
        krisp.setSuppression(lv);
        outw(L"[OK] 抑制レベル=%.0f\n", lv);
    }

    void* sess = krisp.ncSetup(kSr, durationMs);
    if (!sess) { outw(L"[エラー] KrispNCSetup 失敗\n"); return 2; }
    outw(L"[OK] NC セッション確立 (48kHz, %dms/%zuサンプル, model=full_NC)\n",
         durationMs, kFrame);

    // --- デバイス選択 ---
    std::wstring inSub  = argVal(argc, argv, L"--in", L"");
    std::wstring outSub = argVal(argc, argv, L"--out", L"CABLE Input");

    // 入力既定: --in 未指定なら「CABLE を含まない最初のマイク」を選ぶ
    IMMDevice* inDev = nullptr; std::wstring inName;
    if (inSub.empty()) {
        for (auto& d : listDevices(en, eCapture)) {
            if (d.name.find(L"CABLE") == std::wstring::npos) {
                inDev = findDevice(en, eCapture, d.name, &inName); break;
            }
        }
        if (!inDev) inDev = findDevice(en, eCapture, L"", &inName); // 全部CABLEなら既定
    } else {
        inDev = findDevice(en, eCapture, inSub, &inName);
    }
    IMMDevice* outDev = findDevice(en, eRender, outSub, nullptr);
    std::wstring outName;
    if (outDev) outName = deviceName(outDev);

    if (!inDev)  { outw(L"[エラー] 入力デバイスが見つかりません（--in で指定）\n"); return 3; }
    if (!outDev) { outw(L"[エラー] 出力デバイス '%s' が見つかりません（--list で確認）\n", outSub.c_str()); return 4; }
    outw(L"[OK] 入力: %s\n", inName.c_str());
    outw(L"[OK] 出力: %s\n", outName.c_str());

    // --- リングバッファ（処理後の 48k mono を貯める。ドリフト吸収の弾力バッファ） ---
    RingBuffer outFifo(kSr);        // 最大1秒
    const size_t kTargetFill = kSr * 60 / 1000;   // 目標充填 60ms
    const size_t kMaxFill    = kSr * 200 / 1000;  // これを超えたら古いデータを捨てる

    std::atomic<uint64_t> underruns{0}, drops{0};
    std::atomic<int> inPeakMicro{0}, outPeakMicro{0};   // 表示間隔中のピーク×1e6
    const bool bypass = hasFlag(argc, argv, L"--bypass"); // Krisp を素通し（切り分け用）

    // AGC（自動ゲイン調整）: Discord の「音量調節の自動化」相当。既定 ON。
    // Krisp のノイズ除去後にかけ、発話音量を一定に整える（--no-agc で無効）。
    Agc agc(kSr);
    if (hasFlag(argc, argv, L"--no-agc")) agc.setEnabled(false);
    {
        std::wstring tArg = argVal(argc, argv, L"--agc-target", L"");
        if (!tArg.empty()) {
            float tv = (float)_wtof(tArg.c_str());
            if (tv > 0 && tv <= 1.0f) agc.setTarget(tv);
        }
    }

    // キャプチャ: mono48 を貯め、480サンプル毎に Krisp 処理 → outFifo
    std::vector<float> acc;
    std::vector<float> fin(kFrame), fout(kFrame);
    auto peak = [](const float* p, size_t n) {
        float m = 0.f; for (size_t i = 0; i < n; i++) { float a = fabsf(p[i]); if (a > m) m = a; }
        return m;
    };
    auto onFrames = [&](const float* p, size_t n) {
        acc.insert(acc.end(), p, p + n);
        size_t off = 0;
        while (acc.size() - off >= kFrame) {
            for (size_t i = 0; i < kFrame; i++) fin[i] = acc[off + i];
            if (bypass) { for (size_t i = 0; i < kFrame; i++) fout[i] = fin[i]; }
            else        { krisp.ncProcess(sess, fin.data(), kFrame, fout.data(), kFrame); }
            agc.process(fout.data(), kFrame);   // ノイズ除去後に音量を一定化
            int ipk = (int)(peak(fin.data(), kFrame) * 1e6);
            int opk = (int)(peak(fout.data(), kFrame) * 1e6);
            if (ipk > inPeakMicro.load())  inPeakMicro.store(ipk);   // 区間ピーク保持
            if (opk > outPeakMicro.load()) outPeakMicro.store(opk);
            // 過充填ならドリフト補正で古いデータを1フレーム捨てる
            if (outFifo.readAvailable() > kMaxFill) { outFifo.drop(kFrame); drops++; }
            outFifo.push(fout.data(), kFrame);
            off += kFrame;
        }
        acc.erase(acc.begin(), acc.begin() + off);
    };

    // レンダ: outFifo から供給。起動直後や枯渇後は目標充填(60ms)まで無音で待って
    // レイテンシの緩衝を作る（primed ゲート）。不足時は無音でアンダーランを計上。
    std::atomic<bool> primed{false};
    auto pull = [&](float* dst, size_t need) {
        if (!primed.load()) {
            if (outFifo.readAvailable() >= kTargetFill) primed.store(true);
            else { for (size_t i = 0; i < need; i++) dst[i] = 0.f; return; }
        }
        size_t got = outFifo.pop(dst, need);
        if (got < need) {
            for (size_t i = got; i < need; i++) dst[i] = 0.f;
            underruns++;
            if (outFifo.readAvailable() == 0) primed.store(false); // 枯渇したら再プライム
        }
    };

    WasapiCapture capture;
    WasapiRender render;
    if (!capture.start(inDev, onFrames, &err)) { outw(L"[エラー] capture: %s\n", err.c_str()); return 5; }
    if (!render.start(outDev, pull, &err))     { outw(L"[エラー] render: %s\n", err.c_str()); return 6; }

    outw(L"[OK] AGC(音量自動調整)=%s\n", agc.enabled() ? L"ON" : L"OFF");

    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    outw(L"\n▶ %sを開始しました。通話アプリのマイクに「CABLE Output」を選んでください。\n",
        bypass ? L"素通し(bypass)" : L"ノイズ抑制");
    outw(L"  マイクに向かって話すと『入力』バーが振れ、Krispが声と判定した分だけ『出力』へ通ります。\n");
    outw(L"  停止するには Ctrl+C。\n\n");

    // 0..1e6(micro) を対数っぽい20段バーへ（小さな音も見えるよう感度高め）
    auto bar = [](int micro) {
        double v = micro / 1e6;              // 0..1
        int lv = 0;
        if (v > 0) { double db = 20.0 * log10(v); lv = (int)((db + 60.0) / 3.0); } // -60dB..0dB -> 0..20
        if (lv < 0) lv = 0; if (lv > 20) lv = 20;
        std::wstring s(lv, L'#'); s.append(20 - lv, L'.');
        return s;
    };
    bool warnedSilent = false;
    while (!g_stop) {
        Sleep(200);
        int ip = inPeakMicro.exchange(0), op = outPeakMicro.exchange(0);
        unsigned long long tf = capture.totalFrames(), sf = capture.silentFrames();
        outw(L"\r入力[%s]%.4f 出力[%s]%.4f | Fifo=%2dms UR=%llu 破棄=%llu ",
            bar(ip).c_str(), ip / 1e6, bar(op).c_str(), op / 1e6,
            (int)(outFifo.readAvailable() * 1000 / kSr),
            (unsigned long long)underruns.load(), (unsigned long long)drops.load());
        fflush(stdout);
        // マイクが常時 SILENT（ミュート/プライバシーでブロック）なら警告
        if (!warnedSilent && tf > kSr && sf == tf) {
            outw(L"\n[警告] マイクが常に無音(SILENT)を返しています。マイクのミュート、"
                 L"入力音量0、または Windows のマイクのプライバシー設定で"
                 L"「デスクトップアプリのマイクアクセス」が無効の可能性があります。\n");
            warnedSilent = true;
        }
    }

    outw(L"\n停止中...\n");
    capture.stop();
    render.stop();
    krisp.ncReset(sess);
    inDev->Release(); outDev->Release(); en->Release();
    outw(L"終了しました。\n");
    return 0;
}
