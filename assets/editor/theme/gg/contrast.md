# Graphite Gauge v3 — コントラスト検査（自動生成）

`build.py` が `tokens_def.py` から計算した。式は WCAG 2.x の相対輝度（EditorTheme.h の `ContrastRatio` と同じ閾値 0.03928）。
alpha 付きの色は、その下の面に合成してから計算する（`A ⊕ B` = A を B の上に合成）。

- 文字（text）: 4.5:1 以上（すべて本文サイズとして扱う。大きい文字の 3:1 緩和は使わない）
- UI 境界（ui）: 3:1 以上（WCAG 1.4.11。入力欄の記入線・操作部品の外形・状態アイコン・黄の線）
- 参考 / 装飾 / 除外: 合否を問わない値（理由を併記）

**結果: 文字 112 組・UI 境界 99 組を検査し、不合格 0 組**（禁止の組み合わせ 1 組は実装で自動昇格させる）。

## 1. 文字 4 段 × 面（マトリクス）

| 文字 \ 面 | bg0 | bg1 | bg2 | bg3 | bg4 | popup | input | inputHover | inputActive | sel | selHover | selActive |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| t1 `#E6E2DA` | 14.39 | 13.45 | 12.44 | 11.25 | 9.89 | 12.15 | 14.14 | 13.75 | 14.62 | 9.93 | 9.36 | 8.66 |
| t2 `#CFCAC1` | 11.39 | 10.65 | 9.85 | 8.91 | 7.83 | 9.62 | 11.20 | 10.89 | 11.58 | 7.87 | 7.41 | 6.86 |
| t3 `#B4AFA6` | 8.52 | 7.96 | 7.37 | 6.66 | 5.86 | 7.20 | 8.37 | 8.14 | 8.66 | 5.88 | 5.54 | 5.13 |
| t4 `#A39E94` | 6.97 | 6.52 | 6.03 | 5.45 | 4.79 | 5.89 | 6.85 | 6.66 | 7.09 | 4.81 | 4.54 | ~~4.20~~ 禁止 |

面の値: bg0 `#131312` / bg1 `#1B1A19` / bg2 `#222120` / bg3 `#2A2927` / bg4 `#34322F` / popup `#242321` / input `#151514` / inputHover `#181817` / inputActive `#111110` / sel `#33322E` / selHover `#373632` / selActive `#3D3B36`

禁止の組み合わせ: `t4 on selActive`（4.20）。v2 では `t4` は bg4 / selection / selActive の 3 面で禁止だったが、v3 は t3 / t4 を 1 段明るくして selActive（押下の瞬間）だけにした。

## 2. 文字（4.5:1）

| 前景 | 背景 | 実色（前景 / 背景） | 比 | 基準 | 判定 | 使う場所 |
|---|---|---|---:|---:|---|---|
| status.goodText | surface.bg0 | `#58BFB4` / `#131312` | 8.43 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | surface.bg1 | `#58BFB4` / `#1B1A19` | 7.88 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | surface.bg2 | `#58BFB4` / `#222120` | 7.29 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | surface.bg3 | `#58BFB4` / `#2A2927` | 6.59 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | surface.bg4 | `#58BFB4` / `#34322F` | 5.80 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | surface.popup | `#58BFB4` / `#242321` | 7.12 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | selection.sel | `#58BFB4` / `#33322E` | 5.82 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | status.warnBg ⊕ surface.bg1 | `#58BFB4` / `#26201E` | 7.30 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.goodText | status.badBg ⊕ surface.bg1 | `#58BFB4` / `#271D1F` | 7.40 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | surface.bg0 | `#F18B73` / `#131312` | 7.68 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | surface.bg1 | `#F18B73` / `#1B1A19` | 7.18 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | surface.bg2 | `#F18B73` / `#222120` | 6.64 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | surface.bg3 | `#F18B73` / `#2A2927` | 6.01 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | surface.bg4 | `#F18B73` / `#34322F` | 5.28 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | surface.popup | `#F18B73` / `#242321` | 6.49 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | selection.sel | `#F18B73` / `#33322E` | 5.30 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | status.warnBg ⊕ surface.bg1 | `#F18B73` / `#26201E` | 6.65 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.warnText | status.badBg ⊕ surface.bg1 | `#F18B73` / `#271D1F` | 6.74 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | surface.bg0 | `#FB63B0` / `#131312` | 6.65 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | surface.bg1 | `#FB63B0` / `#1B1A19` | 6.22 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | surface.bg2 | `#FB63B0` / `#222120` | 5.75 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | surface.bg3 | `#FB63B0` / `#2A2927` | 5.20 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | surface.bg4 | `#FB63B0` / `#34322F` | 4.57 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | surface.popup | `#FB63B0` / `#242321` | 5.62 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | selection.sel | `#FB63B0` / `#33322E` | 4.59 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | status.warnBg ⊕ surface.bg1 | `#FB63B0` / `#26201E` | 5.76 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.badText | status.badBg ⊕ surface.bg1 | `#FB63B0` / `#271D1F` | 5.84 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | surface.bg0 | `#8FB0D6` / `#131312` | 8.28 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | surface.bg1 | `#8FB0D6` / `#1B1A19` | 7.74 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | surface.bg2 | `#8FB0D6` / `#222120` | 7.16 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | surface.bg3 | `#8FB0D6` / `#2A2927` | 6.47 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | surface.bg4 | `#8FB0D6` / `#34322F` | 5.69 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | surface.popup | `#8FB0D6` / `#242321` | 6.99 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | selection.sel | `#8FB0D6` / `#33322E` | 5.71 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | status.warnBg ⊕ surface.bg1 | `#8FB0D6` / `#26201E` | 7.17 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| status.infoText | status.badBg ⊕ surface.bg1 | `#8FB0D6` / `#271D1F` | 7.26 | 4.5 | OK | 状態の文字（ログ・トースト・件数） |
| text.link | surface.bg1 | `#A9C4F0` / `#1B1A19` | 9.80 | 4.5 | OK | リンク |
| text.link | surface.bg2 | `#A9C4F0` / `#222120` | 9.06 | 4.5 | OK | リンク |
| text.link | surface.popup | `#A9C4F0` / `#242321` | 8.85 | 4.5 | OK | リンク |
| kind.prefabText | surface.bg1 | `#A9C4F0` / `#1B1A19` | 9.80 | 4.5 | OK | プレハブ名 |
| kind.prefabText | selection.sel | `#A9C4F0` / `#33322E` | 7.24 | 4.5 | OK | プレハブ名 |
| kind.prefabText | selection.selHover | `#A9C4F0` / `#373632` | 6.82 | 4.5 | OK | プレハブ名 |
| kind.prefabText | selection.selActive | `#A9C4F0` / `#3D3B36` | 6.31 | 4.5 | OK | プレハブ名 |
| axis.x | surface.input | `#CF6872` / `#151514` | 5.11 | 4.5 | OK | 数値欄の軸文字 |
| axis.x | surface.inputActive | `#CF6872` / `#111110` | 5.29 | 4.5 | OK | 数値欄の軸文字 |
| axis.x | surface.bg1 | `#CF6872` / `#1B1A19` | 4.86 | 4.5 | OK | 数値欄の軸文字 |
| axis.y | surface.input | `#A9DD83` / `#151514` | 11.62 | 4.5 | OK | 数値欄の軸文字 |
| axis.y | surface.inputActive | `#A9DD83` / `#111110` | 12.02 | 4.5 | OK | 数値欄の軸文字 |
| axis.y | surface.bg1 | `#A9DD83` / `#1B1A19` | 11.05 | 4.5 | OK | 数値欄の軸文字 |
| axis.z | surface.input | `#6E98DA` / `#151514` | 6.24 | 4.5 | OK | 数値欄の軸文字 |
| axis.z | surface.inputActive | `#6E98DA` / `#111110` | 6.45 | 4.5 | OK | 数値欄の軸文字 |
| axis.z | surface.bg1 | `#6E98DA` / `#1B1A19` | 5.93 | 4.5 | OK | 数値欄の軸文字 |
| accent.onAccent | accent.accent | `#1C1705` / `#F0C23B` | 10.63 | 4.5 | OK | 黄の面の上の文字（再生ボタン / 再生タグ） |
| accent.onAccent | accent.accentHover | `#1C1705` / `#F6D265` | 12.20 | 4.5 | OK | 黄の面の上の文字（再生ボタン / 再生タグ） |
| accent.onAccent | accent.accentPressed | `#1C1705` / `#D4A82A` | 8.03 | 4.5 | OK | 黄の面の上の文字（再生ボタン / 再生タグ） |
| play.tagPausedText | play.tagPausedBg | `#E6E2DA` / `#131312` | 14.39 | 4.5 | OK | 一時停止タグ |
| viewport.dimLabelText | viewport.dimLabelBg ⊕ viewport.bgKey | `#E6E2DA` / `#21211F` | 12.49 | 4.5 | OK | 寸法 / 軸の札（3D の上） |
| viewport.dimLabelText | viewport.dimLabelBg ⊕ viewport.bgMid | `#E6E2DA` / `#1C1C1A` | 13.25 | 4.5 | OK | 寸法 / 軸の札（3D の上） |
| viewport.dimLabelText | viewport.dimLabelBg ⊕ viewport.bgLow | `#E6E2DA` / `#181816` | 13.80 | 4.5 | OK | 寸法 / 軸の札（3D の上） |
| viewport.dimLabelText | viewport.dimLabelBg ⊕ viewport.bgEdge | `#E6E2DA` / `#151514` | 14.12 | 4.5 | OK | 寸法 / 軸の札（3D の上） |
| viewport.dimLabelText | viewport.dimLabelBg ⊕ viewport.floorNear | `#E6E2DA` / `#1B1B19` | 13.39 | 4.5 | OK | 寸法 / 軸の札（3D の上） |
| viewport.dimLabelText | viewport.dimLabelBg ⊕ viewport.floorFar | `#E6E2DA` / `#141413` | 14.29 | 4.5 | OK | 寸法 / 軸の札（3D の上） |
| timeline.rulerLabel | timeline.rulerBg | `#B4AFA6` / `#131312` | 8.52 | 4.5 | OK | ルーラーの数字 |
| text.t1 | overlay.scrimPalette ⊕ surface.popup | `#E6E2DA` / `#181716` | 13.86 | 4.5 | OK | 暗幕の下に残る文字（参考） |

## 3. UI 境界（3:1）

| 前景 | 背景 | 実色（前景 / 背景） | 比 | 基準 | 判定 | 使う場所 |
|---|---|---|---:|---:|---|---|
| surface.inLine | surface.input | `#716D65` / `#151514` | 3.55 | 3.0 | OK | 入力欄の記入線 |
| surface.inLine | surface.bg1 | `#716D65` / `#1B1A19` | 3.37 | 3.0 | OK | 入力欄の記入線 |
| surface.inLine | surface.bg2 | `#716D65` / `#222120` | 3.12 | 3.0 | OK | 入力欄の記入線 |
| surface.inLineHover | surface.input | `#8A857C` / `#151514` | 4.98 | 3.0 | OK | 記入線（ホバー） |
| accent.accent | surface.input | `#F0C23B` / `#151514` | 10.86 | 3.0 | OK | 記入線（フォーカス、黄 #05） |
| surface.controlEdge | surface.bg1 | `#77736A` / `#1B1A19` | 3.68 | 3.0 | OK | チェック / トグル OFF の外形 |
| surface.controlEdge | surface.bg2 | `#77736A` / `#222120` | 3.40 | 3.0 | OK | チェック / トグル OFF の外形 |
| surface.controlEdge | surface.bg3 | `#77736A` / `#2A2927` | 3.08 | 3.0 | OK | チェック / トグル OFF の外形 |
| surface.controlEdge | surface.popup | `#77736A` / `#242321` | 3.32 | 3.0 | OK | チェック / トグル OFF の外形 |
| surface.primaryBtnEdge | surface.bg1 | `#8A857C` / `#1B1A19` | 4.74 | 3.0 | OK | 主要ボタンの枠 |
| surface.primaryBtnEdge | surface.popup | `#8A857C` / `#242321` | 4.28 | 3.0 | OK | 主要ボタンの枠 |
| accent.accent | surface.bg0 | `#F0C23B` / `#131312` | 11.05 | 3.0 | OK | 黄の線（#01〜#08） |
| accent.accentDim | surface.bg0 | `#9A8240` / `#131312` | 4.99 | 3.0 | OK | フォーカス外の黄の線 |
| accent.accent | surface.bg1 | `#F0C23B` / `#1B1A19` | 10.33 | 3.0 | OK | 黄の線（#01〜#08） |
| accent.accentDim | surface.bg1 | `#9A8240` / `#1B1A19` | 4.67 | 3.0 | OK | フォーカス外の黄の線 |
| accent.accent | surface.bg2 | `#F0C23B` / `#222120` | 9.56 | 3.0 | OK | 黄の線（#01〜#08） |
| accent.accentDim | surface.bg2 | `#9A8240` / `#222120` | 4.32 | 3.0 | OK | フォーカス外の黄の線 |
| accent.accent | selection.sel | `#F0C23B` / `#33322E` | 7.63 | 3.0 | OK | 黄の線（#01〜#08） |
| accent.accentDim | selection.sel | `#9A8240` / `#33322E` | 3.45 | 3.0 | OK | フォーカス外の黄の線 |
| selection.barSecondary | selection.sel | `#8A857C` / `#33322E` | 3.50 | 3.0 | OK | 複数選択の従の左バー |
| status.good | surface.bg0 | `#58BFB4` / `#131312` | 8.43 | 3.0 | OK | 状態アイコン / 左バー |
| status.good | surface.bg1 | `#58BFB4` / `#1B1A19` | 7.88 | 3.0 | OK | 状態アイコン / 左バー |
| status.good | surface.bg2 | `#58BFB4` / `#222120` | 7.29 | 3.0 | OK | 状態アイコン / 左バー |
| status.good | surface.bg3 | `#58BFB4` / `#2A2927` | 6.59 | 3.0 | OK | 状態アイコン / 左バー |
| status.good | surface.bg4 | `#58BFB4` / `#34322F` | 5.80 | 3.0 | OK | 状態アイコン / 左バー |
| status.good | surface.popup | `#58BFB4` / `#242321` | 7.12 | 3.0 | OK | 状態アイコン / 左バー |
| status.warn | surface.bg0 | `#F18B73` / `#131312` | 7.68 | 3.0 | OK | 状態アイコン / 左バー |
| status.warn | surface.bg1 | `#F18B73` / `#1B1A19` | 7.18 | 3.0 | OK | 状態アイコン / 左バー |
| status.warn | surface.bg2 | `#F18B73` / `#222120` | 6.64 | 3.0 | OK | 状態アイコン / 左バー |
| status.warn | surface.bg3 | `#F18B73` / `#2A2927` | 6.01 | 3.0 | OK | 状態アイコン / 左バー |
| status.warn | surface.bg4 | `#F18B73` / `#34322F` | 5.28 | 3.0 | OK | 状態アイコン / 左バー |
| status.warn | surface.popup | `#F18B73` / `#242321` | 6.49 | 3.0 | OK | 状態アイコン / 左バー |
| status.bad | surface.bg0 | `#EC5480` / `#131312` | 5.44 | 3.0 | OK | 状態アイコン / 左バー |
| status.bad | surface.bg1 | `#EC5480` / `#1B1A19` | 5.09 | 3.0 | OK | 状態アイコン / 左バー |
| status.bad | surface.bg2 | `#EC5480` / `#222120` | 4.70 | 3.0 | OK | 状態アイコン / 左バー |
| status.bad | surface.bg3 | `#EC5480` / `#2A2927` | 4.25 | 3.0 | OK | 状態アイコン / 左バー |
| status.bad | surface.bg4 | `#EC5480` / `#34322F` | 3.74 | 3.0 | OK | 状態アイコン / 左バー |
| status.bad | surface.popup | `#EC5480` / `#242321` | 4.60 | 3.0 | OK | 状態アイコン / 左バー |
| status.info | surface.bg0 | `#8FB0D6` / `#131312` | 8.28 | 3.0 | OK | 状態アイコン / 左バー |
| status.info | surface.bg1 | `#8FB0D6` / `#1B1A19` | 7.74 | 3.0 | OK | 状態アイコン / 左バー |
| status.info | surface.bg2 | `#8FB0D6` / `#222120` | 7.16 | 3.0 | OK | 状態アイコン / 左バー |
| status.info | surface.bg3 | `#8FB0D6` / `#2A2927` | 6.47 | 3.0 | OK | 状態アイコン / 左バー |
| status.info | surface.bg4 | `#8FB0D6` / `#34322F` | 5.69 | 3.0 | OK | 状態アイコン / 左バー |
| status.info | surface.popup | `#8FB0D6` / `#242321` | 6.99 | 3.0 | OK | 状態アイコン / 左バー |
| axis.x | surface.input | `#CF6872` / `#151514` | 5.11 | 3.0 | OK | 軸の左バー |
| axis.x | surface.bg1 | `#CF6872` / `#1B1A19` | 4.86 | 3.0 | OK | 軸の左バー |
| axis.y | surface.input | `#A9DD83` / `#151514` | 11.62 | 3.0 | OK | 軸の左バー |
| axis.y | surface.bg1 | `#A9DD83` / `#1B1A19` | 11.05 | 3.0 | OK | 軸の左バー |
| axis.z | surface.input | `#6E98DA` / `#151514` | 6.24 | 3.0 | OK | 軸の左バー |
| axis.z | surface.bg1 | `#6E98DA` / `#1B1A19` | 5.93 | 3.0 | OK | 軸の左バー |
| kind.mesh | surface.bg1 | `#B9B3A8` / `#1B1A19` | 8.34 | 3.0 | OK | 種別アイコン |
| kind.mesh | selection.sel | `#B9B3A8` / `#33322E` | 6.16 | 3.0 | OK | 種別アイコン |
| kind.light | surface.bg1 | `#E8E0CC` / `#1B1A19` | 13.21 | 3.0 | OK | 種別アイコン |
| kind.light | selection.sel | `#E8E0CC` / `#33322E` | 9.76 | 3.0 | OK | 種別アイコン |
| kind.camera | surface.bg1 | `#A6BFD2` / `#1B1A19` | 9.11 | 3.0 | OK | 種別アイコン |
| kind.camera | selection.sel | `#A6BFD2` / `#33322E` | 6.72 | 3.0 | OK | 種別アイコン |
| kind.audio | surface.bg1 | `#9DB9C4` / `#1B1A19` | 8.42 | 3.0 | OK | 種別アイコン |
| kind.audio | selection.sel | `#9DB9C4` / `#33322E` | 6.21 | 3.0 | OK | 種別アイコン |
| kind.script | surface.bg1 | `#B6A1E0` / `#1B1A19` | 7.58 | 3.0 | OK | 種別アイコン |
| kind.script | selection.sel | `#B6A1E0` / `#33322E` | 5.60 | 3.0 | OK | 種別アイコン |
| kind.physics | surface.bg1 | `#9CC27A` / `#1B1A19` | 8.62 | 3.0 | OK | 種別アイコン |
| kind.physics | selection.sel | `#9CC27A` / `#33322E` | 6.36 | 3.0 | OK | 種別アイコン |
| kind.ui | surface.bg1 | `#DE8FB5` / `#1B1A19` | 7.20 | 3.0 | OK | 種別アイコン |
| kind.ui | selection.sel | `#DE8FB5` / `#33322E` | 5.32 | 3.0 | OK | 種別アイコン |
| kind.prefab | surface.bg1 | `#7DA9EC` / `#1B1A19` | 7.25 | 3.0 | OK | 種別アイコン |
| kind.prefab | selection.sel | `#7DA9EC` / `#33322E` | 5.35 | 3.0 | OK | 種別アイコン |
| kind.empty | surface.bg1 | `#9A958B` / `#1B1A19` | 5.83 | 3.0 | OK | 種別アイコン |
| kind.empty | selection.sel | `#9A958B` / `#33322E` | 4.31 | 3.0 | OK | 種別アイコン |
| kind.folder | surface.bg1 | `#C2B59A` / `#1B1A19` | 8.58 | 3.0 | OK | 種別アイコン |
| kind.folder | selection.sel | `#C2B59A` / `#33322E` | 6.34 | 3.0 | OK | 種別アイコン |
| kind.scene | surface.bg1 | `#D4A08C` / `#1B1A19` | 7.62 | 3.0 | OK | 種別アイコン |
| kind.scene | selection.sel | `#D4A08C` / `#33322E` | 5.63 | 3.0 | OK | 種別アイコン |
| kind.texture | surface.bg1 | `#B6A1E0` / `#1B1A19` | 7.58 | 3.0 | OK | 種別アイコン |
| kind.texture | selection.sel | `#B6A1E0` / `#33322E` | 5.60 | 3.0 | OK | 種別アイコン |
| timeline.key | timeline.trackBg | `#8A857C` / `#1B1A19` | 4.74 | 3.0 | OK | キー |
| timeline.keySel | timeline.trackBg | `#F4F1EA` / `#1B1A19` | 15.41 | 3.0 | OK | 選択キー |
| timeline.keySel | timeline.key | `#F4F1EA` / `#8A857C` | 3.25 | 3.0 | OK | 選択キー ↔ 非選択キー（識別） |
| timeline.playhead | timeline.rulerBg | `#F0C23B` / `#131312` | 11.05 | 3.0 | OK | プレイヘッド（ルーラー上） |
| timeline.playhead | timeline.trackBg | `#F0C23B` / `#1B1A19` | 10.33 | 3.0 | OK | プレイヘッド（トラック上） |
| play.frame | play.frameKeyline ⊕ viewport.bgKey | `#F0C23B` / `#1D1C1A` | 10.10 | 3.0 | OK | 再生枠 ↔ 暗い縁（3D の上） |
| play.frame | play.frameKeyline ⊕ viewport.bgMid | `#F0C23B` / `#171614` | 10.77 | 3.0 | OK | 再生枠 ↔ 暗い縁（3D の上） |
| play.frame | play.frameKeyline ⊕ viewport.bgLow | `#F0C23B` / `#111110` | 11.23 | 3.0 | OK | 再生枠 ↔ 暗い縁（3D の上） |
| play.frame | play.frameKeyline ⊕ viewport.bgEdge | `#F0C23B` / `#0E0E0D` | 11.48 | 3.0 | OK | 再生枠 ↔ 暗い縁（3D の上） |
| play.frame | play.frameKeyline ⊕ viewport.floorNear | `#F0C23B` / `#151513` | 10.89 | 3.0 | OK | 再生枠 ↔ 暗い縁（3D の上） |
| play.frame | play.frameKeyline ⊕ viewport.floorFar | `#F0C23B` / `#0D0C0B` | 11.60 | 3.0 | OK | 再生枠 ↔ 暗い縁（3D の上） |
| play.frame | overlay.scrimPalette ⊕ surface.bg1 | `#F0C23B` / `#121110` | 11.21 | 3.0 | OK | 再生枠 ↔ 暗幕の下のパネル（枠は暗幕の上） |
| play.frame | overlay.scrimPalette ⊕ surface.popup | `#F0C23B` / `#181716` | 10.65 | 3.0 | OK | 再生枠 ↔ 暗幕の下のパネル（枠は暗幕の上） |
| viewport.selOutline ⊕ viewport.bgKey | viewport.iconHalo ⊕ viewport.bgKey | `#E4E1D9` / `#3D3B37` | 8.53 | 3.0 | OK | 選択の芯 ↔ 暗い縁（芯の外側 1px） |
| viewport.iconFill ⊕ viewport.bgKey | viewport.iconHalo ⊕ viewport.bgKey | `#DFDBD3` / `#3D3B37` | 8.05 | 3.0 | OK | ビューポートのアイコン ↔ 縁 |
| viewport.selOutline ⊕ viewport.bgMid | viewport.iconHalo ⊕ viewport.bgMid | `#DEDAD3` / `#2C2A27` | 10.32 | 3.0 | OK | 選択の芯 ↔ 暗い縁（芯の外側 1px） |
| viewport.iconFill ⊕ viewport.bgMid | viewport.iconHalo ⊕ viewport.bgMid | `#DBD7CF` / `#2C2A27` | 10.02 | 3.0 | OK | ビューポートのアイコン ↔ 縁 |
| viewport.selOutline ⊕ viewport.bgLow | viewport.iconHalo ⊕ viewport.bgLow | `#D8D6CF` / `#1E1D1B` | 11.59 | 3.0 | OK | 選択の芯 ↔ 暗い縁（芯の外側 1px） |
| viewport.iconFill ⊕ viewport.bgLow | viewport.iconHalo ⊕ viewport.bgLow | `#D9D5CD` / `#1E1D1B` | 11.51 | 3.0 | OK | ビューポートのアイコン ↔ 縁 |
| viewport.selOutline ⊕ viewport.bgEdge | viewport.iconHalo ⊕ viewport.bgEdge | `#D5D3CC` / `#151513` | 12.19 | 3.0 | OK | 選択の芯 ↔ 暗い縁（芯の外側 1px） |
| viewport.iconFill ⊕ viewport.bgEdge | viewport.iconHalo ⊕ viewport.bgEdge | `#D7D3CC` / `#151513` | 12.28 | 3.0 | OK | ビューポートのアイコン ↔ 縁 |
| viewport.selOutline ⊕ viewport.floorNear | viewport.iconHalo ⊕ viewport.floorNear | `#DCD9D2` / `#282724` | 10.65 | 3.0 | OK | 選択の芯 ↔ 暗い縁（芯の外側 1px） |
| viewport.iconFill ⊕ viewport.floorNear | viewport.iconHalo ⊕ viewport.floorNear | `#DBD7CF` / `#282724` | 10.40 | 3.0 | OK | ビューポートのアイコン ↔ 縁 |
| viewport.selOutline ⊕ viewport.floorFar | viewport.iconHalo ⊕ viewport.floorFar | `#D4D1CB` / `#11100F` | 12.44 | 3.0 | OK | 選択の芯 ↔ 暗い縁（芯の外側 1px） |
| viewport.iconFill ⊕ viewport.floorFar | viewport.iconHalo ⊕ viewport.floorFar | `#D6D2CB` / `#11100F` | 12.62 | 3.0 | OK | ビューポートのアイコン ↔ 縁 |

## 4. 参考値（判定しない）

| 前景 | 背景 | 実色（前景 / 背景） | 比 | 基準 | 判定 | 使う場所 |
|---|---|---|---:|---:|---|---|
| play.frameKeyline ⊕ viewport.bgKey | viewport.bgKey | `#1D1C1A` / `#8A857B` | 4.63 | — | 参考 | 暗い縁 ↔ 3D（参考。枠の識別は 黄↔縁 で保証） |
| play.frameKeyline ⊕ viewport.floorNear | viewport.floorNear | `#151513` / `#55524C` | 2.35 | — | 参考 | 暗い縁 ↔ 3D（参考。枠の識別は 黄↔縁 で保証） |
| play.frame | viewport.bgKey | `#F0C23B` / `#8A857B` | 2.18 | — | 参考 | 再生枠 ↔ 3D（縁なしの参考値） |
| play.frame | viewport.bgMid | `#F0C23B` / `#5E5A53` | 4.08 | — | 参考 | 再生枠 ↔ 3D（縁なしの参考値） |
| play.frame | viewport.bgLow | `#F0C23B` / `#3B3935` | 6.85 | — | 参考 | 再生枠 ↔ 3D（縁なしの参考値） |
| play.frame | viewport.bgEdge | `#F0C23B` / `#262522` | 9.11 | — | 参考 | 再生枠 ↔ 3D（縁なしの参考値） |
| play.frame | viewport.floorNear | `#F0C23B` / `#55524C` | 4.63 | — | 参考 | 再生枠 ↔ 3D（縁なしの参考値） |
| play.frame | viewport.floorFar | `#F0C23B` / `#1B1A18` | 10.34 | — | 参考 | 再生枠 ↔ 3D（縁なしの参考値） |
| viewport.selOutline ⊕ viewport.bgKey | viewport.bgKey | `#E4E1D9` / `#8A857B` | 2.81 | — | 参考 | 選択の芯 ↔ 3D（縁なし） |
| viewport.selOutline ⊕ viewport.bgMid | viewport.bgMid | `#DEDAD3` / `#5E5A53` | 4.94 | — | 参考 | 選択の芯 ↔ 3D（縁なし） |
| viewport.selOutline ⊕ viewport.bgLow | viewport.bgLow | `#D8D6CF` / `#3B3935` | 7.91 | — | 参考 | 選択の芯 ↔ 3D（縁なし） |
| viewport.selOutline ⊕ viewport.bgEdge | viewport.bgEdge | `#D5D3CC` / `#262522` | 10.21 | — | 参考 | 選択の芯 ↔ 3D（縁なし） |
| viewport.selOutline ⊕ viewport.floorNear | viewport.floorNear | `#DCD9D2` / `#55524C` | 5.54 | — | 参考 | 選択の芯 ↔ 3D（縁なし） |
| viewport.selOutline ⊕ viewport.floorFar | viewport.floorFar | `#D4D1CB` / `#1B1A18` | 11.40 | — | 参考 | 選択の芯 ↔ 3D（縁なし） |

## 5. 装飾（判定しない）

| 前景 | 背景 | 実色（前景 / 背景） | 比 | 基準 | 判定 | 使う場所 |
|---|---|---|---:|---:|---|---|
| surface.border | surface.bg1 | `#2C2B28` / `#1B1A19` | 1.23 | — | 装飾 | 面の境界（装飾。識別は面の明度差と記入線が担う） |
| surface.groove | surface.bg1 | `#0A0A09` / `#1B1A19` | 1.14 | — | 装飾 | ドックの溝（装飾） |

## 6. 適用除外

| 前景 | 背景 | 実色（前景 / 背景） | 比 | 基準 | 判定 | 使う場所 |
|---|---|---|---:|---:|---|---|
| text.textDisabled | surface.bg1 | `#6E6A62` / `#1B1A19` | 3.23 | — | 除外 | 操作不可（1.4.3 適用除外。ハッチ / 形と併用） |

## 7. 色覚シミュレーション後の色差（Machado 2009・重度 1.0、CIELAB ΔE76）

| A | B | 基準（対象の型の最小） | 通常 | 1 型 protan | 2 型 deutan | 3 型 tritan | 判定 |
|---|---|---:|---:|---:|---:|---:|---|
| accent.accent `#F0C23B` | status.warn `#F18B73` | 25 | 53.1 | 52.9 | 36.6 | 27.0 | OK |
| accent.accent `#F0C23B` | status.warnText `#F18B73` | 25 | 53.1 | 52.9 | 36.6 | 27.0 | OK |
| accent.accent `#F0C23B` | status.bad `#EC5480` | 25 | 87.9 | 83.9 | 57.6 | 51.1 | OK |
| accent.accent `#F0C23B` | status.good `#58BFB4` | 20 | 82.1 | 72.9 | 76.0 | 73.2 | OK |
| accent.accent `#F0C23B` | status.info `#8FB0D6` | 20 | 93.1 | 95.6 | 93.4 | 53.3 | OK |
| accent.accentDim `#9A8240` | status.warn `#F18B73` | 15 | 38.9 | 19.4 | 15.9 | 37.6 | OK（1・2 型で判定） |
| status.warn `#F18B73` | status.bad `#EC5480` | 20 | 35.1 | 31.2 | 21.1 | 24.2 | OK |
| status.good `#58BFB4` | status.bad `#EC5480` | 20 | 95.9 | 24.5 | 22.3 | 119.3 | OK |
| status.warnText `#F18B73` | status.badText `#FB63B0` | 15 | 50.0 | 47.1 | 38.0 | 15.7 | OK |
| status.good `#58BFB4` | status.info `#8FB0D6` | 10 | 36.0 | 22.7 | 17.7 | 22.2 | OK |
| axis.x `#CF6872` | axis.y `#A9DD83` | 25 | 82.5 | 50.8 | 29.2 | 76.6 | OK（1・2 型で判定） |
| axis.y `#A9DD83` | axis.z `#6E98DA` | 20 | 87.4 | 83.1 | 79.5 | 27.1 | OK |
| axis.x `#CF6872` | axis.z `#6E98DA` | 20 | 64.2 | 43.8 | 58.2 | 86.0 | OK |
| accent.accent `#F0C23B` | axis.y `#A9DD83` | 20 | 47.4 | 30.1 | 32.1 | 46.9 | OK |
| accent.accent `#F0C23B` | kind.light `#E8E0CC` | 15 | 59.7 | 64.2 | 58.0 | 27.4 | OK |
| accent.accent `#F0C23B` | kind.folder `#C2B59A` | 15 | 54.7 | 58.8 | 53.2 | 22.7 | OK |
| accent.accent `#F0C23B` | kind.scene `#D4A08C` | 10 | 54.3 | 59.8 | 49.7 | 11.3 | OK |
| accent.accent `#F0C23B` | kind.physics `#9CC27A` | 15 | 47.8 | 37.5 | 39.0 | 42.6 | OK |
| statusCvd.warn `#937AF7` | statusCvd.bad `#E45268` | 25 | 80.7 | 68.6 | 85.5 | 88.1 | OK（1・2 型で判定） |
| statusCvd.good `#7DC3DE` | statusCvd.bad `#E45268` | 20 | 85.2 | 37.7 | 49.7 | 113.9 | OK（1・2 型で判定） |
| statusCvd.good `#7DC3DE` | statusCvd.warn `#937AF7` | 20 | 68.4 | 47.5 | 40.9 | 33.8 | OK（1・2 型で判定） |
| accent.accent `#F0C23B` | statusCvd.warn `#937AF7` | 25 | 135.6 | 136.3 | 131.0 | 49.2 | OK（1・2 型で判定） |
| accent.accent `#F0C23B` | statusCvd.bad `#E45268` | 25 | 78.6 | 74.0 | 48.9 | 52.3 | OK（1・2 型で判定） |

色だけに頼らない冗長化: good = ✓ の円 / warn = △＋破線バー / bad = × の八角形＋3px 実線バー / info = i の円。

## 8. 黄ゾーン排他（黄の予算の自動検査）

黄ゾーン = OKLCH で C ≥ 0.07 かつ h ∈ [75°, 105°]。accent 系（accent / accentHover / accentPressed / accentDim）以外のトークンがゾーンに入っていれば不合格。

| トークン | 値 | L | C | h | ゾーン内 | accent 系 | 判定 |
|---|---|---:|---:|---:|---|---|---|
| accent.accent | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| accent.accentHover | `#F6D265` | 0.874 | 0.134 | 91.2 | ○ | ○ | OK |
| accent.accentPressed | `#D4A82A` | 0.752 | 0.142 | 87.9 | ○ | ○ | OK |
| accent.accentDim | `#9A8240` | 0.615 | 0.090 | 89.6 | ○ | ○ | OK |
| accent.tabOverline | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| accent.tabOverlineDim | `#9A8240` | 0.615 | 0.090 | 89.6 | ○ | ○ | OK |
| selection.barFocused | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| selection.barUnfocused | `#9A8240` | 0.615 | 0.090 | 89.6 | ○ | ○ | OK |
| viewport.selTombo | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| viewport.selTomboDim | `#9A8240` | 0.615 | 0.090 | 89.6 | ○ | ○ | OK |
| timeline.playhead | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| timeline.playheadDim | `#9A8240` | 0.615 | 0.090 | 89.6 | ○ | ○ | OK |
| play.frame | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| play.framePaused | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| play.tagBg | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| play.tagPausedEdge | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| play.buttonFill | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| play.statusBar | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |
| overlay.focusRing | `#F0C23B` | 0.833 | 0.153 | 89.0 | ○ | ○ | OK |

ゾーン内の非 accent トークン: **0 個**。accent 系の値を持つトークンは、すべて黄の予算表（tokens.json の yellowBudget）の場所に対応する。

ゾーン外だが黄に近い暖色（監視対象。C < 0.07 なので黄としては読めない）: accent.onAccent `#1C1705`（C 0.033, h 95°） / kind.folder `#C2B59A`（C 0.040, h 85°） / play.tagText `#1C1705`（C 0.033, h 95°） / play.buttonIcon `#1C1705`（C 0.033, h 95°）

## 9. v3.1 最終モックの黄の画素比（同じ分類器）

- viewport_edit.png（1600×900）: **0.25%**（上限 1.5% → OK）
- play.png（1600×900）: **0.97%**（上限 2.5% → OK）
- pause.png（1600×900）: **0.66%**（上限 2.5% → OK）

v3 のモックは、この分類器（`build.py` の `mock_ratio`）でそのまま検査できる。実機では `dx12_imgui_screenshot` の PNG に同じ式を当てる。
