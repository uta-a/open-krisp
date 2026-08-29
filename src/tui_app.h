// OpenKrisp の TUI 本体。引数なしで起動したときの画面。
//
// デバイス切替・音質パラメータの調整・設定の保存を、アプリを止めずに画面上で行う。
#pragma once

class AudioEngine;

// 戻り値はプロセスの終了コード。VT 非対応の端末では案内を出して 0 以外を返す。
int runTui(AudioEngine& eng);

// 画面を 1 枚だけ組み立てて、装飾なしのテキストとして標準出力へ出す（--uitest）。
// Krisp もオーディオも起動しないので、罫線の桁ずれだけを安全に確認できる。
int runUiTest(AudioEngine& eng, int cols, int rows, bool ascii, bool picker);
