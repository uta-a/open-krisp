#include "tui_app.h"
#include "tui_screen.h"
#include "engine.h"
#include "settings.h"
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>

namespace {

const int kMinCols = 48;
const int kMinRows = 17;
const int kBarCells = 16;      // メーターの桁数（-60dB..0dB）
const int kLabelW = 12;        // 設定行のラベル欄の幅

// 操作できる行。モデル行は表示のみなのでここには入れない。
enum Row { R_IN = 0, R_OUT, R_NC, R_AGC, R_DUR, R_COUNT };

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

// -60dB..0dB を kBarCells 段のバーへ。
std::wstring meterBar(const Glyphs& gl, float peak) {
    int lv = 0;
    if (peak > 0.f) {
        double db = 20.0 * log10((double)peak);
        lv = (int)((db + 60.0) / 60.0 * kBarCells + 0.5);
    }
    lv = (std::max)(0, (std::min)(kBarCells, lv));
    std::wstring s;
    for (int i = 0; i < kBarCells; i++) s += (i < lv) ? gl.barOn : gl.barOff;
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
    int uiTest(int cols, int rows, bool ascii, bool picker);

private:
    void render();
    void renderMain(int bx, int by, int boxW);
    void renderPicker();
    void onKey(const TermEvent& ev);
    void onKeyMain(const TermEvent& ev);
    void onKeyPicker(const TermEvent& ev);
    void openPicker(bool input);
    void applyPicked();
    void toast(const std::wstring& s, uint16_t attr = ATTR_NONE);
    void restartWith(const std::wstring& what, const std::function<bool(std::wstring*)>& op);

    AudioEngine& eng_;
    Term   term_;
    Screen scr_;
    Settings st_;

    int  sel_ = R_NC;
    bool picking_ = false;
    bool pickInput_ = false;
    std::vector<DeviceInfo> pickList_;
    int  pickSel_ = 0;
    int  pickTop_ = 0;

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

// --- 描画 -----------------------------------------------------------------
void App::render() {
    int cols = 0, rows = 0;
    if (dump_) { cols = dumpCols_; rows = dumpRows_; }
    else       term_.size(&cols, &rows);
    scr_.resize(cols, rows);
    scr_.begin();

    if (cols < kMinCols || rows < kMinRows) {
        // 狭い端末でも読めるよう 2 行に分けて中央に出す
        const std::wstring m1 = L"端末が小さすぎます";
        const std::wstring m2 = fmt(L"最小 %dx%d / 現在 %dx%d", kMinCols, kMinRows, cols, rows);
        const int y = (std::max)(0, rows / 2 - 1);
        scr_.put((std::max)(0, (cols - strWidth(m1)) / 2), y, m1, ATTR_YELLOW);
        scr_.put((std::max)(0, (cols - strWidth(m2)) / 2), y + 1, m2, ATTR_DIM);
        if (!dump_) scr_.flush(term_.out());
        return;
    }

    const int boxW = (std::min)((std::max)(cols - 2, kMinCols - 2), 64);
    const int bx = (cols - boxW) / 2;
    const int by = 0;
    renderMain(bx, by, boxW);
    if (picking_) renderPicker();
    if (!dump_) scr_.flush(term_.out());
}

// 端末に触らず、実運用に近いダミー値で 1 枚だけ組み立てて吐き出す。
int App::uiTest(int cols, int rows, bool ascii, bool picker) {
    dump_ = true; dumpCols_ = cols; dumpRows_ = rows;
    scr_.setAscii(ascii);

    stats_.running = true;
    stats_.inName  = L"マイク (2- fifine Microphone)";
    stats_.outName = L"CABLE Input (VB-Audio Virtual Cable)";
    stats_.agcGain = 2.3f;
    stats_.fifoMs  = 61;
    stats_.underruns = 0; stats_.drops = 0;
    inHold_ = 0.12f; outHold_ = 0.07f;
    startedAt_ = GetTickCount64() - 754000;   // 00:12:34
    sel_ = R_NC;

    if (picker) {
        pickInput_ = true;
        pickList_ = { {L"マイク (2- fifine Microphone)"},
                      {L"CABLE Output (VB-Audio Virtual Cable)"},
                      {L"とても長い名前のオーディオデバイス（折り返し確認用）"} };
        pickSel_ = 1;
        picking_ = true;
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

void App::renderMain(int bx, int by, int boxW) {
    const Glyphs& gl = scr_.g();
    const int inner = boxW - 2;
    const int x0 = bx + 1;            // 枠の内側の左端
    const int labelX = x0 + 1;        // カーソル 1 桁ぶん右
    const int valueX = labelX + kLabelW;

    scr_.box(bx, by, boxW, 14, ATTR_NONE);
    scr_.put(bx + 2, by, L" OpenKrisp ", ATTR_BOLD | ATTR_CYAN);
    {
        std::wstring up = L" 稼働 " + hhmmss(startedAt_ ? GetTickCount64() - startedAt_ : 0) + L" ";
        scr_.put(bx + boxW - 1 - strWidth(up) - 1, by, up, ATTR_DIM);
    }

    // デバイス
    scr_.put(labelX, by + 1, padTo(L"入力", 6));
    scr_.put(labelX + 6, by + 1, truncWidth(stats_.inName, inner - 8, gl.ellipsis));
    scr_.put(labelX, by + 2, padTo(L"出力", 6));
    scr_.put(labelX + 6, by + 2, truncWidth(stats_.outName, inner - 8, gl.ellipsis));
    scr_.hsep(bx, by + 3, boxW);

    // メーター
    scr_.put(labelX, by + 4, L"入力 ");
    scr_.put(labelX + 5, by + 4, meterBar(gl, inHold_), ATTR_GREEN);
    scr_.put(labelX + 5 + kBarCells + 1, by + 4, dbText(inHold_) + L" dB", ATTR_DIM);
    scr_.put(labelX, by + 5, L"出力 ");
    scr_.put(labelX + 5, by + 5, meterBar(gl, outHold_), ATTR_CYAN);
    scr_.put(labelX + 5 + kBarCells + 1, by + 5, dbText(outHold_) + L" dB", ATTR_DIM);
    scr_.hsep(bx, by + 6, boxW);

    // 設定行
    EngineConfig c = eng_.config();
    const int rowY[R_COUNT] = { by + 1, by + 2, by + 7, by + 8, by + 9 };
    for (int r = 0; r < R_COUNT; r++)
        scr_.put(x0, rowY[r], (sel_ == r) ? gl.cursor : L" ",
                 (sel_ == r) ? (ATTR_BOLD | ATTR_CYAN) : ATTR_NONE);

    const uint16_t selAttr = ATTR_BOLD;
    // ノイズ抑制
    scr_.put(labelX, by + 7, padTo(L"ノイズ抑制", kLabelW), sel_ == R_NC ? selAttr : ATTR_NONE);
    scr_.put(valueX, by + 7, c.bypass ? L"OFF" : L"ON ",
             c.bypass ? ATTR_RED : ATTR_GREEN);
    scr_.put(valueX + 6, by + 7, fmt(L"強度 %3.0f", c.suppression),
             c.bypass ? ATTR_DIM : ATTR_NONE);
    // AGC
    scr_.put(labelX, by + 8, padTo(L"AGC", kLabelW), sel_ == R_AGC ? selAttr : ATTR_NONE);
    scr_.put(valueX, by + 8, c.agcEnabled ? L"ON " : L"OFF",
             c.agcEnabled ? ATTR_GREEN : ATTR_DIM);
    scr_.put(valueX + 6, by + 8, fmt(L"目標 %.2f", c.agcTarget),
             c.agcEnabled ? ATTR_NONE : ATTR_DIM);
    if (c.agcEnabled && valueX + 17 < bx + boxW - 1)
        scr_.put(valueX + 17, by + 8, fmt(L"gain %.1fx", stats_.agcGain), ATTR_DIM);
    // フレーム長
    scr_.put(labelX, by + 9, padTo(L"フレーム長", kLabelW), sel_ == R_DUR ? selAttr : ATTR_NONE);
    scr_.put(valueX, by + 9, fmt(L"%d ms", c.durationMs));
    // モデル（切替は未対応なので淡色）
    scr_.put(labelX, by + 10, padTo(L"モデル", kLabelW), ATTR_DIM);
    scr_.put(valueX, by + 10, L"full_NC", ATTR_DIM);

    scr_.hsep(bx, by + 11, boxW);
    scr_.put(labelX, by + 12,
             fmt(L"Fifo %2dms  UR %llu  破棄 %llu", stats_.fifoMs, stats_.underruns, stats_.drops),
             ATTR_DIM);

    // 操作説明と、その下の通知行
    scr_.put(bx, by + 14,
             truncWidth(fmt(L" %s%s選択  %s%s値  Enter 切替/決定  S 保存  Q 終了",
                            gl.up, gl.down, gl.left, gl.right),
                        scr_.cols() - bx, gl.ellipsis),
             ATTR_DIM);
    if (!msg_.empty() && GetTickCount64() < msgUntil_)
        scr_.put(bx, by + 15, truncWidth(L" " + msg_, scr_.cols() - bx - 1, gl.ellipsis), msgAttr_);
    else if (!stats_.notice.empty())
        scr_.put(bx, by + 15, truncWidth(L" " + stats_.notice, scr_.cols() - bx - 1, gl.ellipsis),
                 ATTR_YELLOW);
    else if (!stats_.running)
        scr_.put(bx, by + 15, L" 停止中", ATTR_RED);
}

void App::renderPicker() {
    const Glyphs& gl = scr_.g();
    const int cols = scr_.cols(), rows = scr_.rows();
    const int w = (std::min)(cols - 8, 52);
    const int listRows = (std::max)(3, (std::min)((int)pickList_.size(), rows - 8));
    const int h = listRows + 2;          // 上下の枠のぶんだけ足す（余白は作らない）
    const int x = (cols - w) / 2;
    const int y = (std::max)(1, (rows - h) / 2);

    for (int i = 0; i < h; i++) scr_.fill(x, y + i, w, L" ");
    scr_.box(x, y, w, h, ATTR_CYAN);
    std::wstring title = pickInput_ ? L" 入力デバイスを選ぶ " : L" 出力デバイスを選ぶ ";
    scr_.put(x + 2, y, title, ATTR_BOLD | ATTR_CYAN);

    if (pickSel_ < pickTop_) pickTop_ = pickSel_;
    if (pickSel_ >= pickTop_ + listRows) pickTop_ = pickSel_ - listRows + 1;

    for (int i = 0; i < listRows; i++) {
        int idx = pickTop_ + i;
        if (idx >= (int)pickList_.size()) break;
        const bool cur = (idx == pickSel_);
        std::wstring line = (cur ? std::wstring(gl.cursor) : L" ") + L" " +
                            truncWidth(pickList_[idx].name, w - 5, gl.ellipsis);
        scr_.put(x + 1, y + 1 + i, padTo(line, w - 2), cur ? ATTR_REV : ATTR_NONE);
    }
    if (pickTop_ > 0) scr_.put(x + w - 2, y + 1, gl.up, ATTR_DIM);
    if (pickTop_ + listRows < (int)pickList_.size())
        scr_.put(x + w - 2, y + h - 2, gl.down, ATTR_DIM);
    scr_.put(x + 1, y + h - 1, fmt(L" %s%s Enter 決定  Esc 戻る ", gl.up, gl.down), ATTR_DIM);
}

// --- 操作 -----------------------------------------------------------------
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
        }
        return;
    }

    if (ev.vk == VK_RETURN) {
        switch (sel_) {
        case R_IN:  openPicker(true);  break;
        case R_OUT: openPicker(false); break;
        case R_NC:  eng_.setBypass(!c.bypass);
                    toast(c.bypass ? L"ノイズ抑制 ON" : L"ノイズ抑制 OFF（素通し）"); break;
        case R_AGC: eng_.setAgcEnabled(!c.agcEnabled);
                    toast(c.agcEnabled ? L"AGC OFF" : L"AGC ON"); break;
        case R_DUR: restartWith(L"フレーム長を変更", [&](std::wstring* e) {
                        return eng_.setDuration(cycleDuration(c.durationMs, 1), e);
                    }); break;
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
        scr_.setAscii(st_.ascii);
        scr_.invalidate();
        toast(st_.ascii ? L"字形: ASCII（S で保存）" : L"字形: 罫線（S で保存）");
        break;
    default: break;
    }
}

void App::onKey(const TermEvent& ev) {
    if (ev.ctrl && towupper(ev.ch) == L'C') { g_quit.store(true); return; }
    if (picking_) onKeyPicker(ev);
    else          onKeyMain(ev);
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

    if (!term_.enter(&err)) {
        eng_.stop();
        errw(L"[エラー] " + err + L"\n"
             L"TUI を使えない環境です。引数を付けたヘッドレス起動をお使いください"
             L"（例: openkrisp --in fifine --out \"CABLE Input\"）。\n");
        return 6;
    }
    g_term = &term_;
    scr_.setAscii(st_.ascii);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    if (!startErr.empty()) toast(startErr, ATTR_RED);

    stats_ = eng_.stats();
    render();

    unsigned long long lastDraw = 0;
    while (!g_quit.load()) {
        TermEvent ev;
        if (term_.poll(&ev, 50)) {
            if (ev.type == TermEvent::Resize) { scr_.invalidate(); }
            else if (ev.type == TermEvent::Key) onKey(ev);
            lastDraw = 0;   // 操作直後は即座に描き直す
        }
        // Ctrl+C ハンドラが端末を戻した後に描画すると画面が汚れるので、ここで抜ける
        if (g_quit.load()) break;
        unsigned long long now = GetTickCount64();
        if (now - lastDraw < 50) continue;
        lastDraw = now;

        EngineStats s = eng_.stats();
        s.inName = s.inName.empty() ? stats_.inName : s.inName;
        s.outName = s.outName.empty() ? stats_.outName : s.outName;
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

    term_.leave();
    g_term = nullptr;
    eng_.stop();
    return 0;
}

} // namespace

int runTui(AudioEngine& eng) {
    App app(eng);
    return app.run();
}

int runUiTest(AudioEngine& eng, int cols, int rows, bool ascii, bool picker) {
    App app(eng);
    return app.uiTest(cols, rows, ascii, picker);
}
