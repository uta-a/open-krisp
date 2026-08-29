# OpenKrisp

Discord に同梱される Krisp のノイズ抑制（NC）を、Discord 以外の通話アプリ
（Zoom / Teams / ブラウザ通話など）でも使えるようにする常駐ブリッジです。

指定したマイクの入力を Krisp NC でノイズ除去し、仮想オーディオケーブル
（既定は VB-CABLE の "CABLE Input"）へ流します。通話アプリ側はそのケーブルの
出力（"CABLE Output"）をマイクとして選ぶだけで、ノイズ除去済みの音声が使えます。

```
[実マイク] → OpenKrisp (Krisp NC) → CABLE Input →(仮想ケーブル)→ CABLE Output → 通話アプリ
```

**Discord の起動は不要**です。自プロセス内で `discord_krisp.node` を読み込んで処理します。

---

## ⚠ 法的・ライセンス上の注意（必読）

- Krisp SDK は **Discord 向けにライセンスされたもの**です。他アプリ向けの利用は
  ライセンス上グレーであり、**バイナリ（`discord_krisp.node` や `.kef` モデル）の
  複製・再配布は明確に禁止**されています。
- 本ツールは、Krisp が実行体に要求する **Authenticode 署名検証（"Discord Inc." 署名）を
  メモリ上で一時的に無効化**して初期化を通します。これは Krisp の技術的保護手段の回避に
  当たります。
- したがって本ツールは **自分のマシン内での個人利用に限定**してください。モジュール
  ファイルはこのリポジトリに含めず、Discord のインストール先から**その場で読み込むだけ**に
  しています（コピー・同梱しない）。
- 正規の手段が必要なら、**Krisp 公式アプリ**（単体版があり、本マシンにも導入済み）や
  NVIDIA RTX Voice などを利用してください。

---

## 動作要件

- Windows 10/11 (x64)
- Discord（安定版 / PTB / Canary いずれか）がインストールされ、Krisp モジュールが
  ダウンロード済みであること
  （`%LOCALAPPDATA%\Discord*\app-*\modules\discord_krisp-*\discord_krisp\discord_krisp.node`）
- 仮想オーディオケーブル（推奨: [VB-CABLE](https://vb-audio.com/Cable/)）
- ビルド: Visual Studio 2022（C++ ツールセット）+ CMake

---

## ビルド

```powershell
.\build.ps1          # vcvars64 を読み込んで構成＋ビルド
.\build.ps1 -Clean   # build/ を作り直してから
```

`build\openkrisp.exe` が生成されます。VS の x64 ネイティブ環境
（`vcvars64.bat` を通したシェル）なら、直接 CMake を叩いても構いません:

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## 使い方

### 1. VB-CABLE を導入
[VB-CABLE](https://vb-audio.com/Cable/) をインストールすると、録音デバイスに
"CABLE Output"、再生デバイスに "CABLE Input" が追加されます。
サンプリングレートは 48000Hz を推奨（サウンドのプロパティで設定。異なっていても
WASAPI 側で変換しますが、48kHz が最も素直です）。

### 2. デバイス確認
```powershell
build\openkrisp.exe --list
```

### 3. 起動
```powershell
build\openkrisp.exe
```
引数なしの場合、入力は「名前に CABLE を含まない最初のマイク」、出力は "CABLE Input" が
選ばれます。明示指定も可能です（部分一致・大文字小文字無視）:

```powershell
build\openkrisp.exe --in fifine --out "CABLE Input"
```

起動すると稼働状況（入出力レベル・バッファ充填量・アンダーラン・破棄回数）を
表示します。停止は `Ctrl+C`。

#### 音質の調整オプション

Discord の Krisp は前段に **AGC（音量の自動調整）** を通しているため、本ツールでも
既定で AGC を有効にしています（声の大小を一定に整え、「変なところで音が小さくなる」のを防ぐ）。
実際の声に合わせて次を調整できます:

| オプション | 説明 |
|---|---|
| `--no-agc` | AGC を無効化（生のまま出す） |
| `--agc-target 0.12` | AGC の目標音量（0〜1、既定 0.12）。大きいほど大音量 |
| `--duration 20` | Krisp のフレーム長 ms（10/15/20/30/32、既定 10）。大きいほど文脈が増え発話保持が安定する場合があるが、レイテンシは増える |
| `--suppression 100` | ノイズ抑制の強さ（0〜100、既定 100） |

合成音声では品質を正しく評価できないため、**自分の声で聴き比べて**調整してください。

### 4. 通話アプリ側の設定
通話アプリのマイク入力に **"CABLE Output (VB-Audio Virtual Cable)"** を選択します。
これでノイズ除去済みの音声が相手に届きます。

---

## 効果の確認（A/B テスト）

実際にノイズが消えるかを WAV で聴き比べるツールを同梱しています。

```powershell
probe\build.ps1  -src abtest.cpp -exe abtest.exe -runArgs @("8","fifine")
# または VS 環境で直接:
#   cl /nologo /utf-8 /EHsc /O2 probe\abtest.cpp /link ole32.lib avrt.lib
#   abtest.exe 8 fifine
```

指定マイクを 8 秒録音し、`captured.wav`（原音）と `denoised.wav`（Krisp 適用後）を
出力します。録音中に話しながら打鍵音・送風・環境音を出して、両者を聴き比べてください。

---

## 仕組み（技術メモ）

`discord_krisp.node`（x64 PE。Krisp 推論エンジンと ONNX Runtime を静的リンク）は、
Node の N-API に加えて **プレーンな C 関数**をエクスポートしています。Discord 本体
（`discord_voice.node`）もこの C API を `LoadLibrary`/`GetProcAddress` で呼んでおり、
本ツールも同じ経路を使います。

使用する主なエクスポート:

| 関数 | 役割 |
|---|---|
| `KrispInitializeExternal()` | グローバル初期化（内部で一度だけ実行）。0 で成功 |
| `KrispNCSetup2(sampleRate, durationMs, 0, 1)` | NC セッション生成。セッションポインタを返す |
| `KrispNCProcessFloat(session, in, inN, out, outN)` | 1 フレーム処理（in/out は float、inN=outN=フレーム長） |
| `KrispNCReset(session)` | セッション破棄 |

- **セッション生成は `KrispNCSetup2` を第4引数 `engine=1` で呼ぶ必要があります**。
  簡易版の `KrispNCSetup`（= `engine=0`）だと、`ProcessFloat` が処理対象を検索する
  ハッシュマップとは**別のマップ**にセッションが登録されてしまい、処理対象が見つからず
  **声もノイズも含めて全て無音**になります（Ghidra 解析で判明。`WORKLOG.md` に詳細）。
  `engine=1` で本来の「声を通しノイズを消す」動作になります。
- サンプルレートは 8/16/24/32/44.1/48/88.2/96kHz、フレーム長は 10/15/20/30/32ms に対応。
  本ツールは **48kHz / 10ms（480 サンプル）** を使用。
- モデルはモジュール内の `.kef` を SDK が自動登録し、既定の **`full_NC`**（広帯域 NC、
  `krisp-nc-o-med-v7` 相当）が読み込まれます。
- 署名検証は初期化時の内部関数（本バージョンでは RVA `0x53110`）が行い、失敗すると
  `-3`（"Application not signed by Discord"）を返します。本ツールはロード後・初期化前に
  この関数の先頭 3 バイトを `mov al,1; ret` に書き換えて常に成功させます。
  **ファイルは改変せず、プロセスメモリ上のみ**を変更します。

---

## 既知の制約

- **バージョン依存**: 署名検証関数の RVA(`0x53110`) はこの `discord_krisp.node`
  （15,071,160 bytes）に固定です。Discord の更新でモジュールが変わると、先頭バイトの
  ガードにより**安全側で初期化を中止**します（誤ったパッチは当てません）。その場合は
  新バージョンでオフセットを再特定する必要があります。
- **クロックドリフト**: キャプチャとレンダは別クロックのため長時間でわずかにずれます。
  第1版は 60ms の緩衝バッファ＋過充填時の 1 フレーム破棄／枯渇時の無音挿入という
  素朴な補正です（適応リサンプリングは未実装）。実運用で数十分に一度程度の軽微な
  プチノイズが出る可能性があります。
- **リサンプリング**は線形補間（検証・実用上は十分だが高品位ではない）。
- **AEC（エコー除去）は未実装**。Discord は AEC も併用しますが、ヘッドセットマイクでは
  スピーカー音の回り込みが小さいため影響は限定的です。スピーカー再生時に相手へ
  エコーが乗る場合は第2版で対応。
- スコープは **NC（ノイズ除去）+ AGC + 入出力デバイス指定**。モデル切替 UI・VAD ゲート・
  トレイ常駐・自動起動は未実装。

---

## ファイル構成

```
CMakeLists.txt
src/
  main.cpp          エントリ、デバイス選択、パイプライン
  krisp_shim.h/.cpp モジュール探索・ロード・署名検証回避・C API バインド
  wasapi_io.h/.cpp  WASAPI キャプチャ／レンダ（48kHz mono float 境界）
  ring_buffer.h     SPSC ロックフリー・リングバッファ
probe/
  abtest.cpp        効果確認用の録音→処理→WAV 出力ツール
  build.ps1         単体ビルド用スクリプト
tools/              解析用スクリプト（PE 解析・逆アセンブル・xref）
```
