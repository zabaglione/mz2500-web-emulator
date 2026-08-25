# MZ-2500 Web Emulator

## ▶ ブラウザで今すぐ動かす — <https://zabaglione.github.io/mz2500-web-emulator/>

インストール不要。標準形式に加え、トラックごとにセクタ数、C/H/R/N、密度、物理順序が異なる
特殊フォーマットのD88も扱えます。実機 ROM がなくても、IPLPRO形式のD88を起動できます。
IPLPRO互換でない有効なD88は、利用者が登録した `ipl.rom` を使って実IPLで起動します。
ユーザーが挿入したD88はブラウザ内に保存され、次回のPOWER ONで自動的に再挿入されます。
保存済みD88がない場合は無ディスクで待機します。

SHARP MZ-2500 のブラウザエミュレータ。C++ コア → Emscripten/WASM + HTML/JS 構成
（コンセプト参照: [claude-famicom-emu](https://github.com/GOROman/cluade-famicom-emu)）。

![title screen](docs/screenshot-title.png)

**実機 ROM と特殊D88は一切使用・同梱していません。** IPLPRO互換D88のフロッピー
ブート動作だけは、ホスト側ネイティブコード（`core/dummy_ipl.cpp`）が代替する
IPLPROブート実装で起動できます。可変レコードや独自IPLを含むその他の有効なD88は、
利用者所有の `ipl.rom` をROMスロットへ登録してください。ROMなしで特殊D88を起動
しようとした場合は、無限待機せず必要なROMを画面に表示して停止します。

> **動作について**
> 市販ソフトやフリーソフトで動作検証を行っていますが、**すべてのソフトウェアの
> 動作を保証するものではありません**。未実装の周辺機器や、実機と挙動が異なる
> 箇所が残っています。動かないソフトがあってもご容赦ください。

動作検証タイトルとして、自作アクションゲーム **NEKO CAN RUN の無料デモ版
（WORLD 1）** を同梱しています。デモ版・製品版（有償・全4ワールド）は
[BOOTH（ザバイオーネ研究室）](https://zabaglione.booth.pm/items/8672891) にて配布中。
同梱デモは画面下の **NEKO** ボタンで明示的に起動し、保存済みD88を上書きしません。

もうひとつの同梱イメージとして、**CP/M 2.2 のハードディスク**
（[cpm-mz2500](https://github.com/zabaglione/cpm-mz2500)、開発ツール・言語入り）を
画面下の **CP/M** ボタンから起動できます。内蔵IPLがEH-SASI形式のパーティション表を
直接読んでHDDブートするため、ROM登録は不要です。HDDへの書き込みはブラウザに
保存され、次回起動時も続きから使えます。

## 実装範囲

MZ-2500 のうち、IPLPRO ブートのゲームソフトが使う範囲を実装:

- Z80B 6MHz（superzazu/z80、16bit I/O アドレスパッチ）
- バンクメモリ 64×8KB（B4h/B5h、IN(B5h) セレクタ自動+1 の癖を含む）
- MB8877 FDC 読み書き両系（READ/WRITE SECTOR〈単一・マルチセクタ、
  ディレクテッドアドレスマークを含む〉、READ ADDRESS、READ/WRITE TRACK
  〈物理フォーマット〉、STEP系、ライトプロテクト、DEhによるFM/MFM選択。
  同一C/H/Rが混在するD88でも密度を含めてレコードを選択し、MFM 250kbps /
  FM 125kbpsの転送周期と1回転バイト数を反映）
  + D88（反転バス）。C/H/R/N、密度、削除データマーク、格納データ長、物理順序を
  そのまま保持し、N=0〜3（128/256/512/1024バイト）の可変レコードをFDCから扱います。
  N>=4や短縮データは診断・再保存の対象として保持しますが、FDC転送は行いません。
  ネイティブ IPLPRO ブート。セクタ転送はホストがデータレジスタを
  読み切るタイミングではなく、ディスク自身のスケジュールで完了します
- テキスト画面 40/80桁 + PCG（モノクロ/3面合成カラー）、GDE 320×200 16色
  （リングスクロール SAD0-2/SLN1/HDSC、ウィンドウマスク）、CLUT、
  MZ-1M10 RGB444 パレット + デジタルパレット、256色/640×400モード、
  第2テキストページ、背景色、標準/拡張GRAMのEX切替、表示窓とプレーンを
  指定する画面一括消去（BDh BUSYを含む）。拡張GRAM非装着時もCPU窓と
  描画側で同じ不在状態を返します
- 8253 ch0 + GATEパルス（F0h-F3h）+ 割込コントローラ（C6h/C7h、IM2）
- 8255 mode set/BSR、Port A/B/C方向制御、VGATE、BEEP
- YM2203（ymfm。BUSY、Timer A/B満了、CSM、SSG I/O方向をCPUサイクル基準で
  エミュレート）
- キーマトリクス、ジョイスティック（ゲームパッド対応）
- 漢字ROM窓・辞書ROM窓、RP5C15 RTC、Z80B SIO（2chのWR/RR、送受信FIFO、
  非同期フレーム時間、割込ベクタ、モデム線）、MZ-1X10 マウス。CDhで外部
  クロックを切り替え、ブラウザ側は仮想端末・時間付きループバック・Web Serial
  を選択可能
- ポートF6h グラフィックマスク（bit2/1/0 が緑/赤/青の出力をゲート。
  16色モードでは bit0 が I プレーンも遮断）。グラフィック層のみに作用
- MZ-1E35 ADPCM ボード（Y8950、ポート98h/99h。ADPCM RAM 32KBは暫定値 —
  実機の仕様書に記載がなく、EmuZとの黒箱比較でも確認できていません。
  IRQ線は実機同様配線なし＝ステータスポーリング。実ソフトの装着チェックは
  通過（SBディスクマガジン「星くずばこ」が装着ありと判定）。delta-T RAM
  再生は実装・単体テスト済みだが、実ソフトでの再生確認は未了 —
  確認方法を確立するまで「装着チェック通過」までが検証範囲です。
  RESET時のボードRAM保持も資料未確認の暫定互換方針です）。8KB〜256KBの
  外部メモリ構成、Y8950の4bit GPIO方向/入出力、AD入力を実装し、ブラウザの
  音声入力と未校正ミックスゲインを明示的に選択可能
- MZ-1R37 640KB EMM（ポートACh/ADh。SBディスクマガジン「星くずばこ」が
  認識して実動作することを確認済み）
- 内蔵CMTデッキ（PCM WAVと標準MZF論理テープ `.mzf` / `.mzt` / `.m12`、
  再生/録音/早送り/巻戻し、8255操作線、センサ、オートリワインド、
  MZ-80B固有のパルス幅と操作、実IPLのCMT選択による自動再生、ブラウザ内保存）。
  論理テープは未変更なら元形式を保持し、録音後はタイミングを失わないWAVとして保存
- セントロニクス系プリンタポート（FEh/FFh、STB/PRIM、BUSY/ACK、割込、
  生バイト列の表示・保存）
- MZ-1E30 SASI（A4h/A5hのバスフェーズとACK、Xebec S1410の6バイト
  コマンドセット、READ/WRITE、FORMAT、REQUEST SENSE、診断/READ LONG/
  WRITE LONG、A8h/A9h BIOS窓、raw HDFの保存）。後期SCSIのINQUIRY、
  MODE SELECT/SENSE、START/STOPはSASI機へ推測で追加せず、不正コマンドを返す
- MZ-2000/MZ-80B互換モード。背面起動セレクタ、Z80 4MHz、262走査線・
  60.99Hz、B7hメモリモード、E8h互換VRAM窓、F4h〜F7hのモード別機能、
  640×200カラー/320×200グリーン表示を実装。利用者所有のMZ-2500
  IPL/KANJI ROMが必要で、MZ-2000/MZ-80B本体のIPL/CG ROMは使用しません。
  ROM構成はEmuZ-2500のバイナリを外から比較して確認しました。MZ-80Bモードでは
  付属BASICカセット `SB-5510` のMZFイメージを実IPLが自動再生し、BASICまで
  起動することを確認済みです。MZ-2000のカセット起動は未確認です
- 同一インスタンスでの再起動。CPU/周辺デバイスのRESET対象と、RAM・VRAM・
  PCG・RTC・EMMなど保持する記憶を分離し、OPN/Y8950、G-CRTC、CRTC、
  PPI/PIO/SIO、8253、外側ラッチを共通RESETへ戻します。資料で確定できない
  RESET値には実機値と断定しない互換ベースラインを使い、実IPLはそこからROM自身に
  初期化させます。ただしCMT用8255は、実IPLがモード設定より先にSTOP/PLAY信号を
  出すため、通常配線の82hをIPL開始値とします。ダミーIPLだけがそのほかの
  実IPLトレース済み引渡し値を適用します

MZ-1E35 / MZ-1R37 / MZ-1E30 は「ハードウェア構成」パネルで取り外し可能です
（既定 ON）。F6h bit3 MGはモノクロ映像端子への重畳指定であり、現在のRGBA
カラー出力には対応する別端子がありません。CMTのアナログ音声トラック、
MZ-1E35基板固有のGPIO用途・入力ゲイン・フィルタ・OPNとの抵抗ミックス比、
TV/電話/音声合成オプション、SIOの同期式を実機コネクタまで再現する経路は、
未実装または一次資料/実測待ちです。プリンタは文字やESC/Pを解釈せず、SASIは
セクタイメージ単位であり電気的なバス競合までは扱いません。SASIコマンド範囲は
[Xebec S1410 Owner's Manual, Rev. C-1](https://bitsavers.org/pdf/xebec/Xebec_S1410/104524C_S1410Man_Aug83.pdf)
を一次資料にしています。詳細と実装追跡は
[`docs/research/super-mz-unimplemented-audit.md`](https://github.com/zabaglione/mz2500-web-emulator/blob/main/docs/research/super-mz-unimplemented-audit.md)
に記録しています。互換モードのROM構成は
[`docs/research/emuz-compat-rom-blackbox.md`](https://github.com/zabaglione/mz2500-web-emulator/blob/main/docs/research/emuz-compat-rom-blackbox.md)
に、EmuZ内部を参照しない比較条件と未確認境界を記録しています。

MZFは128バイトのヘッダーと、ヘッダー内で宣言されたサイズのデータを読み込みます。
MZT/M12は同じ内容の互換拡張子として扱います。`.cmt`は機種間で意味が異なるため
対象外です。RIFF/WAVEは拡張子にかかわらず内容で判定します。

CMTの3桁カウンターは、現在位置をテープ全長に対して`000`〜`999`へ換算する
簡易表示です。実機のリール径や巻き取り量に伴う回転数の非線形変化は再現して
いません。

## ビルド

`web_emulator/vendor/`（superzazu/z80 と ymfm のベンダリング元）は
`.gitignore` 対象のため、新規に checkout したワークツリーには存在しません。
ビルド前に vendor 一式が揃ったチェックアウトからコピーしてください。

```bash
tools/build_native.sh     # macOSネイティブCLI（ヘッドレス検証用）
tools/build_wasm.sh       # WASM + web/dist（要 emscripten）
tools/test_special_d88_boot.sh PATH_TO_D88 PATH_TO_ROM_DIR  # private canary boot check
python3 -m http.server 8425 --directory web/dist
```

`test_special_d88_boot.sh` は利用者所有の特殊D88と`ipl.rom`を引数に取る受入れ用
カナリアです。ROMやD88はリポジトリおよび配布物へ追加しません。

CLI はスクリーンショット（PPM）、メモリトレース、キー/ジョイパルス、WAV録音、
OPNレジスタトレース等を備えたヘッドレス回帰用フロントエンドです。

## 検証（クリーンルーム方針）

- 実装は標準チップのデータシート（MB8877 / i8253 / Z80 PIO / YM2203）と
  自作ゲーム開発で蓄積した機種ドキュメントに基づく新規実装。
- **EmuZ-2500（Common Source Code Project, GPL-2.0）は、内部実装を参照せず、
  バイナリ実行結果だけを比較するゴールドマスターとして使用**します。
  公開する実装・仕様判断にCSCPのソースコード、データ、生成物は使用しません。
- MZ-2000/MZ-80B互換モードの実装根拠はSHARPの取扱説明書とI/O資料で、
  EmuZ-2500とは画面・I/O・起動結果を外部から比較します。
- FDC タイミングは EmuZ との黒箱比較で校正し、ブート・ロードの
  マイルストーンがフレーム単位で一致。
- タイトル画面・ロード画面は EmuZ 出力と **100.00% ピクセル一致**。
- BGM は EmuZ 録音との比較でエンベロープ相関 0.99+、スペクトログラム類似
  0.96-0.99、主要音高中央値比 1.0000。
- `tools/audit_dist.py` が配布物に実 ROM 由来バイト列が無いことを機械確認。

## 操作

SPACE = 決定/ジャンプ、カーソル = 移動、Z = ダッシュ。
ゲームパッド: A = ジャンプ, X = ダッシュ。
マウス: 画面クリックで捕捉、Esc で解放。

## 追加機能

- **2ドライブ（FD1/FD2）**: ドライブ毎のホットスワップ・アクセスランプ・
  マルチボリュームD88の自動分割とボリューム切替（2枚組以上対応）
- **ブラウザ内保存**: 入れたディスク・登録したROM・ハードウェア構成は
  IndexedDB/localStorageに保存（外部送信なし）
- **ROMスロット**: 手持ちの ipl/kanji/dict.rom とSASI BIOSを登録可能。
  IPLPRO互換でない有効なD88は、ipl.rom登録時に実IPLへ自動選択される。
  ipl.rom登録時は実IPLブート（実験的）を手動選択することもできる。kanji=バンク39h窓と
  互換モード文字、dict=バンク3Ah窓、SASI BIOS=A8h/A9hに結線
- **互換起動セレクタ**: MZ-2500 / MZ-2000 / MZ-80Bを選択してRESET。
  互換モードではMZ-2500実IPL/KANJI ROMをブラウザ内ROMスロットから使用
- **ハードウェア構成**: 拡張RAM(256KB)・拡張GRAM(第2画面)・MZ-1M10(4096色)の
  有無をオプションとして設定可能（実機のボード構成を再現）
- **デバッグパネル**: CPU/バンクマップ/GDE/FDC/割込のライブ表示と
  メモリウォッチ（DEBUGボタンで表示切替）
- **ディスクへの書き込み**: BASIC-M25 の `save` / `load` / `kill` / `name` /
  `mkdir` / `chdir` / `rmdir` と、「フォーマット&コピー」ユーティリティによる
  物理フォーマット・論理フォーマット・ディスクコピーが実機と同等に動作
  （マニュアル記載どおり、ファイルが残ったディレクトリへの `rmdir` と
  ディレクトリへの `kill` は拒否）。「新規」で未フォーマットのブランク
  ディスク（688バイトヘッダ＋全ゼロのトラックテーブル。実機の
  フォーマットユーティリティがトラック配置を決めるまで、エミュレータは
  ディスクレイアウトを勝手に作らない）を作成でき、書き換えたディスクは
  マルチボリューム構成ごと IndexedDB に自動保存されてリロード後も残る。
  「保存」ボタンで現在のディスクを .d88 として取り出せ、「WP」は実機の
  ライトプロテクトタブに相当する
- **マウス（MZ-1X10）**: SIO チャンネルBに接続され、OPNポートAのbit3で
  RS-232Cと切り替わる。ドライバのDTRストローブに3バイトパケットで応答。
  画面クリックで捕捉、Escで解放。感度は左ペインで変更可能（実機側の
  移動比率設定はソフトウェア側の値のままで、エミュレータは変更しない）

## MCPサーバー（AIからの操作）

`mcp/` に、同じWASMコアをNode.jsでヘッドレスにホストするMCPサーバーを同梱。
AIエージェント（Claude Code等）がキー入力・画面テキスト読取・スクリーン
ショット・音状態の解読・録音・メモリ読み書き・ディスク入替/書き出しの
ツール群を通じてBASICプログラミングできる。AIの操作をブラウザで観戦する
ビュー（`http://127.0.0.1:8425/`）も内蔵。ROM/ディスクは利用者所有の
ファイルをパス指定（同梱なし）。**解説: [docs/mcp-server.md](docs/mcp-server.md)**、
ツール一覧: [mcp/README.md](mcp/README.md)。

## ライセンス

- 本体: MIT（`LICENSE`）
- vendoring: superzazu/z80（MIT・改変あり）、ymfm（BSD-3-Clause）—
  `THIRD_PARTY_LICENSES.md` に全文
- 同梱の NEKO CAN RUN デモ版 D88: (C) 2026 ZABAGLIONE PROJECT。
  自由に再配布せず、本リポジトリ/公開ページからの利用にとどめてください

謝辞: EmuZ-2500 / Common Source Code Project（武田俊也氏）、
claude-famicom-emu（GOROman氏）。MZ-2500のI/O仕様の確認では、
[紅茶羊羹さんのMZ-2500資料](http://www.maroon.dti.ne.jp/youkan/mz2500/index.html)を
参考にさせていただきました。
