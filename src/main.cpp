// OpenKrisp : Discord 同梱の Krisp NC を使い、任意マイクの入力をノイズ除去して
// 仮想オーディオケーブル(既定: VB-CABLE の "CABLE Input")へ流す常駐ブリッジ。
//
// チェーン: [実マイク] → 本アプリ(Krisp NC) → CABLE Input →(仮想)→ CABLE Output → 通話アプリ
//
// 起動方法:
//   openkrisp                       TUI（すべての操作を画面上で行う）
//   openkrisp --list                入出力デバイス一覧
//   openkrisp --in fifine --out "CABLE Input"        以下ヘッドレス動作
//   openkrisp --suppression 100     抑制強度(0-100, 既定100)
//   openkrisp --duration 20         Krisp フレーム長ms(10/15/20/30/32, 既定10)
//   openkrisp --no-agc              AGC(音量自動調整)を無効化（既定は ON）
//   openkrisp --agc-target 0.10     AGC の目標音量(0..1, 既定0.12)
//   openkrisp --mute                ミュート状態で開始（出力に無音を流す）
//   openkrisp --bypass              Krisp を素通し（切り分け用）
//
// 引数を 1 つでも付けると従来どおりのヘッドレス動作になる（スクリプトからの
// 無人起動用）。引数なしなら TUI が開く。
//
// 法的注意: 本ツールは Krisp の署名検証を回避する。個人利用限定・再配布禁止。
#include "engine.h"
#include "cli.h"
#include "tui_app.h"
#include <windows.h>
#include <cstdio>

#pragma comment(lib, "ole32.lib")

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    int rc = 0;
    // engine は CoUninitialize より先に壊す必要がある（IMMDevice を持っているため）。
    // そのためにスコープで囲む。
    {
        AudioEngine eng;
        std::wstring err;
        if (!eng.initDevices(&err)) {
            fputs("[error] audio device enumeration failed\n", stderr);
            CoUninitialize();
            return 2;
        }

        if (hasFlag(argc, argv, L"--list")) {
            rc = runList(eng);
        } else if (hasFlag(argc, argv, L"--uitest") && hasFlag(argc, argv, L"--stress")) {
            // 「デバイス一覧を開くと固まる」不具合の再現テスト。
            rc = runScreenStress();
        } else if (hasFlag(argc, argv, L"--uitest")) {
            // 画面の桁ずれ確認用。Krisp もオーディオも起動しない。
            rc = runUiTest(eng,
                           _wtoi(argVal(argc, argv, L"--cols", L"64")),
                           _wtoi(argVal(argc, argv, L"--rows", L"18")),
                           hasFlag(argc, argv, L"--ascii"),
                           hasFlag(argc, argv, L"--picker"));
        } else if (argc > 1) {
            rc = runCli(argc, argv, eng);
        } else {
            rc = runTui(eng);
        }
        fflush(stdout);
    }

    CoUninitialize();
    return rc;
}
