// discord_krisp.node のモジュール探索・ロード・署名検証回避パッチ・
// C API バインドをまとめたラッパ。
//
// 【重要・法的位置づけ】
//   Krisp SDK は Discord 向けにライセンスされたもので、本ラッパは Discord が実行体に
//   要求する署名検証（"Discord Inc." 署名）をメモリ上で一時無効化して初期化を通す。
//   これは Krisp の技術的保護手段の回避に当たる。自分のマシン内での個人利用に限定し、
//   モジュールの複製・再配布は行わないこと。
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

class KrispShim {
public:
    // C API 型
    typedef int   (*InitExternal_t)();
    typedef void  (*NCSetModel_t)(const char* model);
    typedef void* (*NCSetup_t)(int sampleRate, int durationMs);
    // 第4引数 engine=1 が必須。engine=0（KrispNCSetup 既定）だと ProcessFloat が
    // 検索するマップとは別のマップにセッションが登録され、処理対象が見つからず
    // 無音になる（Ghidra 解析で判明。WORKLOG.md 参照）。
    typedef void* (*NCSetup2_t)(int sampleRate, int durationMs, int flag, int engine);
    typedef int   (*NCProcessFloat_t)(void* session, const float* in, size_t inN,
                                      float* out, size_t outN);
    typedef void  (*NCReset_t)(void* session);

    // 署名検証関数 sub_53110 の RVA。将来のバージョン差異はシグネチャ探索で吸収予定。
    // 先頭が想定バイト列でない場合はパッチを中止して失敗させる（誤爆防止）。
    static const uintptr_t kSigCheckRva = 0x53110;
    static constexpr uint8_t kSigCheckHead[3] = { 0x48, 0x8D, 0x15 }; // lea rdx,[rip+..]

    bool load(std::wstring* err);           // モジュール探索→ロード→パッチ→初期化
    // engine=1 で呼ぶ（上記 NCSetup2_t のコメント参照）。
    void* ncSetup(int sr, int durMs) { return NCSetup2_(sr, durMs, 0, 1); }
    int   ncProcess(void* s, const float* in, size_t inN, float* out, size_t outN) {
        return NCProcessFloat_(s, in, inN, out, outN);
    }
    void  ncReset(void* s) { NCReset_(s); }
    void  ncSetModel(const char* m) { if (NCSetModel_) NCSetModel_(m); }

    // 抑制レベル(0-100)。ProcessFloat が毎回参照するグローバル(既定100=最大)を書き換える。
    // 本バージョン固定 RVA。バージョン差異では効かない場合があるが致命的ではない。
    static const uintptr_t kSuppressionRva = 0xDA20B0;
    void setSuppression(float level) {
        if (mod_) *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(mod_) + kSuppressionRva) = level;
    }

    const std::wstring& moduleDir() const { return moduleDir_; }

    // Discord 各ブランチの discord_krisp.node を新しい順に探索して返す。
    static std::vector<std::wstring> findModules();

private:
    HMODULE mod_ = nullptr;
    std::wstring moduleDir_;
    InitExternal_t    InitExternal_ = nullptr;
    NCSetModel_t      NCSetModel_ = nullptr;
    NCSetup_t         NCSetup_ = nullptr;
    NCSetup2_t        NCSetup2_ = nullptr;
    NCProcessFloat_t  NCProcessFloat_ = nullptr;
    NCReset_t         NCReset_ = nullptr;

    bool patchSigCheck(std::wstring* err);
};
