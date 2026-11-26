# 性能を測る
> まず測る。ボトルネック(CPU/GPU/待ち)を名指ししてから直す。

```
dx12_perf_stats {}        # FPS・cpuScopeMs(update/buildList/shadowRec/mainRec/editorUi…)・gpuPassMs・analysis(ボトルネック解析)
dx12_benchmark {...}      # 平均 FPS。uncap 既定 true(垂直同期を外して測る)
dx12_diagnose {only}      # エンジン診断(アセット欠落など)。全部やると assets 全走査で数十秒
dx12_render_debug {mode, path}   # 中間バッファ可視化(なぜ変に見えるか/重いかの切り分け)
```
- エディタでは `editorUi`(ImGui)が重いことが多い。`picking` / `gizmo` は editorUi の内数。ゲーム性能は Play で測る。
- 描画を軽くする: `set_render_scale` / `set_occlusion`(Hi-Z カリング)/ `set_depth_prepass` / SSAO・SSR・SSGI の切り替え(`dx12_set_*`)。
- `--background` のウィンドウは画面外でも描画される。人のエディタと数値を比べる時は条件を揃える。

## 性能ゲート(MCP-01)
- `dx12_perf_gate`(長尾)/ `dx12_job_start {kind:"perf_gate"}`: 予算 `testing/budgets/*.json`(`budgets/quality_presets.json` から生成。TS に数値は無い)に、GPU の葉・フレーム p95・CPU p95・VRAM を当てる。
- **合否は gate 列(W1 は DLSS 列)だけ**。TAAU / TAA native は記録(合否にしない)。DLSS が使えず TAA に落ちたときは `inconclusive`(取り違えて合格にしない)。
- 測り方: エンジンは `--no-dred` で起動、計測窓のロック(`%TEMP%\uno-gpu-window.lock`)を取り、VRAM の Budget が前提(`vram.precheckGb`)未満なら中止。クロックは固定せず記録し、3% 以上ずれた回は捨てる。姿勢ごとに 3 回の中央値。履歴(`perf-history.jsonl`)の直近 5 回の中央値より 10% 以上悪化したら `regression`。
- `mode:"ab"`: 2 つのビルド(`a.buildDir` / `b.buildDir`)を ABAB… に交互に測り、A/A も取る。閾値 = max(0.05ms, 1%, A/A の 2σ)。
- プロジェクトの `.dx12/budgets.json`(`p95Ms` / `rows:{<行 id>: ms}` など)で予算を上書きできる。
