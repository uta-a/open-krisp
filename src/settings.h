// openkrisp.ini の読み書き。exe と同じフォルダに置く。
//
// TUI で S キーを押したときだけ書き出す（起動しただけではファイルを作らない）。
// 引数付きのヘッドレス起動ではこのファイルを読まない。「引数で指定したものが全て」
// という第1版の性質を壊さないため。
#pragma once
#include "engine.h"
#include <string>

struct Settings {
    EngineConfig cfg;
    // 罫線とメーターを ASCII にする。conhost + 日本語フォントだと罫線が
    // 全角幅で描かれて枠がずれることがあるため、その逃げ道。
    bool ascii = false;
};

// exe と同じフォルダの openkrisp.ini のフルパス。
std::wstring settingsPath();

// 読み込む。ファイルが無い／壊れている場合は out を変更せず false を返す
// （呼び出し側は既定値のまま起動できる）。
bool loadSettings(Settings* out);

// 一時ファイルへ書いてから置換する。書きかけの壊れた ini を残さない。
bool saveSettings(const Settings& s, std::wstring* err);
