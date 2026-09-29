# エディタのアイデンティティ候補（テーマ・バリアント）

Uno Engine のエディタに「UE5 のコピーではない独自の見た目」を与えるための候補 3 案を、
トークン表 + 少数の描画装飾で切り替えられるようにした開発用の仕組み。案が決まったら 1 案だけ残して他を捨てる前提。

| 案 | 名前 | 要点 |
|---|---|---|
| Default | 現行 | 第 1〜3 波の見た目そのまま（指定なしなら 1px も変わらない） |
| A | ネオン・エッジ | 藍寄りの暗い面。フォーカス / 選択 / アクティブだけ細いネオンのエッジライト |
| B | グラス・レイヤー | 階調を強めた半透明ガラス。淡い光の縁・大きな角丸・浮遊するポップアップ |
| C | インク・アンド・シグナル | ほぼ無彩色。シグナルのライム 1 色だけを状態表示に使う |

比較と評価は `dx12-ui-audit/identity/IDENTITY_OPTIONS.md`（`_identity_compare.png`）。

## 切り替え

- 起動引数: `DX12Engine.exe --theme-variant a|b|c|default`（指定なし = 現行）。
- 実行中: `--theme-variant` を付けて起動した時だけ、コマンドパレット(Ctrl+K)に「テーマ案: …」が出る。
  内部は `theme::RequestVariant(v)` → `ImGuiManager::BeginFrame` が NewFrame の前に基準スタイルを作り直す（倍率も維持）。

## 構造

- `src/editor/ThemeVariants.h` … 純データ。`Spec { Palette pal; Metrics m; Deco deco; }` を案ごとに返す（`SpecOf(Variant)`）。
  - `Palette` … 配色一式（面 Bg0〜Bg4・入力欄・枠・アクセント・文字・状態色・種別色・ImGuiStyle に流す細かい色）。
  - `Metrics` … 角丸・線幅・分割の太さ。**行高・余白は全案で共通**（レイアウトは案で動かない）。
  - `Deco` … グロー量・イージング時間・行 / 見出しのスタイル・影・光の縁・背後のグラデ・ステータス / ツールバーの線。
- `src/editor/EditorTheme.h` … トークン（`theme::Bg0` 等）は可変。`SetVariant` が書き換え、`ApplyStyle` が ImGuiStyle へ流す。
- `src/editor/UiWidgets.{h,cpp}` の `ui::deco::` … 共通の装飾ヘルパ。
  `Ease`（状態の滑らかな遷移）/ `Glow`（外側へ広がるハロー）/ `RowFace`（行の面）/ `CardSelected` / `SelectionBar` /
  `FloatingCard`（トースト等）/ `PaintChrome`（EndFrame で窓の外側から足す: パネルのエッジ・上端ヘアライン・選択タブの光・ポップアップの影）。
  **どの関数も `Variant::Default` では従来と同じ描画・同じ値**。呼び出し側（各パネル）は案を見ない。
- `Gui` ライブラリは `Editor` に依存できないので、`PaintChrome` は `theme::g_paintChromeFn`（`EditorLayer` が設定）経由で呼ぶ。

## 守ること

- 色・寸法は `EditorTheme.h` のトークンと `ui::Px()` だけ。案ごとの値は `ThemeVariants.h` に足す（16 進を各パネルに書かない）。
- 新しい装飾は `deco::` に足し、`Deco::Active()`（現行以外）の分岐に閉じ込める。現行側の描画は変えない。
- ImGui のドック窓には `ChildWindow` フラグが付く。窓外装飾で「子窓を除く」時は `DockIsActive` を見ること。
- 検証: `tests/theme_variant_test.cpp`（現行値の固定・全案の文字コントラスト AA・レイアウト値の不変・往復）/ `--ui-tests-run-all` を各案で。

## 案を決めたら

1. 採用案の `Palette` / `Metrics` / `Deco` を `MakeDefault()` へ移す。
2. 他案の表・`view.theme.*` コマンド（`EditorCommandTable.h` / `EditorCommands.cpp`）・`--theme-variant` 解析・`deco::` の現行分岐を削除。
3. `ThemeVariantTests` を採用案の値の固定テストへ書き換える。
