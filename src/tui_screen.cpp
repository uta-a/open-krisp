#include "tui_screen.h"
#include <algorithm>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// GetConsoleWindow のスタイルを触ってリサイズを塞ぐのに要る。
// CMakeLists.txt にリンク指定を足さずに済ませるため、ここで指定する。
#pragma comment(lib, "user32.lib")

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

// 全角セルの対を壊さないための後始末。
// 例: 全角で終わる行の最後の桁に半角の "↑" を重ね書きすると、左半分だけが
// w=2 のまま残る。この不整合があると flush() が走査位置を戻し続けて固まるので、
// 上書きの前に取り残される側を半角の空白へ潰しておく。
void Screen::breakWideAt(int x, int y) {
    if (y < 0 || y >= rows_ || x < 0 || x >= cols_) return;
    const size_t row = (size_t)y * cols_;
    // 自分が全角の右半分なら、対になっている左隣が取り残される
    if (back_[row + x].w == 0 && x > 0) {
        Cell& l = back_[row + x - 1];
        l = Cell{ L' ', l.attr, 1 };
    }
    // 右隣が全角の右半分なら、それは自分と対になっていた
    if (x + 1 < cols_ && back_[row + x + 1].w == 0) {
        Cell& r = back_[row + x + 1];
        r = Cell{ L' ', r.attr, 1 };
    }
}

int Screen::put(int x, int y, const std::wstring& s, uint16_t attr) {
    if (y < 0 || y >= rows_) return x;
    for (wchar_t c : s) {
        int w = charWidth(c);
        if (w == 0) continue;                 // 結合文字は落とす（枠がずれるのを防ぐ）
        if (x >= cols_) break;
        if (x + w > cols_) {                  // 全角が右端をはみ出す場合は空白で埋める
            if (x >= 0) {
                breakWideAt(x, y);
                back_[(size_t)y * cols_ + x] = Cell{ L' ', attr, 1 };
            }
            x += 1;
            break;
        }
        if (x >= 0) {
            breakWideAt(x, y);
            if (w == 2) breakWideAt(x + 1, y);
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

namespace {
struct Rgb { unsigned char r, g, b; };
struct Palette { Rgb bg, fg, dim, cyan, green, yellow, red; };

// 利用者の Windows Terminal のスキーム "Claude Dark"
const Palette kClaudeDark = {
    { 0x0D, 0x0D, 0x0F },   // background
    { 0xFA, 0xF9, 0xF5 },   // foreground
    { 0x6C, 0x6A, 0x64 },   // brightBlack
    { 0xB8, 0xCB, 0xD2 },   // brightCyan
    { 0x7F, 0xCB, 0x90 },   // brightGreen
    { 0xE8, 0xA5, 0x5A },   // brightYellow
    { 0xD9, 0x62, 0x62 },   // brightRed
};
// Windows PowerShell のコンソールの既定。淡色は背景が濃紺なので青寄りにする。
const Palette kPowerShell = {
    { 0x01, 0x24, 0x56 },
    { 0xEE, 0xED, 0xF0 },
    { 0x8A, 0x9B, 0xB8 },
    { 0x61, 0xD6, 0xD6 },
    { 0x16, 0xC6, 0x0C },
    { 0xF9, 0xF1, 0xA5 },
    { 0xE7, 0x48, 0x56 },
};

// conhost の既定のタイトルバー色を地色にしたもの。窓全体が一色になる。
// 淡色は背景が少し明るいぶん、Claude Dark より上げてある。
const Palette kConhost = {
    { 0x20, 0x20, 0x20 },
    { 0xFA, 0xF9, 0xF5 },
    { 0x7A, 0x78, 0x72 },
    { 0xB8, 0xCB, 0xD2 },
    { 0x7F, 0xCB, 0x90 },
    { 0xE8, 0xA5, 0x5A },
    { 0xD9, 0x62, 0x62 },
};

// 画面は 1 つしかないので、選択中の配色はここに持つ。
Theme    g_theme = Theme::ClaudeDark;
const Palette* g_pal = &kClaudeDark;
} // namespace

void setTheme(Theme t) {
    g_theme = t;
    switch (t) {
    case Theme::PowerShell: g_pal = &kPowerShell; break;
    case Theme::ClaudeDark: g_pal = &kClaudeDark; break;
    default:                g_pal = &kConhost;    break;
    }
}

Theme currentTheme() { return g_theme; }

const wchar_t* themeName(Theme t) {
    switch (t) {
    case Theme::PowerShell: return L"powershell";
    case Theme::ClaudeDark: return L"claude-dark";
    default:                return L"conhost";
    }
}

bool parseTheme(const std::wstring& s, Theme* out) {
    if (_wcsicmp(s.c_str(), L"powershell") == 0)  { *out = Theme::PowerShell; return true; }
    if (_wcsicmp(s.c_str(), L"claude-dark") == 0) { *out = Theme::ClaudeDark; return true; }
    if (_wcsicmp(s.c_str(), L"conhost") == 0)     { *out = Theme::Conhost;    return true; }
    return false;
}

// 属性を SGR シーケンスへ。属性が変わるところでだけ出す。
static void appendSgr(std::wstring& o, uint16_t a) {
    const Palette& p = *g_pal;
    Rgb fg = p.fg, bg = p.bg;
    if (a & ATTR_DIM)    fg = p.dim;
    if (a & ATTR_CYAN)   fg = p.cyan;
    if (a & ATTR_GREEN)  fg = p.green;
    if (a & ATTR_YELLOW) fg = p.yellow;
    if (a & ATTR_RED)    fg = p.red;
    if (a & ATTR_REV)    { Rgb t = fg; fg = bg; bg = t; }
    wchar_t buf[72];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"\x1b[0%s;38;2;%u;%u;%u;48;2;%u;%u;%um",
                 (a & ATTR_BOLD) ? L";1" : L"",
                 fg.r, fg.g, fg.b, bg.r, bg.g, bg.b);
    o += buf;
}

// セルが前フレームと同一か。幅(w)まで見るのは、字と色が同じでも半角/全角の
// 別が変われば出力すべき桁数が変わるため。ここを見落とすと、対の壊れたセルが
// 「差分なし」と判定されて画面に残り続ける。
static bool sameCell(const Cell& a, const Cell& b) {
    return a.ch == b.ch && a.attr == b.attr && a.w == b.w;
}

void Screen::flush(HANDLE hOut) {
    if (cols_ <= 0 || rows_ <= 0) return;
    std::wstring o;
    o.reserve((size_t)cols_ * 8);
    // 全面再描画のときは、先に既定色を出してから消す。ESC[2J は「そのときの
    // 背景色」で塗るので、順序を逆にすると端末の既定色で塗られてしまう。
    if (full_) { appendSgr(o, ATTR_NONE); o += L"\x1b[2J"; }

    for (int y = 0; y < rows_; y++) {
        int x = 0;
        while (x < cols_) {
            const size_t i = (size_t)y * cols_ + x;
            if (!full_ && sameCell(back_[i], front_[i])) {
                x++; continue;
            }
            // 差分の始まりが全角の右半分なら 1 桁戻して、字の途中から書かないようにする
            int s = x;
            if (back_[i].w == 0 && s > 0) s--;
            // 差分が続く範囲を求める
            int e = s;
            while (e < cols_) {
                const size_t j = (size_t)y * cols_ + e;
                if (!full_ && sameCell(back_[j], front_[j])) break;
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
            // 走査位置は必ず前進させる。s を 1 桁戻したとき、戻した先が前フレームと
            // 同じだと内側のループが即 break して e == s == x-1 になり、そのまま
            // x = e とすると同じ桁を延々と調べ直して無限ループになる。put() 側で
            // セルの対は保つようにしてあるが、万一不整合が残ると描画スレッドごと
            // 固まってアプリが応答しなくなるため、ここでも必ず止めておく。
            if (e <= x) e = x + 1;
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
namespace {

// 窓とバッファをまとめて設定する。窓がバッファより大きい瞬間があると
// SetConsoleWindowInfo / SetConsoleScreenBufferSize は ERROR_INVALID_PARAMETER で
// 失敗するので、
//   (1) 窓を「今」と「目標」の小さい方まで縮める（縮小時は必ず窓が先）
//   (2) バッファを目標へ（拡大時はここで先に広がる）
//   (3) 窓を目標へ広げる
// の順に必ず通す。縦横で拡大と縮小が混ざっても、この順なら破綻しない。
// buf は winCols x winRows 以上であること。
bool setConsoleGeometry(HANDLE hOut, int winCols, int winRows, COORD buf) {
    if (winCols < 1 || winRows < 1) return false;
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo(hOut, &bi)) return false;
    const int curW = bi.srWindow.Right - bi.srWindow.Left + 1;
    const int curH = bi.srWindow.Bottom - bi.srWindow.Top + 1;

    SMALL_RECT shrunk = { 0, 0,
                          (SHORT)((std::min)(winCols, curW) - 1),
                          (SHORT)((std::min)(winRows, curH) - 1) };
    SetConsoleWindowInfo(hOut, TRUE, &shrunk);
    SetConsoleScreenBufferSize(hOut, buf);
    SMALL_RECT win = { 0, 0, (SHORT)(winCols - 1), (SHORT)(winRows - 1) };
    return SetConsoleWindowInfo(hOut, TRUE, &win) != 0;
}

// このコンソールに繋がっているのが自分だけか。
// シェル(cmd/powershell)が同じコンソールに居れば、その窓は利用者のもので、
// スクロールバッファを縮めるとそれまでの出力履歴が消えてしまう。
// 自分しか居なければ（ダブルクリック起動や、こちらが開き直した窓）遠慮は要らない。
bool soleConsoleClient() {
    DWORD pids[8] = {};
    DWORD n = GetConsoleProcessList(pids, (DWORD)(sizeof(pids) / sizeof(pids[0])));
    return n == 1;
}

// XTWINOPS でのリサイズ要求。Windows Terminal / ConPTY はコンソール API では
// 動かず、こちらにだけ反応する。
void requestVtSize(HANDLE hOut, int cols, int rows) {
    wchar_t seq[32];
    _snwprintf_s(seq, _countof(seq), _TRUNCATE, L"\x1b[8;%d;%dt", rows, cols);
    DWORD wrote = 0;
    WriteConsoleW(hOut, seq, (DWORD)wcslen(seq), &wrote, nullptr);
}

} // namespace

bool Term::applySize(int cols, int rows) {
    if (!hOut_ || cols <= 0 || rows <= 0) return false;
    int c = 0, r = 0;
    size(&c, &r);
    if (c == cols && r == rows) return true;   // 毎フレーム呼ばれても即戻る

    // (1) VT。ConPTY では端末側でリサイズしてから通知が返るまで数十 ms 遅れるので、
    //     ここで少し待つ。待たずにコンソール API へ進むと、VT が効いている端末にも
    //     二重で要求してしまう。
    requestVtSize(hOut_, cols, rows);
    for (int i = 0; i < 5; i++) {
        Sleep(10);
        size(&c, &r);
        if (c == cols && r == rows) return true;
    }

    // (2) コンソール API（conhost はこちらでしか変わらない）。
    //     画面に収まらない要求は必ず失敗するので、その場合は実サイズのまま使う。
    const COORD largest = GetLargestConsoleWindowSize(hOut_);
    if (largest.X > 0 && largest.Y > 0 && (cols > largest.X || rows > largest.Y)) {
        size(&c, &r);
        return false;
    }
    // 自分で開いた窓なら、スクロールバーが出ないようバッファを要求サイズ
    // ちょうどにする。利用者が既に使っていた窓では縮めない（それまで出力して
    // いた履歴が消えるため）。その場合はバッファが大きいままなので
    // スクロールバーが残るが、履歴を失うよりはましと考える。
    COORD buf = { (SHORT)cols, (SHORT)rows };
    if (!ownWindow_) {
        CONSOLE_SCREEN_BUFFER_INFO bi;
        if (GetConsoleScreenBufferInfo(hOut_, &bi)) {
            buf.X = (std::max)((SHORT)cols, bi.dwSize.X);
            buf.Y = (std::max)((SHORT)rows, bi.dwSize.Y);
        }
    }
    setConsoleGeometry(hOut_, cols, rows, buf);
    size(&c, &r);
    return c == cols && r == rows;
}

namespace {
// フォントを指定して、実際に反映されたかを読み戻して確かめる。
// conhost は HKLM\...\Console\TrueTypeFont に登録されたフォントしか受け付けず、
// それ以外を渡しても SetCurrentConsoleFontEx は成功を返したまま何も変えない。
// 戻り値だけ見ていると「効いたつもり」になるので必ず読み戻す。
bool trySetConsoleFont(HANDLE hOut, const std::wstring& face, int px) {
    CONSOLE_FONT_INFOEX f = {};
    f.cbSize = sizeof(f);
    f.nFont = 0;
    f.dwFontSize.X = 0;              // TrueType は高さだけ指定すれば幅は追従する
    f.dwFontSize.Y = (SHORT)px;
    f.FontFamily = 54;               // FF_MODERN | TMPF_VECTOR | TMPF_TRUETYPE
    f.FontWeight = 400;
    wcsncpy_s(f.FaceName, face.c_str(), _TRUNCATE);
    if (!SetCurrentConsoleFontEx(hOut, FALSE, &f)) return false;

    CONSOLE_FONT_INFOEX got = {};
    got.cbSize = sizeof(got);
    if (!GetCurrentConsoleFontEx(hOut, FALSE, &got)) return false;
    return _wcsicmp(got.FaceName, face.c_str()) == 0;
}
} // namespace

// フォントと透過を適用する。どちらも conhost のウィンドウにしか効かない。
// Windows Terminal 配下では GetConsoleWindow() が擬似ウィンドウを返すので
// 透過は無視され、フォントは端末側の設定が使われる。
void Term::applyStyle(const TermStyle& style) {
    if (!style.fontFace.empty() && style.fontSize > 0) {
        CONSOLE_FONT_INFOEX cur = {};
        cur.cbSize = sizeof(cur);
        if (GetCurrentConsoleFontEx(hOut_, FALSE, &cur)) { savedFont_ = cur; fontSaved_ = true; }

        appliedFont_ = style.fontFace;
        if (!trySetConsoleFont(hOut_, style.fontFace, style.fontSize)) {
            // 使えないフォントだった。既定のラスターフォントのままにすると
            // 罫線も日本語も潰れるので、必ず登録されている Consolas へ落とす。
            if (trySetConsoleFont(hOut_, L"Consolas", style.fontSize))
                appliedFont_ = L"Consolas";
            else
                appliedFont_.clear();   // 何も適用できなかった
        }
    }

    if (hwnd_ && style.opacity >= 0 && style.opacity < 100) {
        LONG ex = GetWindowLong(hwnd_, GWL_EXSTYLE);
        SetWindowLong(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        layered_ = true;
        // conhost 自身の透過(Ctrl+Shift+ホイール)と同じ仕組み。
        // 文字ごと透ける方式で、Windows Terminal のアクリルとは見え方が違う。
        SetLayeredWindowAttributes(hwnd_, 0,
                                   (BYTE)(style.opacity * 255 / 100), LWA_ALPHA);
    }
}

// タイトルバー・枠・キャプション文字を配色に合わせる。
// 既定のままだと Windows のタイトルバーの色と画面の背景色が食い違って、
// 窓の上だけ浮いて見えるため。
// Windows 11 (build 22000) 以降でのみ有効。それ以前は E_INVALIDARG が返るだけ。
void Term::syncWindowTheme() {
    if (!hwnd_) return;
    const Palette& p = *g_pal;
    const COLORREF bg = RGB(p.bg.r, p.bg.g, p.bg.b);
    const COLORREF fg = RGB(p.fg.r, p.fg.g, p.fg.b);
    // dwmapi.h のバージョンによっては未定義なので数値で渡す
    const DWORD kBorderColor  = 34;   // DWMWA_BORDER_COLOR
    const DWORD kCaptionColor = 35;   // DWMWA_CAPTION_COLOR
    const DWORD kTextColor    = 36;   // DWMWA_TEXT_COLOR
    DwmSetWindowAttribute(hwnd_, kCaptionColor, &bg, sizeof(bg));
    DwmSetWindowAttribute(hwnd_, kBorderColor,  &bg, sizeof(bg));
    DwmSetWindowAttribute(hwnd_, kTextColor,    &fg, sizeof(fg));
    themedWindow_ = true;
}

// 窓のアイコンを自分の exe に埋めたものに差し替える。
// conhost で開き直すと窓は conhost.exe のものになり、そのままでは conhost の
// アイコンが出てしまうので、明示的に付け直す必要がある。
void Term::setWindowIcon() {
    if (!hwnd_) return;
    HMODULE self = GetModuleHandleW(nullptr);
    // 変数名に small は使えない（rpcndr.h が char へ define している）
    HICON big = (HICON)LoadImageW(self, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON sm  = (HICON)LoadImageW(self, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (big) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)big);
        iconBig_ = big;
    }
    if (sm) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)sm);
        iconSmall_ = sm;
    }
}

void Term::enforceSize() {
    if (!entered_ || wantCols_ <= 0 || wantRows_ <= 0) return;
    int c = 0, r = 0;
    size(&c, &r);
    if (c == wantCols_ && r == wantRows_) return;      // 既に要求どおりなら何もしない
    // 要求が弾かれる端末だと「要求 → 端末が別サイズへ → 通知 → 要求」の往復が
    // 止まらなくなる。間隔を空けて、リサイズ通知の嵐でも張り付かないようにする。
    const unsigned long long now = GetTickCount64();
    if (now - lastApply_ < 250) return;
    lastApply_ = now;
    applySize(wantCols_, wantRows_);
}

bool Term::enter(int fixedCols, int fixedRows, bool ownWindow,
                 const TermStyle& style, std::wstring* err) {
    // 呼び出し側が「自分で開いた」と言っていなくても、このコンソールに
    // 自分しか繋がっていなければ実質こちらの窓（ダブルクリック起動など）。
    ownWindow_ = ownWindow || soleConsoleClient();
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
    // ENABLE_WRAP_AT_EOL_OUTPUT を落とすのは、サイズ固定でスクロールバッファを
    // 画面ちょうどにするため。右下のセルまで描く全面再描画で行送りが起きると、
    // 逃がす余白が無く画面全体が 1 行ずれたまま戻らない。
    if (!SetConsoleMode(hOut_, (savedOut_ & ~ENABLE_WRAP_AT_EOL_OUTPUT)
                                         | ENABLE_VIRTUAL_TERMINAL_PROCESSING
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

    // leave() で元へ戻せるよう、触る前の大きさを控えておく。
    // バッファ(dwSize)も控えるのは、スクロールバッファを縮めたまま終わると
    // 端末の履歴の高さが変わってしまうため。
    {
        CONSOLE_SCREEN_BUFFER_INFO bi;
        if (GetConsoleScreenBufferInfo(hOut_, &bi)) {
            savedWinCols_ = bi.srWindow.Right - bi.srWindow.Left + 1;
            savedWinRows_ = bi.srWindow.Bottom - bi.srWindow.Top + 1;
            savedBuf_ = bi.dwSize;
        }
    }

    // ウィンドウの境界と最大化ボタンを外してリサイズを塞ぐ。
    // Windows Terminal では GetConsoleWindow() が擬似ウィンドウを返すため効かない
    // （害もない）。効かない環境では利用者が窓を広げられてしまうので、
    // リサイズ通知のたびに enforceSize() で押し戻す前提でいる。
    hwnd_ = GetConsoleWindow();
    if (hwnd_) {
        savedStyle_ = GetWindowLong(hwnd_, GWL_STYLE);
        if (savedStyle_ != 0) {
            styleSaved_ = true;
            SetWindowLong(hwnd_, GWL_STYLE, savedStyle_ & ~WS_SIZEBOX & ~WS_MAXIMIZEBOX);
        }
    }

    // フォントはサイズ合わせより先に。文字セルの大きさが変わると窓の実寸も
    // 変わるので、順序を逆にすると合わせた直後にずれる。
    applyStyle(style);
    syncWindowTheme();

    // サイズ合わせは代替画面へ入る前に行う。conhost の代替バッファは
    // SetConsoleScreenBufferSize を受け付けず、バッファだけ元の高さ(既定9001行)の
    // まま残ってスクロールバーが出てしまうため。
    wantCols_ = (fixedCols > 0) ? fixedCols : 0;
    wantRows_ = (fixedRows > 0) ? fixedRows : 0;
    if (wantCols_ > 0 && wantRows_ > 0) {
        lastApply_ = GetTickCount64();
        applySize(wantCols_, wantRows_);
    }

    // conhost で開き直すとタイトルが "conhost.exe" のままになるので付け直す。
    // 元のタイトルは leave() で戻す。
    if (GetConsoleTitleW(savedTitle_, (DWORD)(sizeof(savedTitle_) / sizeof(savedTitle_[0]))))
        titleSaved_ = true;
    SetConsoleTitleW(L"OpenKrisp");

    hWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // 代替画面へ入ったら、最初の描画が来る前に PowerShell 相当の配色で塗り潰す。
    // 先に色を出しておかないと ESC[2J が端末の既定色で塗ってしまい、
    // 起動直後だけ地色が違って見える。
    std::wstring init = L"\x1b[?1049h\x1b[?25l";
    appendSgr(init, ATTR_NONE);
    init += L"\x1b[2J";
    DWORD wrote = 0;
    WriteConsoleW(hOut_, init.c_str(), (DWORD)init.size(), &wrote, nullptr);
    entered_ = true;
    left_.store(false);

    // スクロールバーを落とすのは最後。conhost はバッファや窓の大きさを
    // 変えるたびにスクロールバーの要否を計算し直してスタイルを付け直すので、
    // サイズ合わせより前に外しても戻ってしまう。
    // バッファを窓ぴったりに縮められた窓では元々出ないが、利用者のシェルが
    // 同居していて縮められなかった窓ではここで消える。
    if (hwnd_) {
        LONG s = GetWindowLong(hwnd_, GWL_STYLE);
        if (s & (WS_VSCROLL | WS_HSCROLL)) {
            SetWindowLong(hwnd_, GWL_STYLE, s & ~WS_VSCROLL & ~WS_HSCROLL);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }
    return true;
}

void Term::leave() {
    if (!entered_) return;
    if (left_.exchange(true)) return;         // 多重呼び出し（Ctrl+C ハンドラ）に耐える
    const wchar_t* fin = L"\x1b[0m\x1b[?25h\x1b[?1049l";
    DWORD wrote = 0;
    WriteConsoleW(hOut_, fin, (DWORD)wcslen(fin), &wrote, nullptr);

    // 透過とフォントを先に戻す。フォントは文字セルの大きさが変わるので、
    // サイズを戻すより前にやらないと復元後の実寸がずれる。
    if (layered_ && hwnd_) {
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        LONG ex = GetWindowLong(hwnd_, GWL_EXSTYLE);
        SetWindowLong(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
    if (fontSaved_) SetCurrentConsoleFontEx(hOut_, FALSE, &savedFont_);
    if (themedWindow_ && hwnd_) {
        const COLORREF def = 0xFFFFFFFF;   // DWMWA_COLOR_DEFAULT
        DwmSetWindowAttribute(hwnd_, 35, &def, sizeof(def));
        DwmSetWindowAttribute(hwnd_, 34, &def, sizeof(def));
        DwmSetWindowAttribute(hwnd_, 36, &def, sizeof(def));
    }
    // 続けてスタイルを戻す。境界のない窓のままサイズを戻すと、枠の太さの違いで
    // 復元後の大きさがずれる。
    if (styleSaved_ && hwnd_) SetWindowLong(hwnd_, GWL_STYLE, savedStyle_);
    // 元の大きさへ戻す。VT はまだ有効なので、モードを戻す前に済ませる。
    if (savedWinCols_ > 0 && savedWinRows_ > 0 && wantCols_ > 0 && wantRows_ > 0) {
        requestVtSize(hOut_, savedWinCols_, savedWinRows_);
        COORD buf = savedBuf_;
        if (buf.X < savedWinCols_) buf.X = (SHORT)savedWinCols_;
        if (buf.Y < savedWinRows_) buf.Y = (SHORT)savedWinRows_;
        setConsoleGeometry(hOut_, savedWinCols_, savedWinRows_, buf);
    }

    if (titleSaved_) SetConsoleTitleW(savedTitle_);
    if (modeOutSet_) SetConsoleMode(hOut_, savedOut_);
    if (modeInSet_)  SetConsoleMode(hIn_, savedIn_);

    // 窓より先にアイコンを壊さないよう、モードを戻し切ってから解放する
    if (hwnd_) {
        if (iconBig_)   SendMessageW(hwnd_, WM_SETICON, ICON_BIG, 0);
        if (iconSmall_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, 0);
    }
    if (iconBig_)   { DestroyIcon(iconBig_);   iconBig_ = nullptr; }
    if (iconSmall_) { DestroyIcon(iconSmall_); iconSmall_ = nullptr; }
}

bool Term::probeAmbiguousDoubleWidth() {
    if (!entered_) return false;
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo(hOut_, &bi)) return false;

    // 左上へ 1 文字だけ書いてカーソルの進み方を見る。この後すぐ全面描画で
    // 上書きされるので、画面に残る心配はない。
    const COORD origin = { 0, 0 };
    if (!SetConsoleCursorPosition(hOut_, origin)) return false;
    DWORD wrote = 0;
    if (!WriteConsoleW(hOut_, L"─", 1, &wrote, nullptr)) return false;
    if (!GetConsoleScreenBufferInfo(hOut_, &bi)) return false;
    const int advanced = bi.dwCursorPosition.X;

    // 消してカーソルを戻す
    SetConsoleCursorPosition(hOut_, origin);
    WriteConsoleW(hOut_, L"  ", 2, &wrote, nullptr);
    SetConsoleCursorPosition(hOut_, origin);

    return advanced >= 2;
}

void Term::wake() {
    if (hWake_) SetEvent(hWake_);
}

void Term::size(int* cols, int* rows) const {
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (!GetConsoleScreenBufferInfo(hOut_, &bi)) {
        *cols = 80; *rows = 25;
        sizeLocked_ = false;
        return;
    }
    // dwSize はスクロールバッファの大きさなので、見えている窓の大きさを使う
    *cols = bi.srWindow.Right - bi.srWindow.Left + 1;
    *rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    if (*cols < 1) *cols = 1;
    if (*rows < 1) *rows = 1;
    // sizeLocked() は「直近に測った大きさが要求どおりか」を返すので、ここで更新する
    sizeLocked_ = (wantCols_ > 0 && wantRows_ > 0 &&
                   *cols == wantCols_ && *rows == wantRows_);
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
            e.alt   = (k.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
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
