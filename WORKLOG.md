# krisp-bridge 作業ログ（2026-08-29 時点）

## 現状
`discord_krisp.node` の C API に相乗りしてマイク入力をノイズ除去し VB-CABLE へ流す
常駐アプリ `krisp-bridge` を実装。パイプライン(WASAPI capture→Krisp→render)は安定動作するが、
**Krisp NC が声を含めて全部消す（出力が常に無音）** という未解決バグがある。

## 確定した事実（RE + 実測）
- 署名検証回避OK: RVA 0x53110 の3バイトを `B0 01 C3` にメモリパッチ → 初期化成功
- 初期化成功: `KrispInitializeExternal()`=0、"Initialized Krisp SDK successfully"
- **モデルは正しくロードされている**: IATフック(hooktest.cpp)で観測。
  `GetModuleFileNameW(discord_krisp.node)` でモジュールパス取得 →
  `CreateFileW(...\krisp-nc-o-med-v7.kef)` を **OK(成功)で2回オープン**。
  → KMS/cwd/モデルパスの問題では *ない*（これらは全部潰した）。
- セッション確立OK: `KrispNCSetup(48000,10)` / `Setup2(48000,10,0,0)` が有効ポインタを返す
- `KrispNCProcessFloat(session, in, inN, out, outN)` は **rc=0(成功)** を返す。
  引数: 第5=出力長(statsではない。inN=outN=480必須)。抑制レベルは内部でグローバル注入。
- 入力は明瞭な音声(ピーク0.846)だが **出力ピーク=0.0000（完全無音）**
- suppressionグローバル(DA20B0=100.0f)を0/50/100に変えても無音 → 抑制は無関係
- MKL環境変数(MKL_ENABLE_INSTRUCTIONS=AVX2, MKL_DEBUG_CPU_TYPE=5)でも無音

## 最有力の次アクション（優先順）
1. **`setMklZenOverrideEnabled`（napiメソッド）をCで呼ぶ**。
   このマシンはAMD CPU。KrispはIntel MKLを使い、AMD(Zen)ではMKL推論が無効化され
   無音を出す既知問題。環境変数では効かず、napiの setMklZenOverrideEnabled(true) が
   内部で正しい設定をしている可能性。→ napi実体アドレスを特定してCから呼ぶ。
   （napiメソッド定義は sub_39CD0 内。setMklZenOverrideEnabled のコールバック実体を解析）
2. **node で napi 経由の完全初期化 + setMklZenOverrideEnabled を実行**し、
   同一プロセスで自作アドオンから C エクスポート ProcessFloat を叩く検証。
   napi版初期化がC版と違う何かをしていないか最終確認。
3. ProcessFloat の内部 sub_847AF0 を再精査。出力が本当に arg4 に書かれるか、
   推論 sub_848FA0 の戻りを確認。"DfProcessor::initInferEngine(): Failed to load network"
   等のエラーがログレベルを上げれば見えるかも（ログ閾値はTLS経由: DC7698=msgLevel,
   DC7BE8=TLS slot, TLS[slot]+0x38=threshold。DC7678直書きはNG=クラッシュ実績あり）。

## 有効だった解析手法
- IATフック(hooktest.cpp): CreateFileW/GetModuleFileNameW を差し替えてファイルアクセス観測。
  → 次は同手法で MKL DLL ロードや推論の失敗を観測できる。
- tools/: pe.py(func_bounds/find_callers/imports 追加済), dis_at.py, dis_range.py, xref.py
- SAPIで voice.wav 生成: `Add-Type -AssemblyName System.Speech` で48k16bit mono音声

## 動作している部分（第1版として完成）
- src/{main.cpp,krisp_shim.*,wasapi_io.*,ring_buffer.h}: WASAPIパイプライン、
  デバイス自動選択、入出力レベルバー表示、--bypass(素通し)、--list。
  bypassでは入力→出力が通る（マイク・VB-CABLE・レンダは全て正常）。
- 唯一 Krisp NC 処理段が無音を出すのみ。上記1が解ければ完成。

## 2026-08-29 追加調査（セッション2）
判明:
- CPU=AMD Ryzen 9 8945HX 確定
- **推論が全く走っていないことを計測で確定**: ProcessFloatは0.003-0.004ms/frame。
  ニューラルNCなら最低数十us必要。=推論エンジンが処理せず0埋め出力。
- `setMklZenOverrideEnabled`実体=RVA 0x57AE0（`mov [DC7BD0],ecx; ret`のみ）。
  Init前後・ON/OFF・正弦波入力すべてで無音。→MKL Zen問題ではない。
- SetModel("full_NC")をSetup前に明示呼び出しても無音。→呼び出し順ではない。
- **Discord本体(discord_voice.node)はKrispInitializeExternalを呼んでいない**。
  代わりにGetProcAddressで関数テーブル(SetModel+0,Setup+8,Setup2+0x10,Reset+0x18,
  Process+0x20,ProcessFloat+0x28,VADSetup+0x38...)をowner+0x100に構築(loadKrispFns=voice RVA 0x19AB70、呼び出し元 0xCBF40/0xCD520)。
  ロード直後sub_A33B0→"Krisp initialized successfully"。初期化経路がCと異なる可能性。
- ProcessFloat実体sub_847AF0: session[0x10]をsub_848FA0に渡す。session[0x10]の[0]は
  .text内vtable(0x7FFD101C7488)を指すC++オブジェクト。プロセッサリスト構造の静的解釈は
  多階層shared_ptr/vtableで難航中（begin/end解釈が誤り、実測でゴミ値）。
- 出力書き込みsub_C944E0=memcpy(内部出力vec→呼び出し元)。内部出力vecが0。
  fail分岐0x847c63="NcSession::process: Output frame size is not equal to"（サイズ不一致時）。

次アクション候補:
1. **Ghidra導入**（JDK21併設）でsub_847AF0/848FA0/Setup経路のC++構造をデコンパイル。
   静的手作業の限界。プロセッサ有効化条件を特定するのが本筋。
2. Discord本体の初期化経路(voice 0xCBF40, sub_A33B0)を精読し、C版と違う前提設定
   （setKrispPath等)を特定して再現。
3. APIモニタ的手法: ProcessFloat実体にトランポリンフック仕込んでsession内部を
   実行時ダンプ（introspect系の続き。正しいオフセットを実測で当てる）。

## 2026-08-30 Ghidra解析で無音の直接原因を特定
Ghidra 12.1.3(JDK21併設)導入済。tools/ghidra_dl/、proj*/。DecompDump.javaで対象RVAをデコンパイル。
※ -process -noanalysis はOSGiエラーが出るので毎回 -import 新規プロジェクトで実行(約3分)。

**無音の直接原因（確定）**:
ProcessFloat実体 FUN_180837690:
```c
FUN_1808380f0(&local_30, session);  // rdx=session をキーにハッシュマップ検索
if (local_30 != 0) FUN_180839f80(local_30, in,inN,out,outN,supp,stats); // 実処理
// local_30==0 なら何もせず return → out未初期化 → 無音
```
- FUN_8380f0(RVA 0x8380f0): sessionポインタをFNV-1aハッシュ(0x100000001b3, offset 0xcbf29ce484222325)化し、
  グローバルハッシュマップ(bucket=DC7AD8, sentinel=DC7AC8, mask=DC7AF0=7→8バケット)を検索。
  見つかった処理オブジェクト(entry+0x18/+0x20)を local_30 に返す。無ければ0。
- FUN_834000: マップを空初期化(atexit登録)。LoadLibrary時点で存在。
- FUN_837a60: マップから削除(erase)。callers=0x838255等。
- **マップにsessionを登録(insert)する箇所が、C API Setup経路で呼ばれていない疑い**。
  実測(mapcheck.cpp): Setup前後でマップ内容不変、session=0x..3510とsentinel=0x..3170が近接ヒープ。
- NcSession生成テーブル DA2000: [0]=835b40,[8]=835d10,[0x10]=835d40,[0x18]=836b40,[0x20]=836f00,
  [0x28]=837540(NCProcess int16実体),[0x30]=837690(NCProcessFloat実体),[0x38]=8372c0。
  Setup2はbl(=param_3 byte)で DA2000[bl*8+0x18? ]... 実際は table+0x18 起点をicall。
- **次**: FUN_836b40(NcSession生成, Setup2がicallする実体)がマップinsertするか解析中。
  もしDiscordが別途insertしていて、C API Setupがinsertしないなら、Setup後に手動insert or
  ProcessFloatに正しいキーを渡す必要。あるいはSetupの戻り値sessionと登録キーの不一致を修正。

有効ツール: probe/mapcheck.cpp(マップ状態実測), proc_state.cpp, hook848.cpp(トランポリン;15B境界)。

## 2026-08-30 【解決】無音バグの真因と修正
**真因**: KrispNCSetup(=engine=0)が呼ぶセッション生成関数(DA2000[0x18]=FUN_836b40 createNc)は
map1(DC7A80基点)にセッションを登録するが、ProcessFloat実体(FUN_837690)が検索するのは
map2(DC7AC0基点, bucket=DC7AD8)。別マップのため検索が外れ local_30=0 → 無処理 → 無音。

**修正**: KrispNCSetup2(sr, dur, 0, **engine=1**)を使う。engine=1だとicall先が
DA2000[0x20]=FUN_836f00 になりmap2へ登録 → ProcessFloatが対象を見つけ推論実行。
- KrispNCSetup2の第4引数(r9d=bl)がicallインデックス: target=DA2000[bl*8+0x18]。
  bl=0→createNc(map1), bl=1→836f00(map2)。第3引数(r8d=esi)は別用途。

**実証(voicetest.cpp / noisetest.cpp)**:
- engine=1: 処理時間0.13ms/frame(推論走行), voice.wav声保持90%(out/in=0.95),
  ホワイトノイズ抑制率100%(0.173→0.0001)。
- engine=0: 0.004ms/frame(推論なし), 全消し。

**本体修正済**: src/krisp_shim.h の ncSetup() を NCSetup2_(sr,durMs,0,1) に変更、
NCSetup2_t typedef と GetProcAddress("KrispNCSetup2")追加。ビルド成功・起動安定。
残: 実マイクでの実発話ライブ確認(ユーザー担当。build/krisp-bridge.exe 起動→喋る→出力バー確認)。

## 2026-08-30 品質改善: AGC追加（Discordとの差の主因）
ユーザー報告「Discordより劣化・変なところで音が小さくなる」を調査。
- 切り分け結果: Krispコアは同一(full_NC, level不変), 抑制レベル/入力レベル/フレーム長は主因でない,
  パイプラインは60秒でUR=0破棄=0で綺麗。
- **真因: Discordは「音量調節の自動化」(AGC)がON**(ユーザーのDiscord設定スクショで確認)。
  Krispはlevel不変なので入力の大小が出力にそのまま出る→小声や離席で「音が小さくなる」。
  Discordは前段AGCで音量を一定化している。
- **対応: AGC実装(src/agc.h)をKrispノイズ除去後に適用。既定ON**。
  フレーム単位RMSレベル推定+緩やかなゲイン移動(上げ遅め=ポンピング回避)+無音時ゲイン据え置き
  (ノイズ増幅防止)+tanhソフトリミッタ。単体検証: 入力10:1の音量差→AGC後1.7:1に平坦化、ハードクリップなし。
- CLI追加: --no-agc, --agc-target 0.12(既定), --duration(10/15/20/30/32ms), --suppression(0-100)。
  実音声でのチューニング用。合成音声(SAPI)では品質評価が不正確なため実声で調整推奨。
- 注: エコー除去(AEC)は未実装。ヘッドセットマイクでは影響小。必要なら第2版。

---

## 2026-08-30 第2版 (OpenKrisp)

`krisp-bridge` を **OpenKrisp** に改名し、全操作を TUI 化した。
本ログは第1版の解析記録であり、内容はそのまま有効（署名検証 RVA `0x53110`、
セッションマップと `engine=1` の件、抑制強度 RVA `0xDA20B0`）。
第2版での実装上の変更は README.md と git のコミット履歴を参照。

## 2026-09-04 Discord 更新(9256)で開かなくなった → 署名/抑制 RVA 再特定 + 自己修復化
Stable が app-1.0.9255→9256 に更新され、discord_krisp.node が 15,071,160→15,066,552 bytes に変化。
固定 RVA 0x53110 の先頭バイトが合わず「署名検証関数の先頭バイトが想定と異なります」で起動不可に。

再特定（tools/ を KRISP_NODE 環境変数で新旧モジュールへ向けて解析）:
- 署名検証トランポリン: 旧 0x53110 → 新 **0x523E0**。形は不変:
  `48 8D 15 <d1>`(lea rdx,["Discord Inc."]) `4C 8D 05 <d2>`(lea r8,[data]) `E9 <rel>`(jmp 本体)。
  "Discord Inc." 文字列は .rdata:0xCB5990、参照命令末尾 0x523E7 → 命令先頭 0x523E0。
- 抑制グローバル: 旧 0xDA20B0 → 新 **0xDA10B0**（-0x1000）。周辺で float==100.0 は 1 箇所のみ。
  参照元も新旧で 4 箇所・同トポロジー、いずれも `movss xmm0,[rip+global]` で一致。

修正(src/krisp_shim.*):
- kSigCheckRva=0x523E0, kSuppressionRva=0xDA10B0 に更新。
- **自己修復**: 固定 RVA がトランポリン("Discord Inc." 参照 + 形)でなければ、ロード済み
  イメージ(SizeOfImage)全体を走査して同じ形を探し、そこをパッチ。今後の更新で位置が
  ずれても開けるようにした。先頭 3 バイト一致だけでなく文字列参照まで検証して誤爆防止。
- **抑制の値域ガード**: ロード時に kSuppressionRva の float が 0-100 のときだけ有効化。
  版ズレで別 global を指す場合は書き込みを無効化し、無関係な値を壊さない。
- tools/dis_exports.py の MODULE を KRISP_NODE 環境変数 or 最新自動探索に変更。

実証: 9256 をロードして "Initialized Krisp SDK successfully"→NC セッション確立、
--suppression 70 受理。--uitest --stress 3項目 ok。
