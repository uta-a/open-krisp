// 従来どおりの引数指定・ヘッドレス動作。スクリプトからの無人起動用に残している。
//
// 引数を 1 つでも付けて起動した場合はこちらが使われ、openkrisp.ini は読まない
// （「引数で指定したものが全て」という第1版の性質を保つため）。
#pragma once

class AudioEngine;

// デバイス一覧を表示する（--list）。
int runList(AudioEngine& eng);

// 引数を解釈してパイプラインを回し、1行ステータスを表示し続ける。
int runCli(int argc, wchar_t** argv, AudioEngine& eng);

// 引数取り出しの小道具。TUI 側でも初期値の解釈に使う。
const wchar_t* argVal(int argc, wchar_t** argv, const wchar_t* key, const wchar_t* def);
bool hasFlag(int argc, wchar_t** argv, const wchar_t* key);
