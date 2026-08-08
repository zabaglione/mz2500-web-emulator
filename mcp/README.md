# mz2500-mcp — AIからMZ-2500を操作するMCPサーバー

Webエミュレータと同じWASMコアをNode.jsでヘッドレスにホストし、
[Model Context Protocol](https://modelcontextprotocol.io/) のツールとして
キー入力・画面・音・マシン内部を公開します。AIエージェント（Claude Code等）が
実機さながらにBASICプログラミングできます。

- **ツール駆動の時間モデル**: ツール呼び出しの間マシンは停止。同じ操作列は
  常に同じ結果になり、実時間よりずっと速く回ります（Okプロンプトまで約1秒）。
- **ROM・BASICディスクは同梱しません**。お手持ちのファイルをパスで指定します。
  ROM無しでも、ダミーIPLブート（IPLPRO形式のゲームディスク）は動きます。

## セットアップ

```bash
# 1. WASMコアをビルド（要 emscripten）
web_emulator/tools/build_wasm.sh

# 2. サーバーをビルド
cd web_emulator/mcp
npm install
npm run build
```

## 起動設定

すべてCLIフラグまたは環境変数（括弧内）で指定:

| フラグ | 意味 |
| --- | --- |
| `--rom-dir DIR` (`MZ2500_ROM_DIR`) | `ipl.rom`/`cg.rom`/`kanji.rom`/`dict.rom` を探すディレクトリ |
| `--ipl-rom` 等 (`MZ2500_IPL_ROM` 等) | ROMを個別指定（`--rom-dir` より優先） |
| `--disk-a PATH` (`MZ2500_DISK_A`) | ドライブ0（FD1）のD88。BASICブートディスク等 |
| `--disk-b PATH\|blank` (`MZ2500_DISK_B`) | ドライブ1（FD2）。既定は未フォーマットのブランク |
| `--workdir DIR` (`MZ2500_WORKDIR`) | スクショPNG・録音WAV・エクスポートD88の保存先 |
| `--no-auto-boot` | 接続時の自動ブートをしない |
| `--boot-wait TEXT` (`MZ2500_BOOT_WAIT`) | ブート後にこの文字列を待つ（実IPL時の既定は `Ok`） |
| `--spectate-port N` (`MZ2500_SPECTATE_PORT`) | 観戦ビューのHTTPポート（既定8425、`0`で無効） |

IPL ROMとBASICディスクを与えると、起動時に**Okプロンプトで対話可能な状態**
（オートラン完了・キー入力受付を確認済み）まで自動で進みます。

### Claude Code への登録例（`.mcp.json`）

```json
{
  "mcpServers": {
    "mz2500": {
      "command": "node",
      "args": [
        "/path/to/web_emulator/mcp/dist/index.js",
        "--rom-dir", "/path/to/roms/mz2500",
        "--disk-a", "/path/to/basic-m25.d88",
        "--workdir", "/path/to/mz2500-work"
      ]
    }
  }
}
```

## ツール一覧

| ツール | 役割 |
| --- | --- |
| `type_text` | 文字列を打鍵（ASCII+改行、SHIFT自動）。打鍵後の画面を返す |
| `press_key` | 名前指定キー（cr/esc/break/f1-f10/カーソル等、shift/ctrl修飾・長押し可） |
| `joy` | ジョイスティックをNフレーム保持 |
| `run_frames` | Nフレーム時間を進める（約55.5フレーム/秒） |
| `read_screen` | テキストVRAMをデコードして文字列で返す（漢字=〓、PCGアート=#） |
| `wait_for_text` | 指定文字列が画面に出るまで実行（RUN完了待ちに） |
| `screenshot` | 640×400 PNG（画像として返却+workdirに保存） |
| `read_sound_state` | YM2203解読: FM/SSG各chの周波数・音名・キーオン・音量、BEEP |
| `record_audio` | Nフレーム録音してWAV保存、peak/RMS付き |
| `read_memory` | CPUアドレス空間（バンク経由）or 物理512KBのhexダンプ |
| `write_memory` | CPUアドレス空間へpoke |
| `get_machine_state` | CPUレジスタ・バンクマップ・FDC・表示モード等のJSON |
| `insert_disk` | D88ホットスワップ or ブランク挿入 |
| `export_disk` | ドライブ内容をD88としてworkdirへ書き出し |
| `reset` | 再コールドブート（実IPL/ダミーIPL、テキスト待ち付き） |

## 観戦ビュー

サーバー起動中、`http://127.0.0.1:8425/`（`--spectate-port`で変更可）をブラウザで
開くと、AIの操作を人間が観戦できます。画面・音・YM2203の発音状態が
フレーム番号で同期して届き、**ブラウザ側がバッファして実機速度（約55.5fps）で
等速再生**します。マシンはツール駆動のまま全速で進むため、表示は実行より
遅れて追いかけ、追いつくと「AIの操作待ち」になります。

- 観戦者が誰もいない間はフレーム生成ごと停止し、オーバーヘッドはありません
- 配信は127.0.0.1バインド+Origin検証。リモート公開は想定していません
- 長い`run_frames`中は配信がツール完了までバッファされます（等速再生で吸収）

## 典型的な流れ

```
type_text  {"text": "10 for i=1 to 3\n20 print \"HELLO\";i\n30 next\nrun\n"}
wait_for_text {"text": "HELLO 3"}
screenshot {}
```

BASICプログラムの保存は、フォーマット済みディスクをドライブ1に入れて
`save "FD2:NAME"` → `export_disk {"drive": 1}`。次回起動時にそのD88を
`--disk-b` で渡せば続きから作業できます。

## テスト

```bash
MZ2500_ROM_DIR=~/roms/mz2500 MZ2500_DISK_A=~/disks/basic-m25.d88 npm test
```

ROM/ディスクが無い環境ではBASIC系テストはスキップされ、同梱NEKOデモの
ROM無しスモークテストだけが走ります。

## 制限・今後

- 打鍵はASCII+特殊キーのみ（かな・漢字入力は未対応）
- 画面テキストの漢字セルは〓に置き換え（ANK/半角カナは正しく復元）
