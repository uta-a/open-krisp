#include "tui_screen.h"
#include <algorithm>

// --- 文字幅 ---------------------------------------------------------------
namespace {
struct Range { wchar_t lo, hi; };

// East Asian Wide / Fullwidth。日本語 UI で実際に出る範囲を押さえてある。
const Range kWide[] = {
    { 0x1100, 0x115F }, { 0x2E80, 0x303E }, { 0x3041, 0x33FF },
    { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xA000, 0xA4CF },
    { 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF },
    { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 },
    { 0xFFE0, 0xFFE6 },
};
// 結合文字・幅ゼロ
const Range kZero[] = {
    { 0x0300, 0x036F }, { 0x200B, 0x200F }, { 0x2028, 0x202E },
    { 0xFE00, 0xFE0F }, { 0xFEFF, 0xFEFF },
};

bool inRanges(wchar_t c, const Range* r, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < r[mid].lo)      hi = mid;
        else if (c > r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}
} // namespace

int charWidth(wchar_t c) {
    if (c == 0) return 0;
    if (c < 0x0300) return (c < 0x20) ? 0 : 1;          // ASCII と制御文字
    if (inRanges(c, kZero, sizeof(kZero) / sizeof(kZero[0]))) return 0;
    // サロゲート: 上位を 2 桁、下位を 0 桁として近似する（絵文字は 2 桁）
    if (c >= 0xD800 && c <= 0xDBFF) return 2;
    if (c >= 0xDC00 && c <= 0xDFFF) return 0;
    if (inRanges(c, kWide, sizeof(kWide) / sizeof(kWide[0]))) return 2;
    return 1;
}

int strWidth(const std::wstring& s) {
    int w = 0;
    for (wchar_t c : s) w += charWidth(c);
    return w;
}

std::wstring truncWidth(const std::wstring& s, int maxW, const wchar_t* ell) {
    if (maxW <= 0) return L"";
    if (strWidth(s) <= maxW) return s;
    const int ew = strWidth(ell);
    const int limit = (maxW > ew) ? maxW - ew : 0;
    std::wstring out;
    int w = 0;
    for (wchar_t c : s) {
        int cw = charWidth(c);
        if (w + cw > limit) break;
        out.push_back(c);
        w += cw;
    }
    out += ell;
    return out;
}

// --- 字形 -----------------------------------------------------------------
const Glyphs& glyphSet(bool ascii) {
    static const Glyphs uni = {
        L"┌", L"┐", L"└", L"┘", L"─", L"│", L"├", L"┤",
        L"█", L"░", L"▸", L"…", L"↑", L"↓", L"←", L"→"
    };
    static const Glyphs asc = {
        L"+", L"+", L"+", L"+", L"-", L"|", L"+", L"+",
        L"#", L".", L">", L"..", L"^", L"v", L"<", L">"
    };
    return ascii ? asc : uni;
}

// --- 画面 -----------------------------------------------------------------
void Screen::resize(int cols, int rows) {
    if (cols == cols_ && rows == rows_) return;
    cols_ = cols; rows_ = rows;
    back_.assign((size_t)cols_ * rows_, Cell{});
    front_.assign((size_t)cols_ * rows_, Cell{});
    full_ = true;
}

void Screen::begin() {
    for (auto& c : back_) c = Cell{};
}

int Screen::put(int x, int y, const wchar_t* s, uint16_t attr) {
    return put(x, y, std::wstring(s), attr);
}

int Screen::put(int x, int y, const std::wstring& s, uint16_t attr) {
    if (y < 0 || y >= rows_) return x;
    for (wchar_t c : s) {
        int w = charWidth(c);
        if (w == 0) continue;                 // 結合文字は落とす（枠がずれるのを防ぐ）
        if (x >= cols_) break;
        if (x + w > cols_) {                  // 全角が右端をはみ出す場合は空白で埋める
            back_[(size_t)y * cols_ + x] = Cell{ L' ', attr, 1 };
            x += 1;
            break;
        }
        if (x >= 0) {
            back_[(size_t)y * cols_ + x] = Cell{ c, attr, (uint8_t)w };
            if (w == 2) back_[(size_t)y * cols_ + x + 1] = Cell{ L' ', attr, 0 };
        }
        x += w;
    }
    return x;
}

void Screen::fill(int x, int y, int w, const wchar_t* s, uint16_t attr) {
    std::wstring t;
    int need = 0;
    const int sw = strWidth(s);
    if (sw <= 0) return;
    while (need < w) { t += s; need += sw; }
    put(x, y, truncWidth(t, w, L""), attr);
}

void Screen::box(int x, int y, int w, int h, uint16_t attr) {
    if (w < 2 || h < 2) return;
    const Glyphs& gl = g();
    put(x, y, gl.tl, attr);
    fill(x + 1, y, w - 2, gl.h, attr);
    put(x + w - 1, y, gl.tr, attr);
    for (int i = 1; i < h - 1; i++) {
        put(x, y + i, gl.v, attr);
        put(x + w - 1, y + i, gl.v, attr);
    }
    put(x, y + h - 1, gl.bl, attr);
    fill(x + 1, y + h - 1, w - 2, gl.h, attr);
    put(x + w - 1, y + h - 1, gl.br, attr);
}

void Screen::hsep(int x, int y, int w, uint16_t attr) {
    if (w < 2) return;
    const Glyphs& gl = g();
    put(x, y, gl.ml, attr);
    fill(x + 1, y, w - 2, gl.h, attr);
    put(x + w - 1, y, gl.mr, attr);
}

// 属性を SGR シーケンスへ。属性が変わるところでだけ出す。
static void appendSgr(std::wstring& o, uint16_t a) {
    o += L"\x1b[0";
    if (a & ATTR_DIM)    o += L";2";
    if (a & ATTR_BOLD)   o += L";1";
    if (a & ATTR_REV)    o += L";7";
    if (a & ATTR_CYAN)   o += L";36";
    if (a & ATTR_GREEN)  o += L";32";
    if (a & ATTR_YELLOW) o += L";33";
    if (a & ATTR_RED)    o += L";31";
    o += L"m";
}

void Screen::flush(HANDLE hOut) {
    if (cols_ <= 0 || rows_ <= 0) return;
    std::wstring o;
    o.reserve((size_t)cols_ * 8);
    if (full_) o += L"\x1b[2J";

    for (int y = 0; y < rows_; y++) {
        int x = 0;
        while (x < cols_) {
            const size_t i = (size_t)y * cols_ + x;
            if (!full_ && back_[i].ch == front_[i].ch && back_[i].attr == front_[i].attr) {
                x++; continue;
            }
            // 差分の始まりが全角の右半分なら 1 桁戻して、字の途中から書かないようにする
            int s = x;
            if (back_[i].w == 0 && s > 0) s--;
            // 差分が続く範囲を求める
            int e = s;
            while (e < cols_) {
                const size_t j = (size_t)y * cols_ + e;
                if (!full_ && back_[j].ch == front_[j].ch && back_[j].attr == front_[j].attr) break;
                e++;
            }
            wchar_t pos[24];
            _snwprintf_s(pos, _countof(pos), _TRUNCATE, L"\x1b[%d;%dH", y + 1, s + 1);
            o += pos;
            uint16_t cur = 0xFFFF;
            for (int k = s; k < e; k++) {
                const Cell& c = back_[(size_t)y * cols_ + k];
                if (c.w == 0) continue;                   // 全角の右半分は出力しない
                if (c.attr != cur) { appendSgr(o, c.attr); cur = c.attr; }
                o += c.ch;
            }
            o += L"\x1b[0m";
            x = e;
        }
    }
    front_ = back_;
    full_ = false;

    if (!o.empty()) {
        DWORD wrote = 0;
        WriteConsoleW(hOut, o.c_str(), (DWORD)o.size(), &wrote, nullptr);
    }
}

std::wstring Screen::dumpText() const {
    std::wstring o;
    for (int y = 0; y < rows_; y++) {
        for (int x = 0; x < cols_; x++) {
            const Cell& c = back_[(size_t)y * cols_ + x];
            if (c.w == 0) continue;              // 全角の右半分は 1 文字で表現済み
            o += c.ch;
        }
        // 行末の空白は落とす（桁ずれの確認に邪魔なため）
        while (!o.empty() && o.back() == L' ') o.pop_back();
        o += L'\n';
    }
    return o;
}

// --- 端末 -----------------------------------------------------------------
bool Term::enter(std::wstring* err) {
    hOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn_  = GetStdHandle(STD_INPUT_HANDLE);
    if (hOut_ == INVALID_HANDLE_VALUE || hIn_ == INVALID_HANDLE_VALUE) {
        if (err) *err = L"コンソールのハンドルを取得できません。";
        return false;
    }
    if (!GetConsoleMode(hOut_, &savedOut_) || !GetConsoleMode(hIn_, &savedIn_)) {
        if (err) *err = L"コンソールに接続されていません（リダイレクト中？）。";
        return false;
    }
    if (!SetConsoleMode(hOut_, savedOut_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                                         | DISABLE_NEWLINE_AUTO_RETURN)) {
        if (err) *err = L"この端末は VT シーケンスに対応していません。";
        return false;
    }
    modeOutSet_ = true;
    // ENABLE_WINDOW_INPUT: リサイズ検出に必要
    // ENABLE_PROCESSED_INPUT: 残す（Ctrl+C は CTRL_C_EVENT として受ける）
    // QUICK_EDIT / MOUSE は落とす（クリックで画面が固まるのを防ぐ）
    if (!SetConsoleMode(hIn_, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT
                                                    | ENABLE_PROCESSED_INPUT)) {
        SetConsoleMode(hOut_, savedOut_);
        modeOutSet_ = false;
        if (err) *err = L"コンソールの入力モードを設定できません。";
        return false;
    }
    modeInSet_ = true;

    hWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const wchar_t* init = L"\x1b[?1049h\x1b[?25l\x1b[2J";
    DWORD wrote = 0;
    WriteConsoleW(hOut_, init, (DWORD)wcslen(init), &wrote, nullptr);
    entered_ = true;
    left_.store(false);
    return true;
}

void Term::leave() {
    if (!entered_) return;
    if (left_.exchange(true)) return;         // 多重呼び出し（Ctrl+C ハンドラ）に耐える
    const wchar_t* fin = L"\x1b[0m\x1b[?25h\x1b[?1049l";
    DWORD wrote = 0;
    WriteConsoleW(hOut_, fin, (DWORD)wcslen(fin), &wrote, nullptr);
    if (modeOutSet_) SetConsoleMode(hOut_, savedOut_);
    if (modeInSet_)  SetConsoleMode(hIn_, savedIn_);
}

void Term::wake() {
    if (hWake_) SetEvent(hWake_);
}

void Term::size(int* cols, int* rows) const {
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo(hOut_, &bi)) { *cols = 80; *rows = 25; return; }
    // dwSize はスクロールバッファの大きさなので、見えている窓の大きさを使う
    *cols = bi.srWindow.Right - bi.srWindow.Left + 1;
    *rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
}

bool Term::poll(TermEvent* ev, DWORD timeoutMs) {
    if (!queue_.empty()) {
        *ev = queue_.front();
        queue_.erase(queue_.begin());
        return true;
    }
    HANDLE h[2] = { hIn_, hWake_ };
    DWORD n = hWake_ ? 2 : 1;
    DWORD r = WaitForMultipleObjects(n, h, FALSE, timeoutMs);
    if (r == WAIT_TIMEOUT) return false;
    if (r == WAIT_OBJECT_0 + 1) return false;   // wake: 再描画だけして戻る

    // signaled のまま残すと WaitForMultipleObjects が即返り続けるので、必ず読み切る。
    INPUT_RECORD rec[64];
    DWORD got = 0;
    if (!ReadConsoleInputW(hIn_, rec, 64, &got)) return false;
    for (DWORD i = 0; i < got; i++) {
        TermEvent e;
        if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
            // リサイズは何回来ても最後の1回で足りる
            if (!queue_.empty() && queue_.back().type == TermEvent::Resize) continue;
            e.type = TermEvent::Resize;
        } else if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown) {
            const KEY_EVENT_RECORD& k = rec[i].Event.KeyEvent;
            if (k.wVirtualKeyCode == VK_SHIFT || k.wVirtualKeyCode == VK_CONTROL ||
                k.wVirtualKeyCode == VK_MENU) continue;
            e.type  = TermEvent::Key;
            e.vk    = k.wVirtualKeyCode;
            e.ch    = k.uChar.UnicodeChar;
            e.ctrl  = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
            e.shift = (k.dwControlKeyState & SHIFT_PRESSED) != 0;
        } else {
            continue;   // フォーカス・メニュー・マウスは無視
        }
        queue_.push_back(e);
    }
    if (queue_.empty()) return false;
    *ev = queue_.front();
    queue_.erase(queue_.begin());
    return true;
}
