// OpenKrisp の TUI 本体。引数なしで起動したときの画面。
//
// デバイス切替・ミュート・音質パラメータの調整・設定の保存を、
// アプリを止めずに画面上で行う。
//
// 画面は端末いっぱいに描き、端末サイズは起動時に固定サイズへ合わせて
// リサイズできないようにする（レイアウトが崩れる余地をなくすため）。
#pragma once

class AudioEngine;

// 起動時に要求する端末サイズ。この大きさで過不足なく収まるようレイアウトしてある。
extern const int kFixedCols;
extern const int kFixedRows;

// 戻り値はプロセスの終了コード。VT 非対応の端末では案内を出して 0 以外を返す。
int runTui(AudioEngine& eng);

// 画面を 1 枚だけ組み立てて、装飾なしのテキストとして標準出力へ出す（--uitest）。
// Krisp もオーディオも起動しないので、罫線の桁ずれだけを安全に確認できる。
// overlay: 0=なし / 1=デバイス一覧 / 2=キー割り当て
int runUiTest(AudioEngine& eng, int cols, int rows, bool ascii, int overlay);

// 「デバイス一覧を開くと固まる」不具合の再現テスト（--uitest --stress）。
// 全角の右半分を半角で上書きしてセルの対を壊し、次のフレームの flush() が
// 走査位置を戻し続けて抜けられなくなる、という筋道をなぞる。
// 直っていれば即座に 0 を返し、壊れていれば返ってこない。
int runScreenStress();
