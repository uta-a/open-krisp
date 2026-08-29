// openkrisp.ini の読み書き。exe と同じフォルダに置く。
//
// TUI で S キーを押したときだけ書き出す（起動しただけではファイルを作らない）。
// 引数付きのヘッドレス起動ではこのファイルを読まない。「引数で指定したものが全て」
// という第1版の性質を壊さないため。
#pragma once
#include "engine.h"
#include "hotkey.h"
#include "tui_screen.h"
#include <string>

struct Settings {
    EngineConfig cfg;
    // 罫線とメーターを ASCII にする。conhost + 日本語フォントだと罫線が
    // 全角幅で描かれて枠がずれることがあるため、その逃げ道。
    bool ascii = false;
    // ini に ascii の行があったか。無ければ端末を実測して自動で決めるので、
    // 「利用者が明示的に選んだ」のか「単に書かれていない」のかを区別する。
    bool asciiSet = false;

    // TUI の中でミュートを切り替えるキー（修飾なしの 1 打を想定）
    KeyBinding muteKey{ 0, 'M' };
    // どのアプリを使っていても効くキー。既定は未割り当て（何も奪わない）。
    KeyBinding globalMuteKey;

    // conhost で開き直した窓の見た目（フォントと透過）
    TermStyle style;
    // 配色
    Theme theme = Theme::ClaudeDark;
    // TUI を conhost で開き直すか。false なら呼び出し元の端末でそのまま動く
    // （Windows Terminal のタブの中など）。その場合ウィンドウサイズは固定できない。
    bool useConhost = true;
};

// exe と同じフォルダの openkrisp.ini のフルパス。
std::wstring settingsPath();

// 読み込む。ファイルが無い／壊れている場合は out を変更せず false を返す
// （呼び出し側は既定値のまま起動できる）。
bool loadSettings(Settings* out);

// 一時ファイルへ書いてから置換する。書きかけの壊れた ini を残さない。
bool saveSettings(const Settings& s, std::wstring* err);
