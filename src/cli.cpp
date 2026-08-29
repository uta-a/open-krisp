#include "cli.h"
#include "engine.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <atomic>

// ワイド文字列を UTF-8 に変換して出力する（コンソール直・リダイレクト両対応）。
static void outw(const wchar_t* fmt, ...) {
    wchar_t wbuf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, fmt, ap);
    va_end(ap);
    char u8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8, sizeof(u8), nullptr, nullptr);
    if (n > 0) fputs(u8, stdout);
}

const wchar_t* argVal(int argc, wchar_t** argv, const wchar_t* key, const wchar_t* def) {
    for (int i = 1; i < argc - 1; i++)
        if (wcscmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}

bool hasFlag(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; i++) if (wcscmp(argv[i], key) == 0) return true;
    return false;
}

static std::atomic<bool> g_stop{false};
static BOOL WINAPI ctrlHandler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT) { g_stop = true; return TRUE; }
    return FALSE;
}

int runList(AudioEngine& eng) {
    outw(L"=== 入力(マイク) ===\n");
    int i = 0; for (auto& d : eng.devices(eCapture)) outw(L"  [%d] %s\n", i++, d.name.c_str());
    outw(L"=== 出力 ===\n");
    i = 0; for (auto& d : eng.devices(eRender)) outw(L"  [%d] %s\n", i++, d.name.c_str());
    return 0;
}

// 0..1e6(micro) を対数っぽい20段バーへ（小さな音も見えるよう感度高め）
static std::wstring bar(int micro) {
    double v = micro / 1e6;
    int lv = 0;
    if (v > 0) { double db = 20.0 * log10(v); lv = (int)((db + 60.0) / 3.0); } // -60dB..0dB
    if (lv < 0) lv = 0; if (lv > 20) lv = 20;
    std::wstring s((size_t)lv, L'#');
    s.append((size_t)(20 - lv), L'.');
    return s;
}

int runCli(int argc, wchar_t** argv, AudioEngine& eng) {
    std::wstring err;
    if (!eng.initKrisp(&err)) { outw(L"[エラー] %s\n", err.c_str()); return 1; }
    outw(L"[OK] Krisp モジュール: %s\n", eng.moduleDir().c_str());

    EngineConfig cfg;
    cfg.durationMs = _wtoi(argVal(argc, argv, L"--duration", L"10"));
    if (!validDuration(cfg.durationMs)) {
        outw(L"[警告] --duration は 10/15/20/30/32 のみ対応。10 に戻します。\n");
        cfg.durationMs = 10;
    }
    const wchar_t* suppArg = argVal(argc, argv, L"--suppression", L"");
    if (suppArg[0]) {
        cfg.suppression = (float)_wtof(suppArg);
        if (cfg.suppression < 0) cfg.suppression = 0;
        if (cfg.suppression > 100) cfg.suppression = 100;
        outw(L"[OK] 抑制レベル=%.0f\n", cfg.suppression);
    }
    cfg.inMatch  = argVal(argc, argv, L"--in", L"");
    cfg.outMatch = argVal(argc, argv, L"--out", L"CABLE Input");
    cfg.bypass   = hasFlag(argc, argv, L"--bypass");
    cfg.muted    = hasFlag(argc, argv, L"--mute");
    cfg.agcEnabled = !hasFlag(argc, argv, L"--no-agc");
    const wchar_t* tArg = argVal(argc, argv, L"--agc-target", L"");
    if (tArg[0]) {
        float tv = (float)_wtof(tArg);
        if (tv > 0 && tv <= 1.0f) cfg.agcTarget = tv;
    }

    // 指定されたデバイスが無い場合は、第1版と同じ終了コードで落とす。
    // （engine 側は既定へフォールバックするので、ここで先に検査する）
    if (!cfg.inMatch.empty() && !eng.hasDevice(eCapture, cfg.inMatch)) {
        outw(L"[エラー] 入力デバイスが見つかりません（--in で指定）\n");
        return 3;
    }
    if (!eng.hasDevice(eRender, cfg.outMatch)) {
        outw(L"[エラー] 出力デバイス '%s' が見つかりません（--list で確認）\n", cfg.outMatch.c_str());
        return 4;
    }

    if (!eng.start(cfg, &err)) { outw(L"[エラー] %s\n", err.c_str()); return 5; }

    EngineStats st = eng.stats();
    outw(L"[OK] NC セッション確立 (48kHz, %dms/%zuサンプル, model=full_NC)\n",
         cfg.durationMs, (size_t)(48 * cfg.durationMs));
    outw(L"[OK] 入力: %s\n", st.inName.c_str());
    outw(L"[OK] 出力: %s\n", st.outName.c_str());
    outw(L"[OK] AGC(音量自動調整)=%s\n", cfg.agcEnabled ? L"ON" : L"OFF");
    // 既定 OFF なので ON のときだけ知らせる。既定起動の出力を増やさないため。
    if (cfg.muted) outw(L"[OK] ミュート=ON（出力は無音）\n");

    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    outw(L"\n▶ %sを開始しました。通話アプリのマイクに「CABLE Output」を選んでください。\n",
        cfg.bypass ? L"素通し(bypass)" : L"ノイズ抑制");
    outw(L"  マイクに向かって話すと『入力』バーが振れ、Krispが声と判定した分だけ『出力』へ通ります。\n");
    outw(L"  停止するには Ctrl+C。\n\n");

    bool warnedSilent = false;
    while (!g_stop) {
        Sleep(200);
        EngineStats s = eng.stats();
        int ip = (int)(s.inPeak * 1e6f), op = (int)(s.outPeak * 1e6f);
        outw(L"\r入力[%s]%.4f 出力[%s]%.4f | Fifo=%2dms UR=%llu 破棄=%llu ",
            bar(ip).c_str(), ip / 1e6, bar(op).c_str(), op / 1e6,
            s.fifoMs, s.underruns, s.drops);
        fflush(stdout);
        // マイクが常時 SILENT（ミュート/プライバシーでブロック）なら警告
        if (!warnedSilent && s.totalFrames > 48000 && s.silentFrames == s.totalFrames) {
            outw(L"\n[警告] マイクが常に無音(SILENT)を返しています。マイクのミュート、"
                 L"入力音量0、または Windows のマイクのプライバシー設定で"
                 L"「デスクトップアプリのマイクアクセス」が無効の可能性があります。\n");
            warnedSilent = true;
        }
    }

    outw(L"\n停止中...\n");
    eng.stop();
    outw(L"終了しました。\n");
    return 0;
}
