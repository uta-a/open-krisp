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
#include "settings.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

// 開き直したことを子プロセスへ伝える環境変数。コマンドライン引数で渡すと
// 「引数があればヘッドレス」の判定に引っかかるため、環境変数を使う。
static const wchar_t* kRelaunchFlag = L"OPENKRISP_CONHOST_CHILD";

// 本物の conhost のウィンドウにぶら下がっているか。
// Windows Terminal や VS Code のターミナル配下では ConPTY の擬似ウィンドウ
// （クラス名が違う）が返り、ウィンドウのスタイルを触っても効かない。
static bool inRealConhost() {
    HWND h = GetConsoleWindow();
    if (!h) return false;
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, (int)(sizeof(cls) / sizeof(cls[0])))) return false;
    return wcscmp(cls, L"ConsoleWindowClass") == 0;
}

// conhost.exe の子として自分を開き直す。成功したら true（呼び出し元は即終了する）。
// ウィンドウサイズを固定するには本物のコンソールウィンドウが要るため。
static bool relaunchInConhost() {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    wchar_t sysdir[MAX_PATH];
    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) return false;
    std::wstring conhost = std::wstring(sysdir) + L"\\conhost.exe";

    // conhost.exe に「起動してほしいコマンドライン」をそのまま渡す
    std::wstring cmd = L"\"" + conhost + L"\" \"" + std::wstring(self) + L"\"";

    if (!SetEnvironmentVariableW(kRelaunchFlag, L"1")) return false;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableW(kRelaunchFlag, nullptr);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    // TUI を開くときだけ、必要なら本物のコンソールウィンドウで開き直す。
    // ヘッドレス起動は呼び出し元の端末で動かしたいので対象外。
    // 開き直すかどうかは ini で切れるようにしてある（[ui] conhost=0）。
    // 判定が引数解析より前に要るので、ここで設定を先読みする。
    Settings pre;
    loadSettings(&pre);
    if (argc == 1 && pre.useConhost && !inRealConhost() &&
        GetEnvironmentVariableW(kRelaunchFlag, nullptr, 0) == 0) {
        if (relaunchInConhost()) {
            fputs("ウィンドウサイズを固定するため、別ウィンドウで開き直しました。\n", stdout);
            return 0;
        }
        // 開き直せなくてもこの端末で続行する（サイズは固定できない）
    }

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
            // 既定は実際に使う固定サイズ。ずれると崩れ方の確認にならない。
            const std::wstring dc = std::to_wstring(kFixedCols);
            const std::wstring dr = std::to_wstring(kFixedRows);
            rc = runUiTest(eng,
                           _wtoi(argVal(argc, argv, L"--cols", dc.c_str())),
                           _wtoi(argVal(argc, argv, L"--rows", dr.c_str())),
                           hasFlag(argc, argv, L"--ascii"),
                           hasFlag(argc, argv, L"--picker")  ? 1 :
                           hasFlag(argc, argv, L"--capture") ? 2 : 0);
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
