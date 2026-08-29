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
};

class Term {
public:
    // モードを退避し、VT を有効化して代替画面へ移る。
    // VT を有効にできない端末では false を返す（TUI を諦める）。
    bool enter(std::wstring* err);
    // 退避したモードへ完全に戻す。多重呼び出し安全（Ctrl+C ハンドラからも呼ぶ）。
    void leave();
    // timeoutMs 待って入力を取り出す。false ならタイムアウト（＝再描画の合図）。
    bool poll(TermEvent* ev, DWORD timeoutMs);
    void wake();                        // poll を即座に起こす
    void size(int* cols, int* rows) const;
    HANDLE out() const { return hOut_; }

private:
    HANDLE hIn_ = nullptr, hOut_ = nullptr, hWake_ = nullptr;
    DWORD  savedIn_ = 0, savedOut_ = 0;
    bool   modeOutSet_ = false, modeInSet_ = false;
    bool   entered_ = false;
    std::atomic<bool> left_{false};
    // 1 回の ReadConsoleInputW で複数のイベントが来るので、取り出せなかった分を貯める。
    std::vector<TermEvent> queue_;
};
