#include "krisp_shim.h"
#include <algorithm>

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

bool KrispShim::patchSigCheck(std::wstring* err) {
    uint8_t* t = reinterpret_cast<uint8_t*>(mod_) + kSigCheckRva;
    // 想定バイト列でなければ、別バージョンの可能性があるので中止（安全側）
    if (t[0] != kSigCheckHead[0] || t[1] != kSigCheckHead[1] || t[2] != kSigCheckHead[2]) {
        if (err) *err = L"署名検証関数の先頭バイトが想定と異なります"
                        L"（discord_krisp.node のバージョン差異の可能性）。";
        return false;
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
    return true;
}
