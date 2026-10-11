# USF Maker

N64 の ROM を読み込ませるだけで、USF（Ultra 64 Sound Format / `.usflib` + `.miniusf`）を自動で作る Windows アプリです。

USF は「ROM のうち再生に必要な部分」と「曲が鳴り始める直前の Project64 セーブステート」を 1 つにまとめた形式で、
ふつうは Project64 で曲ごとにステートを取り、専用ツールで切り出して作ります。USF Maker はこの作業を自動化します。

- ROM をドロップして [開始] を押すだけ。ゲームを画面なしで高速に動かし、自動でボタンを押して進めながら、音楽が鳴り始めた瞬間を集めます。
- 見つけた曲ごとに、ループ位置・曲の長さ・フェードを解析してタグに書き込みます。
- 同じ曲の重複は自動でまとめます。
- ROM は「再生中に実際に読まれた部分」だけを `.usflib` に入れます（丸ごと入れる設定もあります）。
- 自動操作でたどり着けない曲は、Project64 のセーブステート（`.pj` / `.pj.zip`）から追加できます。
- 一覧から試聴・名前の変更・削除ができます。
- 日本語 / 英語 UI（Windows の表示言語で自動切り替え）。

作者: Nekokabu

---

## ビルド方法（Visual Studio Community 2026）

必要なもの

- Visual Studio Community 2026（ワークロード「**C++ によるデスクトップ開発**」。MSVC v145 と Windows SDK が入ります）
- 外部ライブラリは不要です。必要なものはすべてこのフォルダに入っています。

手順

1. `USFMaker.sln` を Visual Studio で開きます。
2. 上部のツールバーで構成を **Release**、プラットフォームを **x64** にします。
3. メニューの [ビルド] → [ソリューションのビルド]（Ctrl+Shift+B）。
4. `bin\x64\Release\` に次の 2 つができます。
   - `USFMaker.exe` … ウィンドウ版（通常はこちらを使います）
   - `usfmaker-cli.exe` … コマンドライン版

C ランタイムは静的リンクなので、できた exe は単体で動きます（再頒布パッケージ不要）。

コマンドラインからビルドする場合は「Developer PowerShell for VS 2026」で:

```
msbuild USFMaker.sln -p:Configuration=Release -p:Platform=x64 -m
```

補足

- プロジェクトはプラットフォーム ツールセット **v145**（VS 2026）向けです。VS 2022 でビルドしたい場合は、ソリューションを右クリック →
  [ソリューションの再ターゲット] でツールセットを v143 にしてください（コードは C++17 / C で、特定の版に依存していません）。
- エミュレーションコア（`lazyusf2` プロジェクト）は Debug 構成でも最適化を有効にしています。最適化なしでは探索が実用的な速さになりません。
- Windows 以外での開発・テスト用に `Makefile`（GCC / MinGW-w64）も入っています。Windows でのビルドには不要です。

---

## 使い方

1. `USFMaker.exe` を起動し、ROM ファイル（`.z64` / `.n64` / `.v64`、zip 圧縮のままでも可）をウィンドウにドロップします。
2. 必要なら「ゲーム名」「作曲者」を入力します（タグとファイル名に使われます）。
3. 「探索時間」を選んで **[開始]** を押します。
   - 探索中も、見つかった曲が一覧に次々と追加されます。[停止] でいつでも打ち切れます。
   - 「自動でボタンを押して先へ進む」を外すと、無操作で流れる音楽（タイトルやデモ）だけを集めます。
4. 一覧の曲を **[試聴]** で確認します。タイトルは F2 キー（または [名前の変更]）で変更、不要な曲（効果音など）は [削除] で消せます。
5. **[USF を保存]** で保存先フォルダを選ぶと、`<ゲーム名>.usflib` と、チェックを付けた曲ごとの `NN タイトル.miniusf` が書き出されます。

### 見つからない曲があるとき

自動操作はタイトル画面・メニュー・ゲーム序盤までは進めますが、ゲームを攻略するわけではないので、終盤の曲などは見つけられません。
その場合は次のようにして追加できます。

1. Project64 で、目的の曲が**鳴り始める直前**（無音の瞬間が理想）にセーブステートを作ります。
2. USF Maker に同じ ROM を読み込んだ状態で **[セーブステートから追加]** を押し、ステートファイルを選びます（ウィンドウへのドロップでも可）。
   - Project64 1.6 ～ 現行版の `.pj`、および既定設定で作られる `.pj.zip` に対応しています。
   - 追加した曲も、自動で見つけた曲と同じようにループ・長さを解析します。

探索時間を長くする、[詳細設定] の「自動操作のパターン番号」を変えてもう一度探索する、という方法でも見つかる曲が増えることがあります
（すでに一覧にある曲は残り、新しく見つかった曲だけが追加されます）。

### 詳細設定

| 項目 | 説明 |
| --- | --- |
| 拡張パックあり（メモリー 8 MB）で動かす | 外すと 4 MB の本体として動かします。拡張パックがあると動作が変わるゲーム用。 |
| 起動後、無操作で様子を見る時間 | 電源投入後、何も押さずにデモなどを聴く長さ（ゲーム内時間）。 |
| 同時に動かす数 | 並列に動かすエミュレーターの数。0 で CPU コア数に合わせます（最大 8）。 |
| 自動操作のパターン番号 | ボタンの押し方の乱数の種。変えると別の経路を探索します。 |
| ループ曲を繰り返す回数 / フェードアウト | ループ曲の `length` / `fade` タグの決め方。 |
| ループも終わりも不明な曲の長さ | 解析できなかった曲に付ける長さ。 |
| ROM 全体を usflib に入れる | 通常は再生に必要な部分だけを入れます。再生中に音が欠ける曲があるときに試してください。 |

### コマンドライン版

```
usfmaker-cli <rom> [オプション]             ROM から USF 一式を作る
usfmaker-cli --render <usf> <wav> [秒数]    .usf / .miniusf を WAV に書き出す

  -o <フォルダ>       出力先（既定: ROM と同じ場所の「<ROM 名> (USF)」）
  --time <秒>         探索に使う実時間（既定 300）
  --no-buttons        ボタンを押さず、無操作で流れる音楽だけを集める
  --state <ファイル>  Project64 のセーブステートから曲を追加（複数指定可。--time 0 ならステートだけを処理）
  --game <名前>       ゲーム名（タグ、ファイル名）
  --artist <名前>     作曲者タグ
  --threads <n>       並列数
  --full-rom          ROM 全体を usflib に入れる
  --4mb               拡張パックなしで動かす
  --eeprom <4|16>     カートリッジの EEPROM 容量（kbit）。既定はゲームから自動判定
  --no-save-memory    カートリッジのセーブメモリーなしで探索する
  --loops <n>         ループ回数（既定 2）
  --fade <秒>         フェード（既定 10）
  --seed <n>          自動操作のパターン番号
  --wav               各曲の WAV も書き出す
  -v                  詳細表示
```

### 作ったファイルの再生

lazyusf2 を使う USF プレイヤーで再生できます（foobar2000 + foo_input_usf など）。
`.usflib` と `.miniusf` は同じフォルダに置いてください。タグは UTF-8 で書き込みます（`utf8=1`）。
プレイヤーが `.usflib` を見つけられない場合は、ゲーム名を半角英数字にして保存し直してみてください（ゲーム名が `.usflib` のファイル名になります）。

---

## しくみ

1. **探索** … カートリッジを電源投入の状態から、画面を描かないエミュレーターで動かします。コントローラーを挿した状態にして、
   スクリプト（スタート連打、決定、メニュー移動、プレイ操作など）でボタンを押します。出力が「無音 → 音あり」に変わるたびに、
   その直前に取っておいたセーブステートを「曲の候補」にします。候補の位置は、そこから先をさらに探索する分岐点にもなります。
   ROM の新しい部分を読んだ経路ほど優先して掘り下げます。
2. **確定** … 候補を、USF プレイヤーが読み込むのと同じ条件（コントローラーなし、標準設定）で読み込み直して再生します。
   ちゃんと鳴ること、すでにある曲と同じでないこと（音声の指紋で照合）、短すぎないことを確かめ、ループ周期または終わりを検出し、
   再生中に読まれた ROM の範囲を記録します。
3. **書き出し** … 全曲が読んだ ROM 範囲の和集合と、基準になるセーブステート 1 つを `.usflib` に、各曲は基準との差分だけを
   `.miniusf` に書きます。書いたファイルは読み戻して検証します。

エミュレーションコアは、USF プレイヤーの標準実装である **lazyusf2**（mupen64plus 由来）です。プレイヤーと同じコアで作って確かめるので、
作ったファイルはプレイヤーでもそのまま同じ音で鳴ります。もともとは「セーブステートから再生する」だけのコアなので、USF Maker 用に
電源投入からの起動、コントローラー入力、セーブステートの書き出し、カートリッジのセーブメモリー（EEPROM / SRAM / FlashRAM）などを
追加しています（`src/lazyusf2/CHANGES-USFMaker.md` に一覧があります。追加機能は有効にしない限り元の動作のままです）。

---

## 制限事項・注意

- **すべての曲が自動で見つかるわけではありません。** 見つかるのは自動操作でたどり着ける場面の曲です。残りはセーブステートから追加してください。
- **効果音や短いジングルが曲として拾われることがあります。** 試聴して不要なら削除してください。
- **動かないゲームがあります。** lazyusf2 は音楽再生用のコアで、ゲームを最初から動かす用途では通常のエミュレーターほどの互換性はありません。
  起動しない・音が出ないゲームでは曲が見つかりません（その場合もセーブステートからの追加は使えます）。
- 64DD、Aleck64（業務用基板）のイメージには対応していません。
- 探索は実時間で打ち切るため、同じ ROM でも実行のたびに見つかる曲や順番が少し変わることがあります。
- ループ検出は音声の解析によるものです。まれに、ループ位置や長さが期待と違うことがあります。その場合はタグ（`length` / `fade`）を
  タグエディターで直してください。
- ROM ファイルはご自身で所有するカートリッジから吸い出したものを使ってください。作成した USF には ROM の一部が含まれます。

### 検証について

開発時に確認できたのは次の範囲です。

- 自由に配布されている自作ソフト（N64brew Game Jam 2020 の libultra 製 ROM、NTSC / PAL）での探索・解析・書き出し。
- 生成した `.usflib` / `.miniusf` が、手を加えていない lazyusf2（プレイヤー側の実装）でサンプル単位まで同じ音で再生されること。
- MSVC（v142 ツールセット）と MinGW-w64 でのビルド、および Wine 上での GUI / コマンドライン版の動作。
- EEPROM / SRAM / FlashRAM の応答（テスト用の小さな ROM による確認）。

市販ゲームの ROM での動作確認、および Visual Studio 2026（v145）実機でのビルド確認は、開発環境の都合で行えていません。
問題があれば、エラーメッセージや対象ゲームとあわせて知らせてください。

---

## フォルダ構成

```
USFMaker.sln              Visual Studio ソリューション
src/gui/                  ウィンドウ版（Win32 API、追加ライブラリなし）
src/cli/                  コマンドライン版
src/engine/               探索・音声解析・USF 読み書き（C++17）
src/lazyusf2/             エミュレーションコア（C、USF Maker 用の変更入り）
Makefile                  GCC / MinGW 用（開発・テスト用）
LICENSE                   GNU General Public License version 2
THIRD-PARTY-NOTICES.txt   同梱している第三者コードの著作権表示
```

## ライセンス

USF Maker は GNU General Public License version 2 またはそれ以降（GPL-2.0-or-later）で配布します。全文は `LICENSE` にあります。

- `src/lazyusf2/` は lazyusf2（kode54 ほか）で、mupen64plus（GPL v2 以降）に由来します。RSP の LLE 実装（`rsp_lle/`）は
  Iconoclast 氏による CC0、RSP HLE（`rsp_hle/`）は mupen64plus-rsp-hle（GPL v2 以降、`rsp_hle/LICENSES` 参照）です。
  取り込み元は Cog（https://github.com/losnoco/Cog）同梱版です。
  CIC-6105 の応答計算（`si/n64_cic_nus_6105.*`）は X-Scale 氏による 2 条項 BSD ライセンスです。
  各著作権表示は `THIRD-PARTY-NOTICES.txt` にまとめています。exe だけを配布する場合も、`LICENSE` とこのファイルを添え、ソースを入手できるようにしてください。
- USF 形式については https://hcs64.com/usf/ および https://www.vgmpf.com/Wiki/index.php?title=USF を参照しました。

---

## English summary

USF Maker builds USF sets (`.usflib` + `.miniusf`, the N64 counterpart of NSF/SPC) from an N64 ROM automatically.
It boots the cartridge in a headless emulator core (lazyusf2), presses buttons with a scripted "player", takes a save
state whenever music starts, verifies each candidate the way a USF player would load it, detects loop points and
lengths, removes duplicates, and writes a trimmed library plus one miniusf per song. Tunes the automatic player cannot
reach can be added from Project64 save states (`.pj`, `.pj.zip`).

Build: open `USFMaker.sln` in Visual Studio 2026 (Desktop development with C++), select Release / x64, build.
The binaries appear in `bin\x64\Release`. No external dependencies. The UI is English on non-Japanese systems
(`/lang:en` or `/lang:ja` forces a language).

License: GPL-2.0-or-later.
