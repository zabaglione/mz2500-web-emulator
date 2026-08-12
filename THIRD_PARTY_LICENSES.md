# Third-party licenses

このディレクトリの自作コードは MIT License（`LICENSE` 参照）。以下のサードパーティコードを vendoring しています。

## superzazu/z80 (MIT License)

- Path: `vendor/z80/`
- Source: https://github.com/superzazu/z80
- Commit: `d64fe10a2274e5e40019b1086bf7d8990cbc5f23`
- **ローカル改変あり**: `port_in`/`port_out` コールバックの引数を 8bit → 16bit I/O
  アドレスに変更（`(C)` 形式は BC、`(n)` 形式は A<<8|n、OUTI/OTIR は B デクリメント後の
  値を上位に載せる）。MZ-1M10 パレット（ポート AEh、A8-A15 デコード）に必要。
  改変箇所には `local modification` コメントを付記。

License text: `vendor/z80/LICENSE`

```
MIT License

Copyright (c) 2019 Nicolas Allemand

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## ymfm (BSD-3-Clause License)

- Path: `vendor/ymfm/`
- Source: https://github.com/aaronsgiles/ymfm
- Commit: `81aec25ccbb98f4873a255f7551ac4dadac59b4a`
- 使用ファイル: `ymfm.h`, `ymfm_fm.h/.ipp`, `ymfm_opn.h/.cpp`, `ymfm_ssg.h/.cpp`,
  `ymfm_adpcm.h/.cpp`, `ymfm_opl.h/.cpp`, `ymfm_pcm.h/.cpp`
  （YM2203 = `ymfm::ym2203`、Y8950 = `ymfm::y8950` とそのビルド依存のみ）。改変なし。

License text: `vendor/ymfm/LICENSE`

```
BSD 3-Clause License

Copyright (c) 2021, Aaron Giles
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## 参照実装への謝辞（コード非取込）

- **EmuZ-2500 / Common Source Code Project**（武田俊也氏、GPL-2.0）:
  内部実装を参照せず、バイナリ実行結果だけを比較するゴールドマスターとして使用。
  **CSCPのソースコード・データ・生成物は本プロジェクトの実装や仕様判断に
  使用せず、本プロジェクトにも含みません**。
- **claude-famicom-emu**（GOROman氏、MIT）: C++ → Emscripten/WASM + JS という
  構成コンセプトの参照。コードは使用していません。

## ROM について

SHARP MZ-2500 の本体 ROM（IPL / 漢字 / 辞書 / BASIC）は**一切使用・同梱していません**。
ブート処理はホスト側ネイティブコードによる代替実装（`core/dummy_ipl.cpp`）です。
