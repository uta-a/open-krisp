#include "krisp_shim.h"
#include <algorithm>
#include <cstring>   // memcmp / memcpy

constexpr uint8_t KrispShim::kSigCheckHead[3];

// %LOCALAPPDATA%\Discord*\app-*\modules\discord_krisp-*\discord_krisp\discord_krisp.node
// を全ブランチ・全バージョン走査し、更新日時の新しい順に返す。
std::vector<std::wstring> KrispShim::findModules() {
    std::vector<std::pair<FILETIME, std::wstring>> hits;
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};

    // Discord 系フォルダ列挙
    std::wstring base = local;
    WIN32_FIND_DATAW fd;
    HANDLE hBrand = FindFirstFileW((base + L"\\Discord*").c_str(), &fd);
    if (hBrand == INVALID_HANDLE_VALUE) return {};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring brand = base + L"\\" + fd.cFileName;

        // app-* を列挙
        WIN32_FIND_DATAW ad;
        HANDLE hApp = FindFirstFileW((brand + L"\\app-*").c_str(), &ad);
        if (hApp == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring modBase = brand + L"\\" + ad.cFileName + L"\\modules";

            // discord_krisp-* を列挙
            WIN32_FIND_DATAW kd;
            HANDLE hK = FindFirstFileW((modBase + L"\\discord_krisp-*").c_str(), &kd);
            if (hK == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(kd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring dir = modBase + L"\\" + kd.cFileName + L"\\discord_krisp";
                std::wstring node = dir + L"\\discord_krisp.node";
                WIN32_FILE_ATTRIBUTE_DATA st;
                if (GetFileAttributesExW(node.c_str(), GetFileExInfoStandard, &st) &&
                    !(st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    hits.push_back({ st.ftLastWriteTime, dir });
                }
            } while (FindNextFileW(hK, &kd));
            FindClose(hK);
        } while (FindNextFileW(hApp, &ad));
        FindClose(hApp);
    } while (FindNextFileW(hBrand, &fd));
    FindClose(hBrand);

    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return CompareFileTime(&a.first, &b.first) > 0;  // 新しい順
    });
    std::vector<std::wstring> dirs;
    for (auto& h : hits) dirs.push_back(h.second);
    return dirs;
}

// 署名検証トランポリンが t にあるか（形も文字列参照も確かめる）。
//   48 8D 15 <d1> : lea rdx,[rip+d1]  (d1 は "Discord Inc." を指す)
//   4C 8D 05 <d2> : lea r8,[rip+d2]
//   E9 <rel>      : jmp <本体>
static bool isSigTrampoline(uint8_t* t, uint8_t* strAddr) {
    if (t[0] != 0x48 || t[1] != 0x8D || t[2] != 0x15) return false;
    int32_t d1; memcpy(&d1, t + 3, 4);
    if (t + 7 + d1 != strAddr) return false;
    if (t[7] != 0x4C || t[8] != 0x8D || t[9] != 0x05) return false;
    return t[14] == 0xE9;
}

// ロード済みイメージから "Discord Inc." と署名検証トランポリンを探す。
// 固定 RVA が Discord の更新でズレても、この形は版が変わっても保たれるので
// 開けなくなるのを防げる。見つからなければ nullptr。
static uint8_t* findSigTrampoline(uint8_t* base) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const size_t size = nt->OptionalHeader.SizeOfImage;

    // 末尾 NUL まで含めて照合し、"Discord Inc." で始まる別文字列に当たらないようにする
    static const char kNeedle[] = "Discord Inc.";
    const size_t nlen = sizeof(kNeedle);   // NUL 込み 13 バイト
    uint8_t* strAddr = nullptr;
    for (size_t i = 0; i + nlen <= size; i++) {
        if (memcmp(base + i, kNeedle, nlen) == 0) { strAddr = base + i; break; }
    }
    if (!strAddr) return nullptr;

    for (size_t i = 0; i + 15 <= size; i++) {
        if (isSigTrampoline(base + i, strAddr)) return base + i;
    }
    return nullptr;
}

bool KrispShim::patchSigCheck(std::wstring* err) {
    uint8_t* base = reinterpret_cast<uint8_t*>(mod_);

    // "Discord Inc." の実在位置を先に押さえ、固定 RVA が本当にトランポリンかを
    // 文字列参照ごと検証する。先頭 3 バイト一致だけだと別関数を誤爆しうるため。
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    static const char kNeedle[] = "Discord Inc.";
    const size_t nlen = sizeof(kNeedle);
    uint8_t* strAddr = nullptr;
    for (size_t i = 0; i + nlen <= size; i++)
        if (memcmp(base + i, kNeedle, nlen) == 0) { strAddr = base + i; break; }

    uint8_t* t = base + kSigCheckRva;
    if (!(strAddr && isSigTrampoline(t, strAddr))) {
        // 固定 RVA が版ズレで合わない → イメージ全体から形で探し直す
        t = findSigTrampoline(base);
        if (!t) {
            if (err) *err = L"署名検証関数を特定できませんでした"
                            L"（discord_krisp.node のバージョン差異の可能性）。";
            return false;
        }
    }

    DWORD old;
    if (!VirtualProtect(t, 3, PAGE_EXECUTE_READWRITE, &old)) {
        if (err) *err = L"VirtualProtect に失敗しました。";
        return false;
    }
    t[0] = 0xB0; t[1] = 0x01; t[2] = 0xC3;  // mov al,1 ; ret
    VirtualProtect(t, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 3);
    return true;
}

// 抑制グローバルを解決する。固定 RVA の値が抑制レベルとして妥当(0-100)なときだけ
// 有効にする。版ズレでこの番地が別の用途に変わっていた場合、そこへ書くと無関係な
// 値を壊すので、その場合は無効のまま（強度スライダーが効かないだけで済ませる）。
void KrispShim::resolveSuppression() {
    supp_ = nullptr;
    if (!mod_) return;
    float* p = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mod_) + kSuppressionRva);
    const float v = *p;
    if (v >= 0.0f && v <= 100.0f) supp_ = p;
}

bool KrispShim::load(std::wstring* err) {
    auto mods = findModules();
    if (mods.empty()) {
        if (err) *err = L"discord_krisp.node が見つかりません（Discord 未インストール？）。";
        return false;
    }
    moduleDir_ = mods.front();
    SetDllDirectoryW(moduleDir_.c_str());
    std::wstring node = moduleDir_ + L"\\discord_krisp.node";
    mod_ = LoadLibraryW(node.c_str());
    if (!mod_) {
        if (err) *err = L"LoadLibrary に失敗しました: " + node;
        return false;
    }

    // 署名検証を無効化してから初期化する（順序が重要）
    if (!patchSigCheck(err)) return false;

    InitExternal_   = (InitExternal_t)   GetProcAddress(mod_, "KrispInitializeExternal");
    NCSetModel_     = (NCSetModel_t)      GetProcAddress(mod_, "KrispNCSetModel");
    NCSetup_        = (NCSetup_t)         GetProcAddress(mod_, "KrispNCSetup");
    NCSetup2_       = (NCSetup2_t)        GetProcAddress(mod_, "KrispNCSetup2");
    NCProcessFloat_ = (NCProcessFloat_t)  GetProcAddress(mod_, "KrispNCProcessFloat");
    NCReset_        = (NCReset_t)         GetProcAddress(mod_, "KrispNCReset");
    if (!InitExternal_ || !NCSetup2_ || !NCProcessFloat_ || !NCReset_) {
        if (err) *err = L"必要なエクスポート関数が見つかりません。";
        return false;
    }

    int r = InitExternal_();
    if (r != 0) {
        if (err) *err = L"Krisp 初期化に失敗しました（コード " + std::to_wstring(r) + L"）。";
        return false;
    }
    resolveSuppression();   // 抑制グローバルの番地を検査（誤番地なら無効化）
    return true;
}
