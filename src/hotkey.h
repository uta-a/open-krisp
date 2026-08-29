// キーバインドの表現と、他のアプリを使っている最中でも効くグローバルホットキー。
//
// OpenKrisp は通話のあいだ裏で動き続ける常駐アプリなので、ミュートの切り替えは
// 「ゲームやブラウザを触ったまま」できないと意味がない。TUI 内のキー割り当てと
// グローバルホットキーの両方が同じ KeyBinding を使い、同じ文字列表現で
// openkrisp.ini へ書き戻せるようにしてある。
//
// 【なぜ専用スレッドが要るのか】
//   RegisterHotKey に hWnd = NULL を渡すと、WM_HOTKEY は「登録したスレッドの
//   メッセージキュー」へ直接届く。つまり登録・受信・解除をすべて同じスレッドで
//   行わないと受け取れない。OpenKrisp の UI スレッドは WaitForMultipleObjects で
//   コンソール入力を待っており、メッセージを汲む余地がない。そこで GlobalHotkey は
//   専用スレッドを 1 本持ち、その中だけで登録からメッセージループ、解除までを完結させる。
//
// 【スレッド安全性の約束】
//   onFire はホットキースレッドから呼ばれる。UI スレッドとは完全に無関係の
//   タイミングで走るので、呼び出し側は atomic フラグを立てるだけにして、
//   実際の処理は UI スレッドの次のフレームで行うこと。
//   このモジュール側では onFire の前後でロックを取らない（TUI の状態を
//   別スレッドから直接触らせない、という境界をここで引くため）。
//   set() / clear() / active() / current() は互いに排他してあるので、
//   どのスレッドから呼んでもよい。
#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// キーの組み合わせ。mods は MOD_CONTROL / MOD_ALT / MOD_SHIFT / MOD_WIN の OR。
// vk は仮想キーコード（'A'..'Z', '0'..'9', VK_F1.. など）。vk == 0 は「未割り当て」。
struct KeyBinding {
    unsigned int mods = 0;
    unsigned int vk   = 0;
    bool assigned() const { return vk != 0; }
    bool operator==(const KeyBinding& o) const { return mods == o.mods && vk == o.vk; }
    bool operator!=(const KeyBinding& o) const { return !(*this == o); }
};

// "Ctrl+Shift+M" のような表示用文字列にする。未割り当てなら "なし"。
// 修飾子の並びは常に Ctrl+Alt+Shift+Win+<キー> に固定する。
std::wstring formatKeyBinding(const KeyBinding& k);

// 上記の逆。解釈できなければ false（out は変更しない）。
// 受け付ける形: "M" / "F13" / "Ctrl+Shift+M" / "Alt+F1" / "Win+Space" / "" (未割り当て)
// 区切りは '+'、大文字小文字は区別しない。修飾子の別名として
// Control=Ctrl=Ctl, Alt=Menu, Win=Super も受ける。
bool parseKeyBinding(const std::wstring& s, KeyBinding* out);

// グローバルホットキーとして登録してよい組み合わせか。
// 修飾キーなしの英数字を全域で奪うと他アプリで文字が打てなくなるため、
// 修飾キーを 1 つ以上要求する。ただし F13..F24 は通常どのアプリも使わないので例外。
// 駄目な場合は理由を why に入れる（日本語）。
bool isValidGlobalHotkey(const KeyBinding& k, std::wstring* why);

// どのアプリを使っていても効くホットキー。
// 内部で専用スレッドを起こし、そのスレッドで RegisterHotKey とメッセージループを回す。
class GlobalHotkey {
public:
    ~GlobalHotkey();

    // 登録する。既に登録済みなら差し替える。vk == 0 なら解除だけして true を返す。
    // 登録の成否が出るまで呼び出し元スレッドをブロックする（UI がその場で
    // 「登録できませんでした」を出せるようにするため。最大 2 秒で打ち切る）。
    // 他のアプリが同じ組み合わせを既に使っていると失敗するので、
    // err に日本語の理由を入れて false を返す（アプリは動き続けられる）。
    // 失敗した場合はホットキー無しの状態になる（active() == false）。
    //
    // isValidGlobalHotkey を満たさない組み合わせと、空の onFire も false にする。
    // 前者を素通しすると他アプリで文字が打てなくなり、後者は「キーを全域で
    // 奪うのに何も起きない」という直しようのない状態になるため。
    //
    // onFire はホットキースレッドから呼ばれる。ファイル冒頭の約束を守ること。
    bool set(const KeyBinding& k, std::function<void()> onFire, std::wstring* err);

    // 登録を外してスレッドを畳む。多重呼び出し・未登録での呼び出しとも安全。
    void clear();

    bool active() const;
    KeyBinding current() const;

private:
    static DWORD WINAPI threadProc(void* self);
    void loop();                 // ホットキースレッドの本体
    void stopThread();           // mx_ を保持した状態で呼ぶこと

    mutable std::mutex mx_;      // 以下のメンバをまとめて守る
    HANDLE thread_ = nullptr;
    DWORD  tid_    = 0;
    HANDLE ready_  = nullptr;    // 登録の成否が出たことを set() へ知らせる
    KeyBinding want_;            // スレッドへ渡す登録内容（スレッド起動前にだけ書く）
    KeyBinding cur_;             // 実際に登録できている組み合わせ
    std::function<void()> cb_;   // スレッドが居ない間にだけ差し替える
    std::atomic<bool>  regOk_{false};
    std::atomic<DWORD> regErr_{0};
};
