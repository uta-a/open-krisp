// キーバインドの文字列化／解釈と、GlobalHotkey の実装。
//
// 【この .cpp が持っている前提】
//   ・キー名の表は 1 つだけ（kKeyNames）。format と parse が同じ表を引く。
//     名前を 2 箇所に持つと、片方だけ直したときに「保存はできるが読み戻せない」
//     という気付きにくい壊れ方をする。
//   ・GlobalHotkey は専用スレッドを 1 本立て、その中で RegisterHotKey →
//     GetMessage ループ → UnregisterHotKey までを完結させる（理由は hotkey.h 冒頭）。
#include "hotkey.h"
#include <cstdio>
#include <cwchar>

// RegisterHotKey / UnregisterHotKey / PostThreadMessageW / PeekMessageW に要る。
// CMakeLists.txt にリンク指定を足さずに済ませるため、ここで指定する。
#pragma comment(lib, "user32.lib")

// Windows 7 以降で有効なフラグ。SDK の _WIN32_WINNT 設定次第で見えないことが
// あるので、無ければ自前で定義しておく（値は winuser.h と同じ）。
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

namespace {

// RegisterHotKey へ渡してよい修飾ビット。壊れた ini から想定外のビットが
// そのまま API へ流れないよう、登録前にここで漉す。
const unsigned int kModMask = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

// hWnd = NULL のホットキー ID はスレッド単位の名前空間で、このスレッドは
// GlobalHotkey しか使わない。よって固定値で他とぶつかることはない。
const int kHotkeyId = 1;

// set() がスレッドの返事を待つ上限。万一スレッドが立ち上がらなくても
// UI を固まらせないための保険。
const DWORD kReadyTimeoutMs = 2000;

// --- キー名の表 -----------------------------------------------------------
// A-Z / 0-9 は「名前が文字そのもの」（'M' <-> VK 0x4D）で管理する名前が無いため
// 表には入れず、keyName()/keyFromName() で同じ規則を使う。
// それ以外の名前はすべてこの表が唯一の出どころ。
struct KeyName { unsigned int vk; const wchar_t* name; };

const KeyName kKeyNames[] = {
    { VK_F1,  L"F1"  }, { VK_F2,  L"F2"  }, { VK_F3,  L"F3"  }, { VK_F4,  L"F4"  },
    { VK_F5,  L"F5"  }, { VK_F6,  L"F6"  }, { VK_F7,  L"F7"  }, { VK_F8,  L"F8"  },
    { VK_F9,  L"F9"  }, { VK_F10, L"F10" }, { VK_F11, L"F11" }, { VK_F12, L"F12" },
    { VK_F13, L"F13" }, { VK_F14, L"F14" }, { VK_F15, L"F15" }, { VK_F16, L"F16" },
    { VK_F17, L"F17" }, { VK_F18, L"F18" }, { VK_F19, L"F19" }, { VK_F20, L"F20" },
    { VK_F21, L"F21" }, { VK_F22, L"F22" }, { VK_F23, L"F23" }, { VK_F24, L"F24" },

    { VK_SPACE,  L"Space"  }, { VK_RETURN, L"Enter"     }, { VK_TAB,    L"Tab"    },
    { VK_ESCAPE, L"Esc"    }, { VK_BACK,   L"Backspace" }, { VK_DELETE, L"Delete" },
    { VK_INSERT, L"Insert" }, { VK_HOME,   L"Home"      }, { VK_END,    L"End"    },
    { VK_PRIOR,  L"PageUp" }, { VK_NEXT,   L"PageDown"  },
    { VK_UP,     L"Up"     }, { VK_DOWN,   L"Down"      },
    { VK_LEFT,   L"Left"   }, { VK_RIGHT,  L"Right"     },

    { VK_NUMPAD0, L"Num0" }, { VK_NUMPAD1, L"Num1" }, { VK_NUMPAD2, L"Num2" },
    { VK_NUMPAD3, L"Num3" }, { VK_NUMPAD4, L"Num4" }, { VK_NUMPAD5, L"Num5" },
    { VK_NUMPAD6, L"Num6" }, { VK_NUMPAD7, L"Num7" }, { VK_NUMPAD8, L"Num8" },
    { VK_NUMPAD9, L"Num9" },
    { VK_ADD,     L"NumAdd" }, { VK_SUBTRACT, L"NumSub" },
    { VK_MULTIPLY, L"NumMul" }, { VK_DIVIDE,  L"NumDiv" },
};

// 修飾子の別名。表記ゆれで ini が読めなくなるのを防ぐ。
struct ModName { const wchar_t* name; unsigned int bit; };

const ModName kModNames[] = {
    { L"Ctrl",  MOD_CONTROL }, { L"Control", MOD_CONTROL }, { L"Ctl", MOD_CONTROL },
    { L"Alt",   MOD_ALT     }, { L"Menu",    MOD_ALT     },
    { L"Shift", MOD_SHIFT   },
    { L"Win",   MOD_WIN     }, { L"Super",   MOD_WIN     },
};

std::wstring keyName(unsigned int vk) {
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9'))
        return std::wstring(1, (wchar_t)vk);
    for (const KeyName& e : kKeyNames)
        if (e.vk == vk) return e.name;
    // 表に無いキーでも ini へ書いて読み戻せるようにしておく。
    // ここで諦めると、割り当てたキーが保存のたびに消える。
    wchar_t buf[16];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"VK_%u", vk);
    return buf;
}

bool keyFromName(const std::wstring& s, unsigned int* vk) {
    if (s.empty()) return false;
    if (s.size() == 1) {
        wchar_t c = s[0];
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
            *vk = (unsigned int)c;
            return true;
        }
        return false;   // 記号 1 文字は配列によって vk が変わるので VK_ 形式で書いてもらう
    }
    for (const KeyName& e : kKeyNames)
        if (_wcsicmp(s.c_str(), e.name) == 0) { *vk = e.vk; return true; }

    if (s.size() > 3 && _wcsnicmp(s.c_str(), L"VK_", 3) == 0) {
        unsigned int n = 0;
        for (size_t i = 3; i < s.size(); ++i) {
            if (s[i] < L'0' || s[i] > L'9') return false;
            n = n * 10 + (unsigned int)(s[i] - L'0');
            if (n > 255) return false;      // 仮想キーコードは 1..255
        }
        if (n == 0) return false;           // 0 は「未割り当て」の意味なので受けない
        *vk = n;
        return true;
    }
    return false;
}

bool modFromName(const std::wstring& s, unsigned int* mod) {
    for (const ModName& m : kModNames)
        if (_wcsicmp(s.c_str(), m.name) == 0) { *mod = m.bit; return true; }
    return false;
}

// 修飾キーそのもの。単独では押しても意味がなく、RegisterHotKey も受け付けない。
bool isModifierVk(unsigned int vk) {
    switch (vk) {
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:    case VK_RWIN:
            return true;
        default:
            return false;
    }
}

std::wstring trimws(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring registerErrorText(DWORD e) {
    // 圧倒的に多いのはこれ。原因が分からないと直しようがないので言い切る。
    if (e == ERROR_HOTKEY_ALREADY_REGISTERED)
        return L"このキーの組み合わせは他のアプリが使用中です。別の組み合わせにしてください。";
    wchar_t buf[128];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                 L"ホットキーを登録できませんでした（エラー %lu）。", e);
    return buf;
}

}  // namespace

// --- 文字列化 / 解釈 ------------------------------------------------------

std::wstring formatKeyBinding(const KeyBinding& k) {
    if (!k.assigned()) return L"なし";
    // 並びは固定。同じ組み合わせが呼ぶたび違う順で出ると、画面表示も
    // ini の差分も安定せず、文字列同士の比較もできなくなる。
    std::wstring s;
    if (k.mods & MOD_CONTROL) s += L"Ctrl+";
    if (k.mods & MOD_ALT)     s += L"Alt+";
    if (k.mods & MOD_SHIFT)   s += L"Shift+";
    if (k.mods & MOD_WIN)     s += L"Win+";
    s += keyName(k.vk);
    return s;
}

bool parseKeyBinding(const std::wstring& s, KeyBinding* out) {
    if (!out) return false;
    std::wstring t = trimws(s);
    // 空欄と "なし"（format の出力）の両方を未割り当てとして受ける。
    if (t.empty() || t == L"なし") { *out = KeyBinding(); return true; }

    KeyBinding k;
    size_t pos = 0;
    for (;;) {
        size_t plus = t.find(L'+', pos);
        size_t end  = (plus == std::wstring::npos) ? t.size() : plus;
        std::wstring tok = trimws(t.substr(pos, end - pos));
        // "Ctrl+" や "+M" のような欠けた指定は、直感に反する解釈をするより
        // 解釈できないものとして扱う（呼び出し側が入力し直せる）。
        if (tok.empty()) return false;
        if (plus == std::wstring::npos) {
            // 最後の要素がキー本体。"Ctrl+Alt" のような修飾子だけの並びはここで落ちる。
            if (!keyFromName(tok, &k.vk)) return false;
            break;
        }
        unsigned int bit = 0;
        if (!modFromName(tok, &bit)) return false;
        k.mods |= bit;
        pos = plus + 1;
    }
    *out = k;
    return true;
}

bool isValidGlobalHotkey(const KeyBinding& k, std::wstring* why) {
    if (why) why->clear();
    if (!k.assigned()) return true;   // 未割り当ては「解除」の意味なのでエラーではない

    if (isModifierVk(k.vk)) {
        if (why) *why = L"修飾キー単体は登録できません。文字キーや F キーと組み合わせてください。";
        return false;
    }
    // F13..F24 は物理キーボードにほぼ無く、常駐アプリ以外が使うこともないので単独を許す。
    // それ以外を修飾キーなしで全域から奪うと、他のアプリで文字が打てなくなる。
    if ((k.mods & kModMask) == 0 && !(k.vk >= VK_F13 && k.vk <= VK_F24)) {
        if (why) *why = L"修飾キー（Ctrl / Alt / Shift / Win）を1つ以上組み合わせてください"
                        L"（F13〜F24 だけは単独で使えます）。";
        return false;
    }
    return true;
}

// --- GlobalHotkey ---------------------------------------------------------

GlobalHotkey::~GlobalHotkey() { clear(); }

bool GlobalHotkey::active() const {
    std::lock_guard<std::mutex> lk(mx_);
    return cur_.assigned();
}

KeyBinding GlobalHotkey::current() const {
    std::lock_guard<std::mutex> lk(mx_);
    return cur_;
}

void GlobalHotkey::clear() {
    std::lock_guard<std::mutex> lk(mx_);
    stopThread();
    cb_  = nullptr;      // スレッドが抜けた後にだけ触る
    cur_ = KeyBinding();
}

void GlobalHotkey::stopThread() {
    if (thread_) {
        // WM_QUIT はスレッドがメッセージキューを作った後でないと届かない
        // （それ以前は ERROR_INVALID_THREAD_ID で失敗する）。登録に成功していれば
        // 必ず作られているが、起動直後に畳む経路のために届くまで撃ち直す。
        for (int i = 0; i < 200; ++i) {
            if (PostThreadMessageW(tid_, WM_QUIT, 0, 0)) break;
            if (WaitForSingleObject(thread_, 10) == WAIT_OBJECT_0) break;  // 既に抜けている
        }
        // 抜け切るまで待つ。待たずに次の RegisterHotKey へ進むと、前の登録が
        // 残っていて必ず「使用中」で失敗する。また cb_ を差し替える前に
        // スレッドが消えていることを保証する必要がある。
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        tid_    = 0;
    }
    if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
}

DWORD WINAPI GlobalHotkey::threadProc(void* self) {
    reinterpret_cast<GlobalHotkey*>(self)->loop();
    return 0;
}

void GlobalHotkey::loop() {
    // メッセージキューはスレッドが初めてメッセージ API を呼んだ時点で作られる。
    // RegisterHotKey より先に必ず作っておく（キューが無い間に押された分は消える）。
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // MOD_NOREPEAT は必須。付けないとキーを押しっぱなしにしている間ずっと
    // オートリピートで WM_HOTKEY が飛び、ミュートが高速に反転してしまう。
    const UINT mods = (want_.mods & kModMask) | MOD_NOREPEAT;
    bool ok = RegisterHotKey(nullptr, kHotkeyId, mods, want_.vk) != 0;
    regErr_.store(ok ? 0u : GetLastError(), std::memory_order_relaxed);
    regOk_.store(ok, std::memory_order_release);
    SetEvent(ready_);            // ここから先、set() は結果を読んでよい
    if (!ok) return;

    // GetMessage は失敗すると -1 を返すので > 0 で判定する。
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == (WPARAM)kHotkeyId && cb_)
            cb_();               // 呼び出し側は atomic を立てるだけ（hotkey.h の約束）
    }
    // 登録したスレッドと同じスレッドで外す必要がある。
    UnregisterHotKey(nullptr, kHotkeyId);
}

bool GlobalHotkey::set(const KeyBinding& k, std::function<void()> onFire, std::wstring* err) {
    std::lock_guard<std::mutex> lk(mx_);
    if (err) err->clear();

    // 何をするにせよ、まず今の登録を外す。同じ組み合わせへの掛け直しは、
    // 外す前に登録しようとすると必ず「使用中」で失敗するため。
    // 掛け替えのたびにスレッドを作り直すのは、登録内容をスレッドへ渡す経路を
    // 「起動前に書くだけ」に保てて、実行中の受け渡しを一切作らずに済むから。
    // 掛け替えは利用者の操作なので、頻度は問題にならない。
    stopThread();
    cb_  = nullptr;
    cur_ = KeyBinding();

    if (!k.assigned()) return true;   // 解除だけして成功

    std::wstring why;
    if (!isValidGlobalHotkey(k, &why)) {
        if (err) *err = why;
        return false;
    }
    if (!onFire) {
        // 通知先が無いまま登録すると、キーを全域から奪うだけで何も起きない
        // 状態になり、利用者からは原因が分からない。
        if (err) *err = L"ホットキーの通知先が指定されていません。";
        return false;
    }

    // 実際に登録するのは漉した後の mods。current() も同じものを返して、
    // 「表示されている組み合わせ」と「登録されている組み合わせ」をずらさない。
    KeyBinding norm;
    norm.mods = k.mods & kModMask;
    norm.vk   = k.vk;

    cb_   = std::move(onFire);
    want_ = norm;
    regOk_.store(false, std::memory_order_relaxed);
    regErr_.store(0, std::memory_order_relaxed);

    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) {
        cb_ = nullptr;
        if (err) *err = L"ホットキーの同期用イベントを作成できませんでした。";
        return false;
    }
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, &tid_);
    if (!thread_) {
        CloseHandle(ready_); ready_ = nullptr;
        cb_ = nullptr;
        if (err) *err = L"ホットキー用スレッドを作成できませんでした。";
        return false;
    }

    // 登録の成否をその場で画面に出せるよう、結果が出るまで待つ。
    // 万一スレッドが応答しなくても UI を固まらせないよう上限を切る。
    if (WaitForSingleObject(ready_, kReadyTimeoutMs) != WAIT_OBJECT_0) {
        stopThread();
        cb_ = nullptr;
        if (err) *err = L"ホットキーの登録が時間内に終わりませんでした。";
        return false;
    }
    if (!regOk_.load(std::memory_order_acquire)) {
        DWORD e = regErr_.load(std::memory_order_relaxed);
        stopThread();
        cb_ = nullptr;
        if (err) *err = registerErrorText(e);
        return false;
    }

    cur_ = norm;
    return true;
}
