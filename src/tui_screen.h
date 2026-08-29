// TUI の低レベル層。外部ライブラリを使わず、Windows Console API と VT100
// エスケープだけで画面を作る。
//
// 【なぜセルバッファなのか】
//   毎フレーム全部書き直すとちらつく。前回の内容(front_)と今回(back_)を
//   セル単位で比べ、変わった範囲だけを 1 回の WriteConsoleW で流し込む。
//
// 【全角幅の扱い】
//   UI は日本語なので、幅を数えずに詰めると罫線が必ずずれる。CJK は 2 桁、
//   それ以外は 1 桁として数える。
//   ただし罫線(U+2500-257F)やブロック(U+2588)は East Asian "Ambiguous" で、
//   Windows Terminal では 1 桁だが conhost + 日本語フォントでは 2 桁に描かれる
//   ことがある。ここでは 1 桁として扱い、ずれる端末向けに ASCII 版の字形を
//   用意しておく（Glyphs / setAscii）。
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

// --- 文字幅 ---------------------------------------------------------------
int charWidth(wchar_t c);                 // 0(結合文字) / 1 / 2
int strWidth(const std::wstring& s);
// 表示幅が maxW を超えないよう末尾を詰める。切った場合は ell を付ける。
std::wstring truncWidth(const std::wstring& s, int maxW, const wchar_t* ell);

// --- 配色 -----------------------------------------------------------------
// 端末の既定色に任せず自前で塗る。conhost で開き直したときに配色が利用者の
// conhost 側の設定次第でばらつくのと、背景を塗らないと枠の外が浮いて見えるため。
enum class Theme {
    ClaudeDark,   // 利用者の Windows Terminal のスキーム "Claude Dark"
    PowerShell,   // Windows PowerShell のコンソールの既定（濃紺）
};
void  setTheme(Theme t);
Theme currentTheme();
// "claude-dark" / "powershell" との相互変換。解釈できなければ false。
bool  parseTheme(const std::wstring& s, Theme* out);
const wchar_t* themeName(Theme t);

// --- 表示属性 -------------------------------------------------------------
enum Attr : uint16_t {
    ATTR_NONE   = 0,
    ATTR_DIM    = 1 << 0,
    ATTR_BOLD   = 1 << 1,
    ATTR_REV    = 1 << 2,
    ATTR_CYAN   = 1 << 3,
    ATTR_GREEN  = 1 << 4,
    ATTR_YELLOW = 1 << 5,
    ATTR_RED    = 1 << 6,
};

// --- 字形 -----------------------------------------------------------------
// 罫線が全角幅で描かれる端末向けに、ASCII だけの組を用意しておく。
struct Glyphs {
    const wchar_t* tl; const wchar_t* tr; const wchar_t* bl; const wchar_t* br;
    const wchar_t* h;  const wchar_t* v;  const wchar_t* ml; const wchar_t* mr;
    const wchar_t* barOn; const wchar_t* barOff;
    const wchar_t* cursor;
    const wchar_t* ellipsis;
    const wchar_t* up;   const wchar_t* down;
    const wchar_t* left; const wchar_t* right;
};
const Glyphs& glyphSet(bool ascii);

// --- 画面 -----------------------------------------------------------------
struct Cell {
    wchar_t  ch   = L' ';
    uint16_t attr = ATTR_NONE;
    uint8_t  w    = 1;      // 1=半角, 2=全角の左半分, 0=全角の右半分(出力しない)
};

class Screen {
public:
    void setAscii(bool a) { ascii_ = a; }
    const Glyphs& g() const { return glyphSet(ascii_); }

    void resize(int cols, int rows);   // サイズが変われば全面再描画になる
    int  cols() const { return cols_; }
    int  rows() const { return rows_; }
    void invalidate() { full_ = true; }

    void begin();                      // back_ を空白でクリアする
    int  put(int x, int y, const std::wstring& s, uint16_t attr = ATTR_NONE);
    int  put(int x, int y, const wchar_t* s, uint16_t attr = ATTR_NONE);
    void fill(int x, int y, int w, const wchar_t* s, uint16_t attr = ATTR_NONE);
    void box(int x, int y, int w, int h, uint16_t attr = ATTR_NONE);
    void hsep(int x, int y, int w, uint16_t attr = ATTR_NONE);  // 枠の途中の横罫線
    void flush(HANDLE hOut);           // 差分だけを 1 回で書き出す
    // バックバッファを装飾なしのテキストにする（--uitest で桁ずれを確認するため）
    std::wstring dumpText() const;

private:
    // (x,y) を上書きする直前に呼ぶ。全角は 2 セルで 1 文字なので、片側だけを
    // 書き換えると相方が w=2 / w=0 のまま取り残され、flush() の差分計算が壊れる。
    // 取り残される側を半角の空白へ潰して、セルの対を常に成立させておく。
    void breakWideAt(int x, int y);

    std::vector<Cell> front_, back_;
    int  cols_ = 0, rows_ = 0;
    bool full_ = true;
    bool ascii_ = false;
};

// --- 端末 -----------------------------------------------------------------
struct TermEvent {
    enum Type { None, Key, Resize } type = None;
    int     vk    = 0;      // VK_UP など
    wchar_t ch    = 0;      // 印字可能文字（vk と併用）
    bool    ctrl  = false;
    bool    shift = false;
    bool    alt   = false;
    // Win キーはコンソールの入力レコードに乗らない（シェルが横取りする）ため
    // 取得できない。設定ファイルに手で書けば解釈はされる。
};

// conhost のウィンドウに適用する見た目。既定値は利用者の Windows Terminal の
// 設定（JetBrainsMono NFM / opacity 95）に合わせてある。
struct TermStyle {
    std::wstring fontFace = L"JetBrainsMono NFM";
    int fontSize = 16;      // 文字セルの高さ(px)。12pt 相当
    int opacity  = 95;      // 0-100。100 で不透明
};

class Term {
public:
    // モードを退避し、VT を有効化して代替画面へ移る。
    // 代替画面へ入る前に fixedCols x fixedRows へ端末を合わせ、リサイズも塞ぐ。
    // VT を有効にできない端末では false を返す（TUI を諦める）。
    // サイズを合わせられない端末でも TUI 自体は起動する（size() は実サイズを返す）。
    //
    // ownWindow: この窓を自分で開いたか。自分の窓ならスクロールバッファも
    //   要求サイズちょうどに縮める（スクロールバーを出さないため）。
    //   利用者が既に使っていた窓では縮めない。今まで出力していた履歴が消えるため。
    bool enter(int fixedCols, int fixedRows, bool ownWindow,
               const TermStyle& style, std::wstring* err);
    // 退避したモード・サイズ・ウィンドウスタイルへ完全に戻す。
    // 多重呼び出し安全（Ctrl+C ハンドラからも呼ぶ）。
    void leave();
    // リサイズされてしまったときに、要求サイズへ戻すよう再要求する。
    // WINDOW_BUFFER_SIZE_EVENT を受けたら呼ぶ。既に要求どおりなら何もしない。
    void enforceSize();
    // タイトルバーと枠の色を、いま選ばれている配色の背景色に合わせる。
    // 配色を切り替えたあとにも呼ぶこと。Windows 11 でしか効かない。
    void syncWindowTheme();
    // 要求どおりのサイズになっているか（画面側が「小さすぎ」表示に落とす判断に使う）。
    // 直近の size() の測定結果で決まる。
    bool sizeLocked() const { return sizeLocked_; }
    // 罫線(U+2500)が全角幅で描かれる端末か、実際に 1 文字書いてカーソルの
    // 進み具合で測る。East Asian Ambiguous はフォント任せで決め打ちできないため。
    // 代替画面へ入った後、最初の描画より前に呼ぶこと。
    bool probeAmbiguousDoubleWidth();
    // 実際に適用できたフォント名。要求と違えば conhost に弾かれている。
    // 何も適用できなかった場合は空。
    const std::wstring& appliedFont() const { return appliedFont_; }
    // timeoutMs 待って入力を取り出す。false ならタイムアウト（＝再描画の合図）。
    bool poll(TermEvent* ev, DWORD timeoutMs);
    void wake();                        // poll を即座に起こす
    void size(int* cols, int* rows) const;
    HANDLE out() const { return hOut_; }

private:
    // 要求サイズへ寄せる。合わせられたら true。
    bool applySize(int cols, int rows);
    // フォントと透過を適用する。conhost 以外では効かない。
    void applyStyle(const TermStyle& style);

    HANDLE hIn_ = nullptr, hOut_ = nullptr, hWake_ = nullptr;
    DWORD  savedIn_ = 0, savedOut_ = 0;
    bool   modeOutSet_ = false, modeInSet_ = false;
    wchar_t savedTitle_[256] = {};
    bool   titleSaved_ = false;
    bool   entered_ = false;
    std::atomic<bool> left_{false};
    // 1 回の ReadConsoleInputW で複数のイベントが来るので、取り出せなかった分を貯める。
    std::vector<TermEvent> queue_;

    // --- サイズ固定 ---
    int    wantCols_ = 0, wantRows_ = 0;      // 0 なら固定しない
    // size() は const だが、測るたびに判定を更新したいので mutable にしてある。
    mutable bool sizeLocked_ = false;
    unsigned long long lastApply_ = 0;        // enforceSize() の暴走よけ
    bool   ownWindow_ = false;
    // leave() で元へ戻すために enter() で退避する
    CONSOLE_FONT_INFOEX savedFont_ = {};
    bool   fontSaved_ = false;
    std::wstring appliedFont_;
    bool   layered_ = false;      // 透過のために WS_EX_LAYERED を足したか
    bool   themedWindow_ = false; // タイトルバーの色を変えたか
    int    savedWinCols_ = 0, savedWinRows_ = 0;
    COORD  savedBuf_ = { 0, 0 };
    HWND   hwnd_ = nullptr;
    LONG   savedStyle_ = 0;
    bool   styleSaved_ = false;
};
