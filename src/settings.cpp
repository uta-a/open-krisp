#include "settings.h"
#include <windows.h>
#include <map>
#include <vector>
#include <cstdio>

std::wstring settingsPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"openkrisp.ini";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"openkrisp.ini";
    return p.substr(0, slash + 1) + L"openkrisp.ini";
}

static std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// UTF-8 のファイルを丸ごと読んでワイド文字列にする。BOM は読み飛ばす。
static bool readFileUtf8(const std::wstring& path, std::wstring* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (1 << 20)) { CloseHandle(h); return false; }
    std::vector<char> bytes((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = bytes.empty() ||
              (ReadFile(h, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size());
    CloseHandle(h);
    if (!ok) return false;

    const char* p = bytes.data();
    size_t n = bytes.size();
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) { p += 3; n -= 3; }
    if (n == 0) { out->clear(); return true; }

    int wn = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
    if (wn <= 0) return false;
    out->assign((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, (int)n, &(*out)[0], wn);
    return true;
}

bool loadSettings(Settings* out) {
    std::wstring text;
    if (!readFileUtf8(settingsPath(), &text)) return false;

    // "section.key" -> value。値中の ; は落とさない（デバイス名を壊さないため、
    // 行コメントは行頭の ; / # のみ）。
    std::map<std::wstring, std::wstring> kv;
    std::wstring section, line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        line = trim(text.substr(pos, (nl == std::wstring::npos ? text.size() : nl) - pos));
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        kv[section + L"." + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    if (kv.empty()) return false;

    auto str = [&](const wchar_t* k, std::wstring& dst) {
        auto it = kv.find(k); if (it != kv.end()) dst = it->second;
    };
    auto num = [&](const wchar_t* k, float& dst) {
        auto it = kv.find(k); if (it != kv.end()) dst = (float)_wtof(it->second.c_str());
    };
    auto integer = [&](const wchar_t* k, int& dst) {
        auto it = kv.find(k); if (it != kv.end()) dst = _wtoi(it->second.c_str());
    };
    auto boolean = [&](const wchar_t* k, bool& dst) {
        auto it = kv.find(k);
        if (it != kv.end()) dst = (it->second == L"1" || it->second == L"true");
    };

    Settings s;
    str(L"device.input",  s.cfg.inMatch);
    str(L"device.output", s.cfg.outMatch);
    integer(L"audio.duration",   s.cfg.durationMs);
    num(L"audio.suppression",    s.cfg.suppression);
    boolean(L"audio.bypass",     s.cfg.bypass);
    boolean(L"audio.mute",       s.cfg.muted);
    boolean(L"audio.agc",        s.cfg.agcEnabled);
    num(L"audio.agc_target",     s.cfg.agcTarget);
    boolean(L"ui.ascii",         s.ascii);

    // 壊れた値で起動しないよう、ここで正気の範囲へ丸める。
    if (!validDuration(s.cfg.durationMs)) s.cfg.durationMs = 10;
    if (s.cfg.suppression < 0.f)   s.cfg.suppression = 0.f;
    if (s.cfg.suppression > 100.f) s.cfg.suppression = 100.f;
    if (s.cfg.agcTarget < 0.01f)   s.cfg.agcTarget = 0.01f;
    if (s.cfg.agcTarget > 0.50f)   s.cfg.agcTarget = 0.50f;

    *out = s;
    return true;
}

static bool writeFileUtf8(const std::wstring& path, const std::wstring& text, std::wstring* err) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                nullptr, 0, nullptr, nullptr);
    std::vector<char> bytes(3 + (size_t)(n > 0 ? n : 0));
    bytes[0] = (char)0xEF; bytes[1] = (char)0xBB; bytes[2] = (char)0xBF;  // メモ帳向けの BOM
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                            bytes.data() + 3, n, nullptr, nullptr);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"設定ファイルを開けません: " + path;
        return false;
    }
    DWORD wrote = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) &&
              wrote == bytes.size();
    CloseHandle(h);
    if (!ok && err) *err = L"設定ファイルの書き込みに失敗しました。";
    return ok;
}

bool saveSettings(const Settings& s, std::wstring* err) {
    wchar_t buf[256];
    std::wstring t;
    t += L"; OpenKrisp 設定ファイル\n";
    t += L"; TUI の S キーで上書き保存されます。引数付き起動では読まれません。\n";
    t += L"\n[device]\n";
    t += L"; フレンドリ名。起動時は部分一致（大文字小文字無視）で探します。\n";
    t += L"input=" + s.cfg.inMatch + L"\n";
    t += L"output=" + s.cfg.outMatch + L"\n";
    t += L"\n[audio]\n";
    t += L"; duration は 10/15/20/30/32 のみ、suppression は 0-100\n";
    t += L"; mute=1 で出力だけ無音にする（マイク自体は動いたまま）\n";
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
        L"duration=%d\nsuppression=%.0f\nbypass=%d\nmute=%d\nagc=%d\nagc_target=%.3f\n",
        s.cfg.durationMs, s.cfg.suppression, s.cfg.bypass ? 1 : 0, s.cfg.muted ? 1 : 0,
        s.cfg.agcEnabled ? 1 : 0, s.cfg.agcTarget);
    t += buf;
    t += L"\n[ui]\n";
    t += L"; ascii=1 で罫線とメーターを ASCII にする（枠がずれる端末向け）\n";
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"ascii=%d\n", s.ascii ? 1 : 0);
    t += buf;

    // 一時ファイルへ書いてから置換する。途中で失敗しても既存の ini は壊れない。
    std::wstring path = settingsPath();
    std::wstring tmp  = path + L".tmp";
    if (!writeFileUtf8(tmp, t, err)) return false;
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        if (err) *err = L"設定ファイルの置き換えに失敗しました。";
        return false;
    }
    return true;
}
