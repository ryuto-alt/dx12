# UI 書体（Graphite Gauge）

エディタ UI 用の書体のサブセット。IBM Plex と Noto Sans JP と PlemolJP は **SIL OFL 1.1**。PlemolJP は Plex に無い記号（矢印・数学記号・図形など）を Hack（元は Bitstream Vera Sans Mono。Bitstream Vera License は許可ライセンスの外）から借りているため、**PlemolJP 由来の 2 出力は IBM Plex Mono / IBM Plex Sans JP が持つコードポイントだけに絞ってある**（Hack 由来の字形は入らない。`--check` が検査）。絞られた記号はシステム書体 / Plex 系の書体へ縮退する。ライセンス文は `third_party/licenses/`。
生成物だけをリポジトリに入れる（元ファイルは 1 つ 5〜6MB あるので入れない。URL と SHA256 は `fonts.lock.json`）。

| 書体 | 元 | 出力 |
|---|---|---|
| Uno UI Sans JP（Regular / Medium / SemiBold） | IBM Plex Sans JP 3.0.0 | `ibm-plex/subset/UnoUISansJP-*.ttf` |
| Uno UI Sans（Regular / Medium / SemiBold） | IBM Plex Sans 1.3.6 | `ibm-plex/subset/UnoUISans-*.ttf` |
| Uno UI Mono（Light / Regular / Medium。Light は GG の数値表示 300） | IBM Plex Mono 2.5.0 | `ibm-plex/subset/UnoUIMono-*.ttf` |
| Uno UI Mono JP Regular（ログ・パス用の等幅、和欧 1:2） | PlemolJP 3.1.0（Plex の字だけ） | `plemoljp/subset/UnoUIMonoJP-Regular.ttf` |
| Noto Sans JP Regular（足りない字の予備。名前は "Noto Sans JP" Regular / 400 に正規化） | `assets/editor/fonts/NotoSansJP-Regular.ttf` | `noto-sans-jp/subset/NotoSansJP-Regular.ttf` |
| 起動画面専用 Uno Splash Mono Light（等幅、約 100KB。％の数字は GG の mono 300 に合わせた Light） | PlemolJP 3.1.0 Light（Plex の字だけ） | `assets/editor/splash/fonts/splash_subset.ttf` |

- **名前の付け替え**: IBM Plex は Reserved Font Name "Plex"、PlemolJP は "PlemolJP"（Vera 系の "Bitstream" / "Vera"）を宣言しているため、
  改変版（サブセット）の名前表は「Uno UI …」「Uno Splash Mono」に付け替えてある（著作権・ライセンスの欄は元のまま）。
  GG 仕様の書体名（IBM Plex Sans JP など）は、エンジン内ではこのファイルを指す論理名。Noto の RFN は "Source" だけなので
  名前は "Noto Sans JP" Regular に正規化するだけ。`--check` と `font_subset_test` が、全出力の nameID 1/4/6/16/21 に予約名が無いことを検査する。
- **字の集合**: JIS X 0208 全体 + ASCII + Latin-1 + 記号・矢印・罫線・全角形 + `src/**` の文字列リテラルから抜き出した全 UI 文字
  （Tips、StageLabel、ReleaseNotes を含む）+ `third_party/fonts/extra_chars.txt`（`[ui]` 節: 番号付きタブの表題・図番 UN-x.y.z・GG トークンの記号。`[splash]` 節: 起動画面の仕様の字）。
  結果は `charset_ui.txt`（起動画面は `charset_splash.txt`）。`--check` は charset_ui.txt との一致は要求せず、
  「必要な字がコミット済みのどれかの書体の cmap にあるか（無ければ許可リスト）」だけを見る。
- **起動画面の字の拾い方**: `src/**/Splash*` の文字列リテラルの非 ASCII 字は、すべて表示文言として数える（診断ログ等の日本語をそのファイルに書かない。書くなら別ファイルへ）。起動画面の文言は `[splash]` 節にも置ける。`Splash*` 以外のファイルに置く起動画面の文言は `[splash]` 節へ足すこと。
- **ヒンティングは外してある**（描画は ImGui の FreeType LightHinting）。
- **サブセットに入れない字**（アイコン・絵文字）は `charset_system_fallback.txt`。

## コマンド（`python -I`。fonttools は MIT。venv へ `pip install fonttools` で入れる。リポジトリには入れない）

```
python -I tools/subset_fonts.py --check                 # 標準ライブラリだけ。charset・SHA256・上限（ctest の FontSubsetCheck）
python -I tools/subset_fonts.py --fetch --src <dir>     # 元ファイルを取得（約 550MB を一時的に取る）して SHA256 を検証
python -I tools/subset_fonts.py --build --src <dir>     # サブセットを作り直す（fonts.lock.json の出力欄と charset も更新）
python -I tools/subset_fonts.py --check --src <dir>     # 元ファイルから作り直してバイト一致まで確かめる（fonttools が要る）
```

新しい UI 文字列に、どの書体にも無い字（JIS X 0208 の外など）を使って `FontSubsetCheck` が赤くなったら、
`extra_chars.txt` へ足して `--build` で作り直す（アイコン・絵文字は許可リストへ）。
