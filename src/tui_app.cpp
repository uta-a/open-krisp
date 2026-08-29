#include "tui_app.h"
#include "tui_screen.h"
#include "engine.h"
#include "settings.h"
#include "hotkey.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <algorithm>

// 起動時に端末をこの大きさへ合わせ、リサイズできないようにする。
// レイアウトはこのサイズで過不足なく埋まるように組んである。
const int kFixedCols = 64;
const int kFixedRows = 20;

namespace {

// 固定に失敗した端末でも、これ以上あれば崩れずに描ける下限。
const int kMinCols = 56;
const int kMinRows = 20;

const int kBoxRows  = 17;   // 枠の高さ（この下にキー説明 2 行 + 通知 1 行）
// ラベル欄の幅。最長の「全体ホットキー」(全角7文字=14桁) が値とくっつかないよう
// 2 桁ぶん余裕を持たせる。
const int kLabelW   = 16;
const int kMaxBar   = 30;   // メーターの最大桁数

// 操作できる行。モデル行は表示のみなのでここには入れない。
enum Row { R_IN = 0, R_OUT, R_MUTE, R_NC, R_AGC, R_DUR, R_MUTEKEY, R_GLOBALKEY, R_COUNT };

// TUI 内のキー割り当てで奪ってはいけないキー。奪うと画面から出られなくなる。
bool reservedForUi(int vk) {
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
           vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_TAB;
}

// いま押されている修飾キーを KeyBinding の mods へ。
// Win キーはコンソールの入力レコードに乗らないので取れない。
unsigned int modsOf(const TermEvent& ev) {
    unsigned int m = 0;
    if (ev.ctrl)  m |= MOD_CONTROL;
    if (ev.alt)   m |= MOD_ALT;
    if (ev.shift) m |= MOD_SHIFT;
    return m;
}

// Ctrl+C / ウィンドウを閉じる操作でも端末を必ず元へ戻すため、ハンドラから触る。
Term* g_term = nullptr;
std::atomic<bool> g_quit{ false };

BOOL WINAPI ctrlHandler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT ||
        t == CTRL_CLOSE_EVENT || t == CTRL_LOGOFF_EVENT || t == CTRL_SHUTDOWN_EVENT) {
        g_quit.store(true);
        // 閉じる系はハンドラから戻った直後に強制終了されるため、ここで直接戻す。
        if (g_term) { g_term->leave(); g_term->wake(); }
        if (t == CTRL_CLOSE_EVENT) Sleep(200);
        return TRUE;
    }
    return FALSE;
}

// stderr へ UTF-8 で出す。fwprintf のワイド出力はバイト指向のストリームで
// 化けるため、明示的に変換する（cli.cpp の outw と同じ理由）。
void errw(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
    std::vector<char> u8((size_t)(n > 0 ? n : 0) + 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                   u8.data(), n, nullptr, nullptr);
    fputs(u8.data(), stderr);
}

std::wstring padTo(const std::wstring& s, int w) {
    std::wstring t = truncWidth(s, w, L"");
    int pad = w - strWidth(t);
    if (pad > 0) t.append((size_t)pad, L' ');
    return t;
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list ap; va_start(ap, f);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, f, ap);
    va_end(ap);
    return buf;
}

// 0..1 のピークを dB 表記に。無音は "  -inf"。
std::wstring dbText(float peak) {
    if (peak <= 1e-6f) return L"  -inf";
    double db = 20.0 * log10((double)peak);
    if (db < -99.0) db = -99.0;
    return fmt(L"%6.1f", db);
}

// -60dB..0dB を cells 段のバーへ。
std::wstring meterBar(const Glyphs& gl, float peak, int cells) {
    int lv = 0;
    if (peak > 0.f) {
        double db = 20.0 * log10((double)peak);
        lv = (int)((db + 60.0) / 60.0 * cells + 0.5);
    }
    lv = (std::max)(0, (std::min)(cells, lv));
    std::wstring s;
    for (int i = 0; i < cells; i++) s += (i < lv) ? gl.barOn : gl.barOff;
    return s;
}

std::wstring hhmmss(unsigned long long ms) {
    unsigned long long sec = ms / 1000;
    return fmt(L"%02llu:%02llu:%02llu", sec / 3600, (sec / 60) % 60, sec % 60);
}

// ---------------------------------------------------------------------------
class App {
public:
    App(AudioEngine& eng) : eng_(eng) {}
    int run();
    int uiTest(int cols, int rows, bool ascii, int overlay);

private:
    void render();
    void renderMain(int cols, int rows);
    void renderPicker(int cols, int rows);
    void renderCapture(int cols, int rows);
    void onKey(const TermEvent& ev);
    void onKeyMain(const TermEvent& ev);
    void onKeyPicker(const TermEvent& ev);
    void onKeyCapture(const TermEvent& ev);
    void openPicker(bool input);
    void applyPicked();
    void beginCapture(bool global);
    void applyGlobalHotkey();
    void toggleMute();
    void toast(const std::wstring& s, uint16_t attr = ATTR_NONE);
    void restartWith(const std::wstring& what, const std::function<bool(std::wstring*)>& op);

    AudioEngine& eng_;
    Term   term_;
    Screen scr_;
    Settings st_;

    int  sel_ = R_MUTE;
    bool picking_ = false;
    bool pickInput_ = false;
    std::vector<DeviceInfo> pickList_;
    int  pickSel_ = 0;
    int  pickTop_ = 0;

    // キー割り当ての取得中。次に押されたキーをそのまま割り当てる。
    bool capturing_ = false;
    bool captureGlobal_ = false;
    std::wstring captureErr_;

    GlobalHotkey ghk_;
    // ホットキーは専用スレッドから飛んでくるので、フラグを立てるだけにして
    // 実際の切り替えは UI スレッドで行う。
    std::atomic<bool> ghkFired_{ false };

    EngineStats stats_;
    unsigned long long startedAt_ = 0;
    std::wstring msg_;
    uint16_t msgAttr_ = ATTR_NONE;
    unsigned long long msgUntil_ = 0;
    bool warnedSilent_ = false;

    // メーターの落ち方を緩めて、バーがちらつかないようにする
    float inHold_ = 0.f, outHold_ = 0.f;

    // --uitest 用: 端末に触らず固定サイズで 1 枚だけ組み立てる
    bool dump_ = false;
    int  dumpCols_ = 0, dumpRows_ = 0;
};

void App::toast(const std::wstring& s, uint16_t attr) {
    msg_ = s;
    msgAttr_ = attr;
    msgUntil_ = GetTickCount64() + 3000;
}

// 再起動を伴う操作。数百 ms 止まるので、先に「切替中」を描いてから呼ぶ。
void App::restartWith(const std::wstring& what,
                      const std::function<bool(std::wstring*)>& op) {
    msg_ = what + L"...";
    msgAttr_ = ATTR_YELLOW;
    msgUntil_ = GetTickCount64() + 3000;
    render();
    std::wstring err;
    if (op(&err)) toast(what + L"しました", ATTR_GREEN);
    else          toast(what + L"に失敗: " + err, ATTR_RED);
}

void App::toggleMute() {
    const bool next = !eng_.muted();
    eng_.setMuted(next);
    if (next) { outHold_ = 0.f; }          // メーターを即座に落として状態を分かりやすく
    toast(next ? L"ミュートしました（相手に音は届きません）" : L"ミュートを解除しました",
          next ? ATTR_RED : ATTR_GREEN);
}

// --- 描画 -----------------------------------------------------------------
void App::render() {
    int cols = 0, rows = 0;
    if (dump_) { cols = dumpCols_; rows = dumpRows_; }
    else       term_.size(&cols, &rows);
    scr_.resize(cols, rows);
    scr_.begin();

    if (cols < kMinCols || rows < kMinRows) {
        // 端末サイズを固定できなかった環境向けの逃げ道。
        const std::wstring m1 = L"端末が小さすぎます";
        const std::wstring m2 = fmt(L"%dx%d 以上にしてください（現在 %dx%d）",
                                    kMinCols, kMinRows, cols, rows);
        const int y = (std::max)(0, rows / 2 - 1);
        scr_.put((std::max)(0, (cols - strWidth(m1)) / 2), y, m1, ATTR_YELLOW);
        scr_.put((std::max)(0, (cols - strWidth(m2)) / 2), y + 1, m2, ATTR_DIM);
        if (!dump_) scr_.flush(term_.out());
        return;
    }

    renderMain(cols, rows);
    if (picking_)   renderPicker(cols, rows);
    if (capturing_) renderCapture(cols, rows);
    if (!dump_) scr_.flush(term_.out());
}

void App::renderMain(int cols, int rows) {
    const Glyphs& gl = scr_.g();
    const int boxH   = (std::max)(kBoxRows, rows - 3);   // 画面下端まで枠を伸ばす
    const int curX   = 1;                                // 選択マーカーの桁
    const int labelX = 2;
    const int valueX = labelX + kLabelW;
    const int rightX = cols - 2;                         // 枠の内側の右端

    scr_.box(0, 0, cols, boxH, ATTR_NONE);

    // 上枠: タイトル / ミュート表示 / 稼働時間
    scr_.put(2, 0, L" OpenKrisp ", ATTR_BOLD | ATTR_CYAN);
    const bool muted = eng_.muted();
    if (muted) scr_.put(15, 0, L" ● ミュート中 ", ATTR_BOLD | ATTR_RED);
    {
        std::wstring up = L" 稼働 " + hhmmss(startedAt_ ? GetTickCount64() - startedAt_ : 0) + L" ";
        scr_.put(rightX - strWidth(up) + 1, 0, up, ATTR_DIM);
    }

    // デバイス
    const int nameX = labelX + 6;
    scr_.put(labelX, 1, L"入力");
    scr_.put(nameX, 1, truncWidth(stats_.inName, rightX - nameX + 1, gl.ellipsis));
    scr_.put(labelX, 2, L"出力");
    scr_.put(nameX, 2, truncWidth(stats_.outName, rightX - nameX + 1, gl.ellipsis));
    scr_.hsep(0, 3, cols);

    // メーター（幅は端末に合わせて伸ばす）
    const int meterX = labelX + 5;
    const int bar = (std::min)(kMaxBar, (std::max)(12, rightX - meterX - 10));
    scr_.put(labelX, 4, L"入力 ");
    scr_.put(meterX, 4, meterBar(gl, inHold_, bar), ATTR_GREEN);
    scr_.put(meterX + bar + 1, 4, dbText(inHold_) + L" dB", ATTR_DIM);
    scr_.put(labelX, 5, L"出力 ");
    scr_.put(meterX, 5, meterBar(gl, outHold_, bar), muted ? ATTR_RED : ATTR_CYAN);
    scr_.put(meterX + bar + 1, 5, dbText(outHold_) + L" dB", ATTR_DIM);
    scr_.hsep(0, 6, cols);

    // 設定行
    EngineConfig c = eng_.config();
    const int yIn = 1, yOut = 2, yMute = 7, yNc = 8, yAgc = 9, yDur = 10,
              yMuteKey = 11, yGlobalKey = 12, yModel = 13;
    const int selY[R_COUNT] = { yIn, yOut, yMute, yNc, yAgc, yDur, yMuteKey, yGlobalKey };
    for (int r = 0; r < R_COUNT; r++)
        scr_.put(curX, selY[r], (sel_ == r) ? gl.cursor : L" ",
                 (sel_ == r) ? (ATTR_BOLD | ATTR_CYAN) : ATTR_NONE);

    auto label = [&](int y, const wchar_t* text, int row) {
        scr_.put(labelX, y, padTo(text, kLabelW), (sel_ == row) ? ATTR_BOLD : ATTR_NONE);
    };

    // ミュート
    label(yMute, L"ミュート", R_MUTE);
    scr_.put(valueX, yMute, muted ? L"ON " : L"OFF", muted ? (ATTR_BOLD | ATTR_RED) : ATTR_DIM);
    scr_.put(valueX + 7, yMute,
             muted ? std::wstring(L"マイクを止めています")
                   : formatKeyBinding(st_.muteKey) + L" で切替",
             muted ? ATTR_RED : ATTR_DIM);
    // ノイズ抑制
    label(yNc, L"ノイズ抑制", R_NC);
    scr_.put(valueX, yNc, c.bypass ? L"OFF" : L"ON ", c.bypass ? ATTR_RED : ATTR_GREEN);
    scr_.put(valueX + 7, yNc, fmt(L"強度 %3.0f", c.suppression),
             c.bypass ? ATTR_DIM : ATTR_NONE);
    // AGC
    label(yAgc, L"AGC", R_AGC);
    scr_.put(valueX, yAgc, c.agcEnabled ? L"ON " : L"OFF",
             c.agcEnabled ? ATTR_GREEN : ATTR_DIM);
    scr_.put(valueX + 7, yAgc, fmt(L"目標 %.2f", c.agcTarget),
             c.agcEnabled ? ATTR_NONE : ATTR_DIM);
    if (c.agcEnabled && valueX + 19 + 9 <= rightX)
        scr_.put(valueX + 19, yAgc, fmt(L"gain %.1fx", stats_.agcGain), ATTR_DIM);
    // フレーム長
    label(yDur, L"フレーム長", R_DUR);
    scr_.put(valueX, yDur, fmt(L"%d ms", c.durationMs));
    // ミュートのキー割り当て
    label(yMuteKey, L"ミュートキー", R_MUTEKEY);
    scr_.put(valueX, yMuteKey, formatKeyBinding(st_.muteKey), ATTR_CYAN);
    label(yGlobalKey, L"全体ホットキー", R_GLOBALKEY);
    {
        const bool on = ghk_.active();
        scr_.put(valueX, yGlobalKey, formatKeyBinding(st_.globalMuteKey),
                 on ? ATTR_CYAN : ATTR_DIM);
        if (st_.globalMuteKey.assigned() && !on)
            scr_.put(valueX + 16, yGlobalKey, L"登録できず", ATTR_RED);
        else if (!st_.globalMuteKey.assigned())
            scr_.put(valueX + 16, yGlobalKey, L"他アプリ中でも効く", ATTR_DIM);
    }
    // モデル（切替は未対応なので淡色）
    scr_.put(labelX, yModel, padTo(L"モデル", kLabelW), ATTR_DIM);
    scr_.put(valueX, yModel, L"full_NC", ATTR_DIM);

    scr_.hsep(0, boxH - 3, cols);
    scr_.put(labelX, boxH - 2,
             fmt(L"Fifo %2dms  UR %llu  破棄 %llu", stats_.fifoMs, stats_.underruns, stats_.drops),
             ATTR_DIM);

    // 枠の下: キー説明 2 行と通知 1 行
    const int f1 = rows - 3, f2 = rows - 2, sy = rows - 1;
    scr_.put(1, f1, truncWidth(fmt(L"%s%s 選択   %s%s 値   Enter 切替/決定   %s ミュート",
                                   gl.up, gl.down, gl.left, gl.right,
                                   formatKeyBinding(st_.muteKey).c_str()),
                               cols - 2, gl.ellipsis), ATTR_DIM);
    scr_.put(1, f2, truncWidth(L"S 保存   R デバイス再検索   A 字形   Q 終了",
                               cols - 2, gl.ellipsis), ATTR_DIM);

    if (!msg_.empty() && GetTickCount64() < msgUntil_)
        scr_.put(1, sy, truncWidth(msg_, cols - 2, gl.ellipsis), msgAttr_);
    else if (!stats_.running)
        scr_.put(1, sy, L"停止中（Enter でデバイスを選び直してください）", ATTR_RED);
    else if (!stats_.notice.empty())
        scr_.put(1, sy, truncWidth(stats_.notice, cols - 2, gl.ellipsis), ATTR_YELLOW);
    else if (muted)
        scr_.put(1, sy, L"ミュート中です。相手に音は届いていません。", ATTR_RED);
}

void App::renderPicker(int cols, int rows) {
    const Glyphs& gl = scr_.g();
    const int w = (std::min)(cols - 8, 52);
    const int listRows = (std::max)(3, (std::min)((int)pickList_.size(), rows - 8));
    const int h = listRows + 2;          // 上下の枠のぶんだけ足す（余白は作らない）
    const int x = (cols - w) / 2;
    const int y = (std::max)(1, (rows - h) / 2);

    for (int i = 0; i < h; i++) scr_.fill(x, y + i, w, L" ");
    scr_.box(x, y, w, h, ATTR_CYAN);
    scr_.put(x + 2, y, pickInput_ ? L" 入力デバイスを選ぶ " : L" 出力デバイスを選ぶ ",
             ATTR_BOLD | ATTR_CYAN);

    if (pickSel_ < pickTop_) pickTop_ = pickSel_;
    if (pickSel_ >= pickTop_ + listRows) pickTop_ = pickSel_ - listRows + 1;

    // 右端の 2 桁はスクロール記号のために空けておく。行の文字と重ね書きすると
    // 全角の右半分を半角で潰してしまい、セルバッファが壊れる。
    const int textW = w - 5;
    for (int i = 0; i < listRows; i++) {
        int idx = pickTop_ + i;
        if (idx >= (int)pickList_.size()) break;
        const bool cur = (idx == pickSel_);
        std::wstring line = (cur ? std::wstring(gl.cursor) : L" ") + L" " +
                            truncWidth(pickList_[idx].name, textW, gl.ellipsis);
        scr_.put(x + 1, y + 1 + i, padTo(line, w - 3), cur ? ATTR_REV : ATTR_NONE);
    }
    if (pickTop_ > 0) scr_.put(x + w - 2, y + 1, gl.up, ATTR_DIM);
    if (pickTop_ + listRows < (int)pickList_.size())
        scr_.put(x + w - 2, y + h - 2, gl.down, ATTR_DIM);
    scr_.put(x + 1, y + h - 1, fmt(L" %s%s Enter 決定  Esc 戻る ", gl.up, gl.down), ATTR_DIM);
}

void App::renderCapture(int cols, int rows) {
    const Glyphs& gl = scr_.g();
    const int w = (std::min)(cols - 8, 52);
    const int h = captureErr_.empty() ? 4 : 5;   // 上枠 + 説明2行(+エラー) + 下枠
    const int x = (cols - w) / 2;
    const int y = (std::max)(1, (rows - h) / 2);

    for (int i = 0; i < h; i++) scr_.fill(x, y + i, w, L" ");
    scr_.box(x, y, w, h, ATTR_CYAN);
    scr_.put(x + 2, y, captureGlobal_ ? L" 全体ホットキーを割り当て "
                                      : L" ミュートキーを割り当て ",
             ATTR_BOLD | ATTR_CYAN);

    int ln = y + 1;
    scr_.put(x + 2, ln++, L"割り当てたいキーを押してください", ATTR_BOLD);
    scr_.put(x + 2, ln++,
             captureGlobal_ ? L"Ctrl / Alt / Shift と組み合わせてください"
                            : L"矢印・Enter・Esc は使えません",
             ATTR_DIM);
    if (!captureErr_.empty())
        scr_.put(x + 2, ln++, truncWidth(captureErr_, w - 4, gl.ellipsis), ATTR_RED);
    scr_.put(x + 2, y + h - 1, L" Esc 中止   Delete 解除 ", ATTR_DIM);
}

// --- 操作 -----------------------------------------------------------------
void App::beginCapture(bool global) {
    capturing_ = true;
    captureGlobal_ = global;
    captureErr_.clear();
}

// 設定を実際のホットキー登録へ反映する。失敗してもアプリは動き続ける。
void App::applyGlobalHotkey() {
    std::wstring err;
    if (ghk_.set(st_.globalMuteKey,
                 [this] { ghkFired_.store(true); term_.wake(); }, &err))
        return;
    toast(L"全体ホットキーを登録できません: " + err, ATTR_RED);
}

void App::onKeyCapture(const TermEvent& ev) {
    if (ev.vk == VK_ESCAPE) { capturing_ = false; captureErr_.clear(); return; }

    KeyBinding k;
    if (ev.vk == VK_DELETE || ev.vk == VK_BACK) {
        k = KeyBinding{};                       // 未割り当てにする
    } else {
        k.vk   = (unsigned int)ev.vk;
        k.mods = modsOf(ev);
    }

    if (captureGlobal_) {
        std::wstring why;
        if (!isValidGlobalHotkey(k, &why)) { captureErr_ = why; return; }
        st_.globalMuteKey = k;
        capturing_ = false;
        captureErr_.clear();
        applyGlobalHotkey();
        if (ghk_.active() || !k.assigned())
            toast(L"全体ホットキー: " + formatKeyBinding(k) + L"（S で保存）", ATTR_GREEN);
        return;
    }

    if (k.assigned() && reservedForUi((int)k.vk)) {
        captureErr_ = L"そのキーは画面の操作に使うので割り当てられません";
        return;
    }
    st_.muteKey = k;
    capturing_ = false;
    captureErr_.clear();
    toast(L"ミュートキー: " + formatKeyBinding(k) + L"（S で保存）", ATTR_GREEN);
}

void App::openPicker(bool input) {
    pickInput_ = input;
    pickList_ = eng_.devices(input ? eCapture : eRender);
    if (pickList_.empty()) { toast(L"デバイスが見つかりません", ATTR_RED); return; }
    const std::wstring& cur = input ? stats_.inName : stats_.outName;
    pickSel_ = 0;
    for (size_t i = 0; i < pickList_.size(); i++)
        if (pickList_[i].name == cur) { pickSel_ = (int)i; break; }
    pickTop_ = 0;
    picking_ = true;
}

void App::applyPicked() {
    if (pickSel_ < 0 || pickSel_ >= (int)pickList_.size()) return;
    const std::wstring chosen = pickList_[pickSel_].name;
    EngineConfig c = eng_.config();
    const std::wstring in  = pickInput_ ? chosen : c.inMatch;
    const std::wstring out = pickInput_ ? c.outMatch : chosen;
    picking_ = false;
    restartWith(pickInput_ ? L"入力を切替" : L"出力を切替",
                [&](std::wstring* e) { return eng_.setDevices(in, out, e); });
}

void App::onKeyPicker(const TermEvent& ev) {
    switch (ev.vk) {
    case VK_UP:     if (pickSel_ > 0) pickSel_--; break;
    case VK_DOWN:   if (pickSel_ + 1 < (int)pickList_.size()) pickSel_++; break;
    case VK_HOME:   pickSel_ = 0; break;
    case VK_END:    pickSel_ = (int)pickList_.size() - 1; break;
    case VK_RETURN: applyPicked(); break;
    case VK_ESCAPE: picking_ = false; break;
    default: break;
    }
}

void App::onKeyMain(const TermEvent& ev) {
    EngineConfig c = eng_.config();
    const int dir = (ev.vk == VK_RIGHT) ? 1 : -1;

    // 割り当てられたミュートキーを最優先で見る。修飾なしの割り当てなら
    // Shift の有無は問わない（大文字で打っても効くようにするため）。
    if (st_.muteKey.assigned() && (unsigned int)ev.vk == st_.muteKey.vk &&
        (st_.muteKey.mods == 0 || modsOf(ev) == st_.muteKey.mods)) {
        toggleMute();
        return;
    }

    switch (ev.vk) {
    case VK_UP:   sel_ = (sel_ + R_COUNT - 1) % R_COUNT; return;
    case VK_DOWN: sel_ = (sel_ + 1) % R_COUNT;           return;
    case VK_ESCAPE: g_quit.store(true); return;
    default: break;
    }

    if (ev.vk == VK_LEFT || ev.vk == VK_RIGHT) {
        switch (sel_) {
        case R_IN:  case R_OUT:
            toast(L"Enter でデバイス一覧を開きます");
            break;
        case R_MUTE:
            toggleMute();
            break;
        case R_NC:
            eng_.setSuppression(c.suppression + dir * (ev.shift ? 1.f : 5.f));
            break;
        case R_AGC:
            eng_.setAgcTarget(c.agcTarget + dir * (ev.shift ? 0.005f : 0.01f));
            break;
        case R_DUR:
            restartWith(L"フレーム長を変更", [&](std::wstring* e) {
                return eng_.setDuration(cycleDuration(c.durationMs, dir), e);
            });
            break;
        case R_MUTEKEY:
        case R_GLOBALKEY:
            toast(L"Enter でキーの割り当てを始めます");
            break;
        }
        return;
    }

    if (ev.vk == VK_RETURN) {
        switch (sel_) {
        case R_IN:   openPicker(true);  break;
        case R_OUT:  openPicker(false); break;
        case R_MUTE: toggleMute(); break;
        case R_NC:   eng_.setBypass(!c.bypass);
                     toast(c.bypass ? L"ノイズ抑制 ON" : L"ノイズ抑制 OFF（素通し）"); break;
        case R_AGC:  eng_.setAgcEnabled(!c.agcEnabled);
                     toast(c.agcEnabled ? L"AGC OFF" : L"AGC ON"); break;
        case R_DUR:  restartWith(L"フレーム長を変更", [&](std::wstring* e) {
                         return eng_.setDuration(cycleDuration(c.durationMs, 1), e);
                     }); break;
        case R_MUTEKEY:   beginCapture(false); break;
        case R_GLOBALKEY: beginCapture(true);  break;
        }
        return;
    }

    switch (towupper(ev.ch)) {
    case L'Q': g_quit.store(true); break;
    case L'S': {
        st_.cfg = eng_.config();
        std::wstring err;
        if (saveSettings(st_, &err)) toast(L"保存しました: " + settingsPath(), ATTR_GREEN);
        else                         toast(L"保存に失敗: " + err, ATTR_RED);
        break;
    }
    case L'R':
        restartWith(L"デバイスを再検索", [&](std::wstring* e) {
            return eng_.setDevices(c.inMatch, c.outMatch, e);
        });
        break;
    case L'A':
        st_.ascii = !st_.ascii;
        st_.asciiSet = true;          // 自動判定より利用者の指定を優先する
        scr_.setAscii(st_.ascii);
        scr_.invalidate();
        toast(st_.ascii ? L"字形: ASCII（S で保存）" : L"字形: 罫線（S で保存）");
        break;
    default: break;
    }
}

void App::onKey(const TermEvent& ev) {
    if (ev.ctrl && towupper(ev.ch) == L'C') { g_quit.store(true); return; }
    if (capturing_)   onKeyCapture(ev);
    else if (picking_) onKeyPicker(ev);
    else               onKeyMain(ev);
}

// --- メインループ ---------------------------------------------------------
int App::run() {
    std::wstring err;
    if (!eng_.initKrisp(&err)) {
        errw(L"[エラー] " + err + L"\n");
        return 1;
    }

    // 前回の設定があれば復元する（無ければ既定値のまま）
    loadSettings(&st_);

    // 起動に失敗しても TUI は開く。デバイスを挿し直して R や一覧から選び直せる。
    std::wstring startErr;
    if (!eng_.start(st_.cfg, &startErr)) startErr = L"起動に失敗: " + startErr;
    startedAt_ = GetTickCount64();

    // 自分で開いた窓かどうか。main.cpp が conhost で開き直すときに立てる。
    const bool ownWindow =
        GetEnvironmentVariableW(L"OPENKRISP_CONHOST_CHILD", nullptr, 0) != 0;
    if (!term_.enter(kFixedCols, kFixedRows, ownWindow, st_.style, &err)) {
        eng_.stop();
        errw(L"[エラー] " + err + L"\n"
             L"TUI を使えない環境です。引数を付けたヘッドレス起動をお使いください"
             L"（例: openkrisp --in fifine --out \"CABLE Input\"）。\n");
        return 6;
    }
    g_term = &term_;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // ini に ui.ascii の指定が無ければ、罫線が全角幅で描かれる端末かを実測して決める。
    // conhost + 日本語フォントだと全角になり、罫線版のままでは枠がずれるため。
    if (!st_.asciiSet) st_.ascii = term_.probeAmbiguousDoubleWidth();
    scr_.setAscii(st_.ascii);

    applyGlobalHotkey();

    if (!startErr.empty()) toast(startErr, ATTR_RED);
    else if (term_.appliedFont().empty())
        toast(L"フォントを設定できませんでした（" + st_.style.fontFace + L"）", ATTR_YELLOW);
    else if (term_.appliedFont() != st_.style.fontFace)
        toast(st_.style.fontFace + L" は使えないので " + term_.appliedFont() + L" にしました",
              ATTR_YELLOW);
    else if (!term_.sizeLocked())
        toast(L"端末サイズを固定できませんでした（表示が崩れる場合は手動で広げてください）",
              ATTR_YELLOW);

    stats_ = eng_.stats();
    render();

    unsigned long long lastDraw = 0;
    while (!g_quit.load()) {
        TermEvent ev;
        if (term_.poll(&ev, 50)) {
            if (ev.type == TermEvent::Resize) {
                term_.enforceSize();    // 勝手に変えられたら元のサイズへ戻す
                scr_.invalidate();
            } else if (ev.type == TermEvent::Key) {
                onKey(ev);
            }
            lastDraw = 0;   // 操作直後は即座に描き直す
        }
        // ホットキーは別スレッドから飛んでくるので、ここで拾って切り替える
        if (ghkFired_.exchange(false)) { toggleMute(); lastDraw = 0; }
        // Ctrl+C ハンドラが端末を戻した後に描画すると画面が汚れるので、ここで抜ける
        if (g_quit.load()) break;
        unsigned long long now = GetTickCount64();
        if (now - lastDraw < 50) continue;
        lastDraw = now;

        EngineStats s = eng_.stats();
        if (s.inName.empty())  s.inName  = stats_.inName;
        if (s.outName.empty()) s.outName = stats_.outName;
        stats_ = s;
        // ピークは落ち方を緩めてバーのちらつきを抑える
        inHold_  = (std::max)(s.inPeak,  inHold_  * 0.80f);
        outHold_ = (std::max)(s.outPeak, outHold_ * 0.80f);

        if (!warnedSilent_ && s.totalFrames > 48000 && s.silentFrames == s.totalFrames) {
            toast(L"マイクが常に無音です。ミュート・入力音量・プライバシー設定を確認してください",
                  ATTR_YELLOW);
            warnedSilent_ = true;
        }
        render();
    }

    ghk_.clear();
    term_.leave();
    g_term = nullptr;
    eng_.stop();
    return 0;
}

// 端末に触らず、実運用に近いダミー値で 1 枚だけ組み立てて吐き出す。
int App::uiTest(int cols, int rows, bool ascii, int overlay) {
    dump_ = true; dumpCols_ = cols; dumpRows_ = rows;
    scr_.setAscii(ascii);
    parseKeyBinding(L"Ctrl+Shift+M", &st_.globalMuteKey);

    stats_.running = true;
    stats_.inName  = L"マイク (2- fifine Microphone)";
    stats_.outName = L"CABLE Input (VB-Audio Virtual Cable)";
    stats_.agcGain = 2.3f;
    stats_.fifoMs  = 61;
    stats_.underruns = 0; stats_.drops = 0;
    inHold_ = 0.12f; outHold_ = 0.07f;
    startedAt_ = GetTickCount64() - 754000;   // 00:12:34
    sel_ = R_MUTE;

    if (overlay == 1) {
        pickInput_ = true;
        pickList_ = { {L"マイク (2- fifine Microphone)"},
                      {L"CABLE Output (VB-Audio Virtual Cable)"},
                      {L"とても長い名前のオーディオデバイス（折り返し確認用）"},
                      {L"ステレオ ミキサー (Realtek(R) Audio)"} };
        pickSel_ = 1;
        picking_ = true;
    } else if (overlay == 2) {
        capturing_ = true;
        captureGlobal_ = true;
        captureErr_ = L"修飾キー（Ctrl / Alt / Shift）を1つ以上組み合わせてください";
    }

    render();
    std::wstring text = scr_.dumpText();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                nullptr, 0, nullptr, nullptr);
    std::vector<char> u8((size_t)(n > 0 ? n : 0) + 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   u8.data(), n, nullptr, nullptr);
    fputs(u8.data(), stdout);
    return 0;
}

} // namespace

int runTui(AudioEngine& eng) {
    App app(eng);
    return app.run();
}

int runUiTest(AudioEngine& eng, int cols, int rows, bool ascii, int overlay) {
    App app(eng);
    return app.uiTest(cols, rows, ascii, overlay);
}

int runScreenStress() {
    // 実際に起きた不具合の筋道:
    //   1 フレーム目でデバイス名の全角文字の右半分に半角のスクロール記号を重ね書き
    //   → 左半分が w=2 のまま取り残される
    //   2 フレーム目で flush() が「差分の先頭が全角の右半分なら 1 桁戻す」を行い、
    //     戻した先が前フレームと同じだったため走査位置が後退して空回りする
    // put() が対を保つようになったので、ここは即座に抜けるはず。
    // 出力先はコンソールでなくてよい（WriteConsoleW は失敗するが走査は行われる）。
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Screen s;
    s.resize(40, 3);
    for (int pass = 0; pass < 3; pass++) {
        s.begin();
        s.put(0, 0, L"あいうえおかきくけこ");   // 20 桁ぶんの全角
        s.put(2, 1, L"さしすせそ");
        if (pass == 1) {
            s.put(9, 0, L"^");                  // 全角の右半分を半角で潰す
            s.put(4, 1, L"-");                  // もう一箇所、別の位置でも同じことをする
        }
        s.flush(nul);
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    fputs("screen stress: ok\n", stdout);

    int bad = 0;
    // キー表記の往復。設定ファイルに書いて読み戻せないと、次の起動で割り当てが
    // 黙って既定へ戻ってしまう。
    const wchar_t* cases[] = {
        L"M", L"Ctrl+Shift+M", L"Alt+F1", L"Win+Space", L"F13", L"F24",
        L"Num5", L"NumAdd", L"PageDown", L"Ctrl+Alt+Shift+Win+A", L"なし",
    };
    for (const wchar_t* c : cases) {
        KeyBinding a, b;
        if (!parseKeyBinding(c, &a)) { errw(fmt(L"keybind parse 失敗: %s\n", c)); bad++; continue; }
        const std::wstring text = formatKeyBinding(a);
        if (!parseKeyBinding(text, &b) || !(a == b)) {
            errw(fmt(L"keybind 往復失敗: %s -> %s\n", c, text.c_str()));
            bad++;
        }
    }
    // 修飾キーなしの英数字を全域で奪うと他アプリで文字が打てなくなるので弾く。
    // F13 以降は通常どのアプリも使わないので例外として通す。
    struct { const wchar_t* k; bool ok; } valid[] = {
        { L"M", false }, { L"Ctrl+M", true }, { L"F1", false },
        { L"F13", true }, { L"なし", true },
    };
    for (auto& v : valid) {
        KeyBinding k; std::wstring why;
        parseKeyBinding(v.k, &k);
        if (isValidGlobalHotkey(k, &why) != v.ok) {
            errw(fmt(L"global 判定が期待と違う: %s\n", v.k));
            bad++;
        }
    }
    fputs(bad ? "keybind: NG\n" : "keybind: ok\n", stdout);

    // 実際に登録できるか（専用スレッドとメッセージループが動くか）を確かめる。
    // Ctrl+Alt+F24 は通常どのアプリも使っていない。
    {
        KeyBinding k; std::wstring err;
        parseKeyBinding(L"Ctrl+Alt+F24", &k);
        GlobalHotkey hk;
        if (!hk.set(k, [] {}, &err)) errw(L"hotkey 登録失敗: " + err + L"\n");
        const bool on = hk.active();
        hk.clear();
        fputs(on && !hk.active() ? "hotkey: ok\n" : "hotkey: NG\n", stdout);
        if (!on) bad++;
    }
    return bad ? 1 : 0;
}
