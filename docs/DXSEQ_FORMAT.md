# `.dxseq` 形式と評価コアの仕様(シーケンサー S0)

- 対象: `src/sequencer/`(ターゲット `SequencerCore`。標準ライブラリ + nlohmann-json だけ。GPU / ECS / ImGui 非依存)
- 設計: `docs/SEQUENCER_DESIGN.md`(§2 データモデル・§3 評価と再生・§8 S0)。本書は **S0 で確定した意味論の正本**で、設計書と食い違う点は本書が優先する(§17 に差分を列挙)。
- 状態: S0 完了(データモデル + 評価コア + 編集操作 + シリアライズ + 旧台本の変換 + 単体テスト)。UI(S1a)・エンジン統合/適用層(S1b)は未着手。
- 表記: 「ティック」= 整数時刻(既定 6000/秒)。「正準形」= `SerializeSequence` が出力するバイト列(§2)。

---

## 1. 構成(ファイルと責務)

| ファイル | 内容 |
|---|---|
| `SeqTypes.h/.cpp` | `Tick`・時間変換(丸め規則)・ID 形式と決定論的な発行器 `IdAllocator`・JSON 風の汎用値 `SeqValue` |
| `SeqModel.h/.cpp` | `Sequence` / `Binding` / `Track` / `Channel` / `Key` / `Clip` / `EventItem` / `Cut` / `Marker`、検索、構造検査 `ValidateSequence` |
| `SeqCurve.h/.cpp` | イージング統一表・カーブ評価 `EvalChannel`・Auto 接線・ベジェ・範囲外・sRGB→リニア・クォータニオン(slerp / squad) |
| `SeqEval.h/.cpp` | `Evaluate`(状態の評価)・`SelectCut`・マーカー検索・`CollectEvents`(イベント発火)・`SeqPlayback`(再生位置) |
| `SeqOps.h/.cpp` | `SeqOp`(35 種)・`ApplyOp` / `ApplyTxn`(逆 op 生成)・`SeqHistory`(Undo/Redo) |
| `SeqSerialize.h/.cpp` | `.dxseq` の読み書き(決定的な書き出し・往復でバイト一致) |
| `SeqBinding.h/.cpp` | バインディング解決の抽象 `IBindingResolver`・解決順の規則・テスト用 `MapBindingResolver` |
| `SeqConvert.h/.cpp` | 旧 `dx12_sequence_author` の台本 JSON → `Sequence`(§15) |

適用層(エンジンへの書き込み)は S1b。`Evaluate` は**値を返すだけ**で、エンジンの型を一切含まない。

---

## 2. ファイル形式

拡張子 `.dxseq`、UTF-8、LF、末尾改行。JSON。**書き出しは自前で決定的**(nlohmann の整形は使わない)。

```jsonc
{
  "format": "dxseq",            // 固定
  "version": 1,                 // 未対応の版は読み込みを拒否する
  "id": "q_7f3a91c2",           // 省略可(空なら書かない)
  "name": "BossReveal",
  "ticksPerSecond": 6000,
  "frameRate": 30,              // 整数 fps(1..1000)。23.976 / 29.97 は 24 / 30 で編集する
  "range": [0, 27000],          // 省略可。省略 = 全要素の外接 [0, SequenceExtent]
  "render": {"width": 1920, "height": 1080, "shutter": 0.5, "warmup": 8},   // 省略可
  "meta": {"author": "x"},      // 省略可(空なら書かない)。任意のツール/ユーザーのメタデータ
  "cuts":     [ {"id": "c_01", "start": 0, "end": 12000, "camera": "b_cam", "blend": 300}, … ],   // blend は 0 なら書かない
  "markers":  [ {"id": "m_01", "t": 15600, "name": "impact", <params…>}, … ],
  "bindings": [ { … } ]
}
```

ルートのキー順は固定(上記の順)。`cuts` / `markers` / `bindings` は空でも必ず書く(`[]`)。

### 2.1 バインディング

```jsonc
{
  "id": "b_cam", "name": "CutsceneCam", "kind": "entity",          // kind: entity / scene / spawnable
  "hint": {"guid": "00a1b2c3d4e5f607", "path": "Rig/CutsceneCam", "name": "CutsceneCam"},   // 空のメンバは書かない。全部空なら hint ごと省略
  <params…>,                                                        // 未知フィールドはここに保持(辞書順)
  "tracks": [ { … } ]
}
```

### 2.2 トラック

固定フィールドの順: `id` `type` `name`(空なら省略)`mute`(true のときだけ)`lock`(true のときだけ)`rotation`(**Transform のみ常に**。`euler` / `quat`)`path`(空なら省略)`valueType`(float 以外)`colorSpace`(linear 以外)→ `<params>`(辞書順)→ 本体(`channels` / `clips` / `events`)。

`type` と族:

| type | 族 | 本体 | S0 の扱い |
|---|---|---|---|
| `transform` | カーブ | `channels`(`position.{x,y,z}` / `rotation.{x,y,z}`(度)または `rotation.{x,y,z,w}` / `scale.{x,y,z}`) | **評価する** |
| `property` | カーブ | `channels`(通常 `value`。Color は `r,g,b,a`)+ `path`(`"Component.field"`)+ `valueType` / `colorSpace` | **評価する** |
| `camera` | カーブ | `channels`: `fov` `near` `far` `orthoSize` `dofAperture` `dofFocalLength` `dofBlurSize` `dofFocusDist` / params: `focus`(`{"binding": "<id>"}`) | **評価する**(FOV・DoF・フォーカス距離はただのカーブ) |
| `post` | カーブ | `channels`(名前 = `PostProcessSettings` のフィールド名) | 評価する |
| `timeScale` | カーブ | `channels`: `value` | 評価する |
| `light` | カーブ | `channels`(`intensity` `color.r` …) | 評価する |
| `animation` `audio` `vfx` `shake` `subsequence` | クリップ | `clips` | **有効クリップ(localTick・重み)まで評価する**。中身の解釈は S4/S5 |
| `event` | イベント | `events` | `CollectEvents` で発火(§11) |
| `aim` | 制約 | (なし。params: `target` `offset` `roll` `start` `end` …) | 型とシリアライズだけ(評価は S2a) |

カット(`cuts[]`)とマーカー(`markers[]`)はルートに持つ。**カメラカットは「カット帯」= ルート `cuts[]`**(トラックではない)。

### 2.3 チャンネルとキー

```jsonc
"position.x": {
  "pre": "loop", "post": "pingpong",          // 範囲外の挙動。hold(既定)は書かない
  "keys": [
    [0, 0, "a"],                              // [時刻(整数ティック), 値, 補間, …]
    [6000, 90, "s"],
    [9000, 45, "l"],
    [12000, 6, "e:inOutQuad"],
    [19200, 6, "b", 400, 0, 400, 0]           // ベジェ: [t, v, "b", inDt, inDv, outDt, outDv]
  ]
}
```

1 キー 1 行のタプル。補間コード: `s` Step / `l` Linear / `a` Auto / `b` Bezier / `e:<イージング名>`。**補間コードの省略は Auto**(読み込み時のみ許容。書き出しは必ず書く)。`e:linear` は `l` へ正規化される。

チャンネルの並びは配列順(= UI の並び。`position.x,y,z` の順を保つ)。キー配列は**時刻昇順**(同時刻は配列の後ろが勝つ)。

### 2.4 クリップ・イベント・マーカー(1 行オブジェクト)

- クリップ: `{"id", "start", "dur", <params…>}`(`dur` = 0 は点)
- イベント: `{"id", "t", "kind", "name"(空なら省略), <params…>}`(`kind` は `emit` / `lua` / `loadScene` / `log` など)
- マーカー: `{"id", "t", "name", <params…>}`

`<params…>` は `SeqValue`(§14)のメンバをキー辞書順に平らに書いたもの。予約語(`id`・`start`・`dur` 等の型付きフィールド名)は params に使えない。

### 2.5 正準形と往復

- `Serialize(Parse(x)) == x` は **x が正準形のとき**バイト一致(テスト: `tests/data/sequencer/*.dxseq`)。正準でない入力(キー順・`e:linear`・空白・キー順の違い)は読み込み後の書き出しで正準形に直る(**冪等**)。
- 数値: `std::to_chars` の**最短の往復可能表現**(`0.1` は `0.1`、`1e21` は `1e+21`)。`-0` は `0`。NaN / inf は持てない(モデルが拒否する)。
- 未知フィールド: **トラック / クリップ / イベント / バインディング / マーカー**は params に保持して往復で失わない。**ルート / カット / hint / render / チャンネル**の未知フィールドは警告(`ParseResult::warnings`)のうえ無視。
- キーが昇順でなければ安定ソートで直して警告。ID 重複・形式不正・カットの参照先なし・未知のトラック種別・未対応バージョンは**エラー**(位置つきの日本語メッセージ。例 `bindings[0].tracks[0].channels.v.keys[0][2]: 知らない補間種別: zz`)。
- 保存は一時ファイルへ書いてからリネーム(`SaveSequenceFile`。アトミック)。

---

## 3. 時間

整数ティック(`int64`)。`ticksPerSecond` 既定 **6000**(24 / 25 / 30 / 48 / 50 / 60 / 100 / 120 fps がすべて整数ティックに乗る)。23.976 / 29.97 は 24 / 30 で編集し、出力(ffmpeg)のレートだけ変える。妥当な範囲は `|t| ≤ 2^42`。

**丸め規則(すべて整数演算で決定的)**

| 変換 | 規則 |
|---|---|
| 秒 → ティック `SecondsToTicks(sec, tps)` | 四捨五入(0.5 は 0 から遠い側 = `llround`)。非有限は 0。範囲外は ±`kMaxTick` に頭打ち |
| ティック → 秒 | `t / tps`(double) |
| フレーム → ティック `FrameToTick(f, fps, tps)` | `round-half-up(f * tps / fps)`(`floor((2·f·tps + fps) / (2·fps))`。**+∞ 方向**の半値丸め)。`tps % fps == 0` なら厳密 |
| ティック → フレーム(所属)`TickToFrameFloor` | `floor(t · fps / tps)`(負のティックも数学的な floor) |
| ティック → フレーム(最寄り)`TickToFrameNearest` | `round-half-up(t · fps / tps)` |
| `SnapToFrame(t)` | `FrameToTick(TickToFrameNearest(t))` |

`FloorDiv` / `FloorMod` は負の被除数でも数学的な床除算 / 非負の剰余。

---

## 4. ID

- 形式: 1〜40 文字の `[A-Za-z0-9_.-]`。慣例の接頭辞 `q_`(シーケンス)`b_`(バインディング)`t_`(トラック)`k_`(クリップ)`e_`(イベント)`m_`(マーカー)`c_`(カット)+ 8 桁 hex(`FormatId`)。手書き(`c_01`)も可。
- **シーケンス内で全要素が 1 つの名前空間**(トラックと ID が被ってもエラー)。キー(`Key`)には ID が無い(タプル形式のため)。**キーは (トラック ID, チャンネル名, 添字) で指す。**
- `IdAllocator(seed)`: SplitMix64 による決定論的な発行器。同じ seed・同じ予約集合なら同じ列。`Reserve` / `ReserveAllIds(seq, alloc)` で既存 ID を避ける。本番の seed は `IdAllocator::RandomSeed()`。
- ops は**出来上がった ID を運ぶ**(発行は呼び出し側)。だから Redo・リプレイ・別マシンでの再適用で ID が食い違わない。

---

## 5. 補間

キーが持つ `ip` は「**そのキーから次のキーまで**の区間」の補間。最後のキーの `ip` は評価に使わない。

| ip | 意味 | 端点 |
|---|---|---|
| Step `s` | 次のキーまで値を保持 | 次のキーの時刻ちょうどで次の値 |
| Linear `l` | 線形 | |
| Auto `a` | スムーズ(Catmull-Rom → Hermite)。**カメラ位置の既定** | キー値を通る |
| Bezier `b` | 重み付きベジェ | 両端キーを通る |
| Ease `e:<名>` | `v0 + (v1 − v0) · E(p)` | E(0)=0・E(1)=1 |

同時刻のキーは配列の**後ろが勝つ**(`t` がその時刻ちょうどのとき後ろのキーの値。手前の同時刻キーが持つ区間は評価されない)。単一キーのチャンネルは常にその値。空チャンネルは評価結果に出ない(適用側は元の値を保つ)。

### 5.1 Auto の接線(値/ティック)

キー i の接線 `m_i`(両隣の時刻が**厳密に**異なるものだけを隣とみなす):

1. 隣が無い(孤立)→ 0。片側だけ → その側の割線 `d`。
2. 両側あり: 左右の割線 `dl`, `dr`。**`dl · dr ≤ 0`(極値・平坦)なら 0。**
3. それ以外: `m = (v[i+1] − v[i−1]) / (t[i+1] − t[i−1])`(非等間隔の Catmull-Rom)を `|m| ≤ 3·min(|dl|, |dr|)` に制限(Fritsch–Carlson。**単調な区間でオーバーシュートしない**)。

区間 i→i+1 は `Hermite(v_i, m_i, v_{i+1}, m_{i+1}, h = t_{i+1} − t_i, p)`。一直線のキー列は直線のまま(`m` が傾きに一致)。

### 5.2 Bezier

`Key` のハンドル(Bezier のときだけ有効。他は 0 に正規化):

- out ハンドル位置 = `(t + outDt, v + outDv)`(このキーから次へ向かう側)
- in ハンドル位置 = `(t − inDt, v − inDv)`(前から来る側)
- `dt` はティック(**0 以上**)、`dv` は値。対称でなめらかな接線は `inDt == outDt && inDv == outDv`。

区間 A→B(A が Bezier)は、A の out ハンドルと **B の in ハンドル**を使う。B が Bezier でなければ B 側は既定(`dt = span/3`、`dv = Auto の接線 · dt`)。時間軸のベジェ `x(s)` を Newton 法(区間 [0,1] に括った安全な反復・最大 48 回)で解いて `y(s)` を返す。`x` が単調になるよう **`outDt`, `inDt` は `[0, span]` に制限し、和が `span` を超えるなら同率で縮める**(`dv` も同率で縮めて傾きを保つ)。

**ハンドルが `span/3` で `dv = Auto の接線 · dt` の Bezier は Auto と一致する**(差 ≤ 1e-9。テストで検証)。`SetInterp` が Bezier へ切り替えるときは、この Auto と同じ見た目になる既定ハンドルが入る(`DefaultBezierHandles`)。

### 5.3 範囲外(チャンネルの `pre` / `post`)

| 値 | 意味 |
|---|---|
| `hold`(既定) | 端のキーの値 |
| `linear` | 端のキーの接線で延長(Auto = Auto 接線 / Linear = 隣への割線 / Bezier = ハンドルの傾き / Step・Ease = 傾き 0) |
| `loop` | `t' = first + mod(t − first, last − first)`。`t == last` は範囲内なので最後のキーの値(次の周回の頭ではない) |
| `pingpong` | 周期 `2·(last − first)` で折り返す |

キーがすべて同時刻(幅 0)なら loop / pingpong は先頭に固定。

### 5.4 色

- Property の `valueType: "color"` は**成分チャンネル**(`r,g,b,a`)で持つ。キー値の空間は `colorSpace`: `linear`(既定。キー値そのままがリニア値)/ `srgb`(キー値を sRGB とみなす)。
- **補間は常にリニア空間**。`srgb` のとき、評価はキー値(と Bezier のハンドル点)を sRGB→リニアへ変換してから補間し、**リニアで返す**。アルファ(チャンネル名の末尾要素が `a`)は変換しない。`Light` トラックも `colorSpace` を持てる。
- `SrgbToLinear` / `LinearToSrgb`: IEC 61966-2-1(往復で 1e-12 以内)。

### 5.5 bool / int

`valueType` が `bool` / `int` の Property は、全キーが Step でなければならない(構造エラー)。値は 0/1 や整数を `double` で持つ。

---

## 6. イージング統一表

既存の **3 系統**(Lua `Ease` 9 種・C++ `UiEase` 12 種・`sequence.ts` 6 種)の**和集合 15 種**。**式は変えない**(既存作品の見た目を保つ)。ファイル上は `"e:<統一名>"`。`EaseFromName` は統一名に加えて `sequence.ts` の別名(`in` / `out` / `inOut`)も受ける。

| 統一名 | 式 E(p) (p は [0,1] に clamp) | C++ `UiEase` 番号 | Lua `Ease` | `sequence.ts` |
|---|---|---:|---|---|
| `linear` | p | 0 | `linear` | `linear` |
| `inQuad` | p² | — | `inQuad` | **`in`** |
| `outQuad` | 1 − (1 − p)² | — | `outQuad` | **`out`** |
| `inOutQuad` | p < ½ ? 2p² : 1 − (−2p + 2)² / 2 | — | `inOutQuad` | **`inOut`** |
| `inCubic` | p³ | **1**(名前は「イーズイン」) | `inCubic` | — |
| `outCubic` | 1 − (1 − p)³ | **2** | `outCubic` | — |
| `inOutCubic` | p < ½ ? 4p³ : 1 − (−2p + 2)³ / 2 | 3 | — | — |
| `inOutSine` | ½ − ½·cos(πp) | 11 | `inOutSine` | — |
| `outBack` | 1 + c₃(p − 1)³ + c₁(p − 1)²(c₁ = 1.70158, c₃ = c₁ + 1) | 4 | `outBack` | `outBack` |
| `outBounce` | Penner の 4 区間(n₁ = 7.5625, d₁ = 2.75) | 5 | `outBounce` | `outBounce` |
| `outElastic` | 2^(−10p) · sin((10p − 0.75) · 2π/3) + 1 | 6 | — | — |
| `outExpo` | 1 − 2^(−10p)(p ≥ 1 で 1) | 7 | — | — |
| `inBack` | c₃p³ − c₁p² | 8 | — | — |
| `inOutBack` | c₂ = c₁ · 1.525。p < ½ ? (2p)²((c₂+1)·2p − c₂) / 2 : ((2p−2)²((c₂+1)(2p−2) + c₂) + 2) / 2 | 9 | — | — |
| `outQuint` | 1 − (1 − p)⁵ | 10 | — | — |

**罠(3 系統で同じ名前が別の曲線を指す)**: UiEase の 1 番「イーズイン」は**三次**(`inCubic`)、`sequence.ts` の `in` と Lua の `inQuad` は**二次**。`sequence.ts` の `in / out / inOut` を新形式へ移すときは `inQuad / outQuad / inOutQuad` に 1:1(**`inCubic` 系ではない**)。Lua `Ease` にない名前を渡すと Lua は黙って `outQuad` に落ちるが、新形式は未知のイージング名を**エラー**にする。

検証(`SequencerCoreTests`): 全種で E(0) = 0・E(1) = 1(厳密)/ 単調種別の単調性 / `UiEase` 12 種と全 p(0..1 を 1/1000 刻み)で差 ≤ 2e-6(float 実装との差)/ Lua 9 種の式を C++ に写して差 ≤ 1e-12 / `sequence.ts` の 3 式と差 ≤ 1e-15 / 名前と `UiEase` 番号の往復。`double` で評価する(`UiEase` は float)。

---

## 7. 回転

- 既定は **Euler 度の 3 チャンネル**(`rotation.x/y/z`。エンジンの Transform が Euler 保存のため)。スカラーとして補間する。キーを打つ側が前キーとの差を ±180° に連続化する(`UnwrapDegrees(prev, cur)` を提供。`SeqOp` は自動では変えない)。
- `"rotation": "quat"`(Transform のみ)は `rotation.x/y/z/w` の **4 チャンネル**。**4 本のキー数と時刻が揃っていること**(`QuatAlignmentProblem` が診断。揃っていなくても構造エラーにはしないが、キー数が違えば `Evaluate` はその 4 本を**通常のスカラー**として扱う)。評価は 4 本を 1 つの `QuatSample` にまとめる:
  - 各キーを正規化し、**先頭から順に隣り合うキーの半球を揃える**(`q` と `−q` の入れ替わりで出力が跳ばない)。
  - Step = 直前キー / Linear = **slerp**(最短経路)/ Ease = イージングした p で slerp / **Auto・Bezier = squad**(内側・外側の slerp は符号を反転しない。端は自分自身で代用。**等間隔を仮定**した接線。Bezier のハンドルは無視して Auto と同じ)。
  - 範囲外は hold / loop / pingpong(`linear` は hold 扱い)。
  - 出力は常に単位長。平行に近い(sin θ ≈ 0)ときは線形補間 + 正規化。
- クォータニオン ↔ Euler の変換は適用層の責務(エンジンの `QuaternionToEulerDegrees`)。

---

## 8. 評価 `Evaluate(seq, t, out, filter)`

**純関数**。`t`(ティック)だけで結果が決まり、内部状態(キャッシュ・前回位置)を持たない = ヒステリシスなし。同じ入力は**ビット単位で同じ結果**(テストで、10,000 個のランダムな t を任意順に評価した結果が昇順評価とビット一致することを 3 系統のランダムシーケンスで検査)。

出力 `EvalResult`(バッファ再利用可。並びは決定的):

| フィールド | 内容 |
|---|---|
| `channels[]` | `{binding, track, channel, value}`。バインディング配列順 → トラック順 → チャンネル順。**キーが 0 個のチャンネル・ミュートされたトラックは出ない**。クォータニオン回転の 4 本は出ない(`quats` へ) |
| `quats[]` | `{binding, track, q}`(rotation=quat の Transform) |
| `clips[]` | `{binding, track, clip, localTick, weight}`。`start ≤ t < start + dur`(`dur == 0` は `t == start` のみ)で有効。`weight` は params の `blendIn` / `blendOut`(ティック)による線形フェード(0..1) |
| `cutIndex` / `cutBinding` | 選ばれたカット(`cuts[]` の添字)とそのカメラのバインディング位置。無ければ −1 |

- `EvalFilter::bindingEnabled[i] == 0` のバインディングは評価しない(未解決バインディングの無効化に使う。§12)。
- イベントは `Evaluate` には含めない(`CollectEvents`。§11)。`aim`(制約)は S0 では評価しない。
- 性能: 各チャンネルは二分探索で O(log n)。実測は `SEQ_S0_REPORT.md`(50 トラック × 100 キー = 数 µs)。

### 8.1 評価の順序と書き込み側の規約(適用層向け)

設計書 §3.2 のとおり: 各カーブ/クリップ(バインディング配列順 → トラック順)→ 制約(`aim`)→ カット選択 → 加算効果(`shake`)は描画側で足す。**同一 `(binding, path)` を 2 本のトラックが書くときは後勝ち**(検査で警告)。

---

## 9. カメラカット

`cuts[]`: `{id, start, end, camera(バインディング ID), blend}`。`SelectCut(seq, t)`:

- `[start, end)` の半開区間で判定。
- **重なりは配列の後ろが勝つ**(検査で警告)。
- **穴(どのカットにも入らない区間)は −1** = 「現在アクティブなカメラのまま」(設計 §2.3)。
- 例外: `t` が**範囲末尾ちょうど**(`GetRange` の終端)で、そこを終端に持つカットがあり、他に含むカットが無ければ**最後のそのカット**(最終フレームでカメラが戻らない)。範囲の途中のカットの終端では働かない。
- カットが未知のバインディングを指すと構造エラー(`ApplyOp` も拒否する)。`Evaluate` の `cutBinding` は −1 になり得る(カット自体は選ばれる)。
- `blend`(ディゾルブ)は予約(S7)。

---

## 10. マーカー

副作用なし(UI のジャンプ・MCP からの参照用)。`NextMarker(seq, t)` / `PrevMarker(seq, t)` は厳密に後/前の最初/最後のマーカー(同時刻は配列順の先頭)。無ければ −1。

---

## 11. イベントの発火規則

`CollectEvents(seq, u0, u1, opt, out)`。`u0` → `u1` は**展開時刻**(ラップ前の連続した時計)。

### 11.1 基本(位置ベースの通過)

| 移動 | 発火する区間 | 開始時の特例 `startInclusive` |
|---|---|---|
| 前進 `u1 > u0` | `(u0, u1]`(左開右閉) | `[u0, u1]`(**t = 0 のイベントを落とさない**) |
| 後退 `u1 < u0`(`fireBackward` が true のとき) | `[u1, u0)`(前進の鏡像) | `[u1, u0]` |
| `u0 == u1` | 何も発火しない(`startInclusive` でも) | |

- 前進の `(u0, u1]` と後退の `[u1, u0)` は鏡像なので、同じ区間を往復しても「1 回通過するたびに 1 回」だけ発火する。
- 出力の並びは**移動方向に沿った時刻順**。同時刻は バインディング → トラック → イベントの配列順(後退は全体が逆順)。
- ミュートされたイベントトラックは発火しない。範囲外のイベント(`Once`)はフィルタしない(呼び出し側が位置を範囲に収める)。
- **★スクラブ(Seek)では発火しない**: 呼び出し側が `CollectEvents` を呼ばないこと。`SeqPlayback::Seek` がその規則を実装している(`armStart = false`)。

### 11.2 ループ / ピンポン

範囲 `[start, end]`(`opt.rangeStart/End`、周期 `P = end − start`)の周回を展開時刻で扱う。

| mode | 対象イベント | 発火する瞬間(展開時刻 u) |
|---|---|---|
| `Once` | すべて | `u = t` |
| `Loop` | `start ≤ t < end`(**`t == end` は鳴らさない**。次の周回の `start` と同じ瞬間なので二重にならないように) | `u = t + k·P`(k は整数) |
| `PingPong` | `start ≤ t ≤ end` | 前進側 `u = t + 2kP`、折り返し側 `u = start + 2P − (t − start) + 2kP`。**端(`t == start` / `t == end`)は 1 回だけ**(折り返しの重複を除く) |

- 「ちょうど周回境界をまたぐ 1 歩(99 → 101)」は `t = 0` のイベントを 1 回だけ発火する(`Loop`, P = 100)。
- 1 回の呼び出しで 64 周を超える移動は古い側を切り捨てる(暴走防止。`(u1 − u0) > 64·周期`)。

**不変条件(テストで 300 ケース × 3 モード × 前進/後退を検査)**: 任意の分割で `[u0, u1]` を刻んでも(最初の 1 歩だけ `startInclusive`)、**発火する多重集合は一括呼び出しと一致**し、1 tick ずつ位置を調べる独立なオラクルとも一致する。

### 11.3 `SeqPlayback`(再生位置)

`Reset(seq, mode)`(範囲は `GetRange`。範囲が空なら `Once`)/ `SetRate(rate)`(負 = 逆再生)/ `Seek(pos)`(**発火しない**)/ `Play()` / `Pause()` / `Advance(seq, dtSeconds, out)`。

- 内部は展開時刻(int64)+ 端数(double)。1e-9 tick 以内の誤差は整数に吸着する(`0.1 s × 10` で 1 tick も失わない)。
- `Position()`: `Once` は範囲に clamp / `Loop` は `start + mod(u − start, P)`(`[start, end)`)/ `PingPong` は反射。
- `Once` は端で止まり `IsFinished()` になる。
- `Play()` は位置が**再生方向の起点**(前進 = 範囲始点、後退 = 終点)にあるときだけ、最初の 1 歩で起点のイベントを含める(整数 tick に満たない `Advance` では武装を維持)。途中位置から再生しても、その位置のイベントは再発火しない。
- クロックの選択(実時間 / ゲーム時間)は適用層の責務。`Advance` に渡す `dtSeconds` がどちらの dt かを決める(設計 §3.5。既定は実時間)。

---

## 12. バインディング解決(抽象)

`IBindingResolver`(エンジン側が実装): `FindByGuid(guidHex) → TargetId` / `FindByPath(path, out)` / `FindByName(name, out)`。Path / Name は**エンジンの優先順**(先頭 = 採用される方)で候補を返す(エンジンの `Scene::FindEntity` は「最後に作られたものが勝つ」ので、その順)。`TargetId` は不透明な `uint64`(0 = 無効、`~0` = scene)。

`ResolveBinding` の規則(**先に見つかった手段が勝つ**):

1. `kind == scene` → 常に `kSceneTarget`(`ResolveVia::Scene`)。
2. `kind == spawnable` → 未対応(S7)。未解決 + `SpawnableUnsupported`。
3. **上書き**(`BindingOverrides`: バインディング ID → guid hex。SequencePlayer 側の表)→ 見つからなければ次へ。
4. `hint.guid`。
5. `hint.path`(候補が複数なら `AmbiguousPath` の警告 + 先頭を採用)。guid が指定されていたのに見つからずここで解決したら `FellBack`。
6. `hint.name`(候補が複数なら **`AmbiguousName` の警告 + 先頭(= 最後に作られた方)を採用**)。guid / path が指定されていたなら `FellBack`。
7. どれでも見つからなければ **`Unresolved`**。

`ResolveBindings(seq, resolver, overrides)` は全バインディングを解決し、`BindingSet{byBinding[], issues[]}` を返す。追加の警告: **同じ対象に解決された別のバインディング**(`SharedTarget`。同じプロパティを書くと後ろが勝つ)。

**未解決のバインディングは「警告として保持」**: モデルからトラックは消さない(データを失わない)。`BindingSet::EnabledMask()` を `EvalFilter::bindingEnabled` に渡すと、そのバインディングのトラックだけが評価から外れる。`issues[].trackIds` が影響を受けるトラック、`message` が日本語の説明。

`MapBindingResolver`(メモリ上の表。新しく追加された方を先頭候補にする)はテスト用。実エンジンの解決器(guid → 階層パス → 名前、`m_sceneGeneration` キャッシュ)は S1b。

---

## 13. 編集操作 `SeqOp`

**すべての編集(UI / MCP / Lua / Undo)がこの 1 本の経路を通る**。

- `ApplyOp(seq, op, &inverse)`: 成功したら `inverse`(適用すると元とバイト一致に戻る op)を返す。**失敗した op はシーケンスを 1 バイトも変えない**(検証してから変更する)。
- `ApplyTxn(seq, txn, &inverse)`: op 列を先頭から適用し、途中で失敗したらそれまでを逆順に巻き戻して**全体を元に戻す**(エラーに `op[i] <名前>` を付ける)。成功時の `inverse` は逆 op を**逆順に並べた** txn。
- `SeqHistory`: `Execute` / `Undo` / `Redo`(上限 0 = 無制限、既定 1000)。`Execute` 成功で Redo 側を捨てる。履歴の外でシーケンスが書き換えられて逆 op が失敗したら、履歴を破棄してエラーを返す。逆 op は**添字と ID で動く**ので、シーケンスをシリアライズ→パースで作り直しても履歴は有効。**`Record(forward, inverse)`** は「すでに適用済みの変更」を 1 ステップとして積む(ドラッグ中は `ApplyTxn` で直接動かして逆 txn を溜め、離したときに 1 ステップへまとめる用途)。`MoveKey` の結果の添字は、返った逆 op(`OpMoveKey.index`)から読める(選択状態の追従に使う)。
- op は自己完結(ID・添字・値を全部運ぶ)。`index = -1` は既定位置(配列末尾 / キーは時刻順の挿入位置)。それ以外は厳密な添字で範囲検査あり。

**不変条件**: 正準化(ハンドルは Bezier のみ・`ease` は Ease のみ・`(Ease, Linear)` は Linear・`-0` は 0・Transform 以外の `rotation` は Euler・null の params は空オブジェクト)を `Add*` / `Set*` が行うので、モデルの等価 = シリアライズ結果の等価。

### 13.1 一覧(35 種)

| op | 内容 | 逆 op |
|---|---|---|
| `SetName{name}` | シーケンス名 | `SetName`(旧) |
| `SetFrameRate{fps}` | 1..1000 | `SetFrameRate` |
| `SetRange{has,start,end}` | 再生範囲(`has=false` で解除) | `SetRange` |
| `SetRender{render?}` | 出力設定(解像度 > 0・shutter 0..1・warmup ≥ 0) | `SetRender` |
| `SetMeta{meta}` | メタデータ(オブジェクト) | `SetMeta` |
| `AddBinding{binding,index}` | バインディング(トラックつきで丸ごと) | `DeleteBinding` |
| `DeleteBinding{id}` | **カットが参照していると拒否** | `AddBinding`(丸ごと・元の位置) |
| `MoveBinding{id,toIndex}` | 並べ替え | `MoveBinding`(元の位置) |
| `SetBindingHeader{id,header}` | name / kind / hint / params | `SetBindingHeader`(旧) |
| `AddTrack{bindingId,track,index}` | トラック(構造検査・ID の一意) | `DeleteTrack` |
| `DeleteTrack{trackId}` | | `AddTrack`(元のバインディング・位置) |
| `MoveTrack{trackId,toBindingId,toIndex}` | バインディングをまたいで移動可 | `MoveTrack`(元) |
| `SetTrackHeader{trackId,header}` | name / mute / lock / path / valueType / colorSpace / rotation / params | `SetTrackHeader`(旧) |
| `AddChannel{trackId,channel,index}` | カーブ族のみ・名前の一意 | `DeleteChannel` |
| `DeleteChannel{trackId,channel}` | | `AddChannel`(元の位置) |
| `SetChannelExtrap{trackId,channel,pre,post}` | 範囲外挙動 | `SetChannelExtrap` |
| `AddKey{trackId,channel,key,index}` | `-1` = 時刻順(**同時刻の後ろ**)。指定位置が時刻順を壊すと拒否 | `DeleteKey` |
| `DeleteKey{trackId,channel,index}` | | `AddKey`(元の添字) |
| `MoveKey{trackId,channel,index,newT,newIndex}` | 時刻だけ動かす。`newIndex = -1` = 新時刻の順(同時刻の後ろ) | `MoveKey`(新添字 → 旧時刻・旧添字) |
| `SetKeyValue{…,index,v}` | 値 | `SetKeyValue` |
| `SetInterp{…,index,ip,ease}` | 補間。Bezier へ初めて切り替えると既定ハンドルが入る | `SetKey`(旧キー丸ごと) |
| `SetTangent{…,index,inDt,inDv,outDt,outDv}` | Bezier キーのみ | `SetTangent` |
| `SetKey{…,index,key}` | 全フィールド差し替え(時刻は同じであること) | `SetKey` |
| `AddClip` / `DeleteClip` / `SetClip{trackId,clip}` | クリップ族のみ。`SetClip` は `clip.id` で特定して丸ごと差し替え(移動/伸縮/パラメータ) | `DeleteClip` / `AddClip` / `SetClip` |
| `AddEvent` / `DeleteEvent` / `SetEvent{trackId,event}` | イベント族のみ | 同上 |
| `AddMarker` / `DeleteMarker` / `SetMarker` | | 同上 |
| `AddCut` / `DeleteCut` / `SetCut` | カメラが実在するバインディングであること・`end ≥ start` | 同上 |

Bool / Int の Property トラックは Step 以外のキーを受け付けない(`AddKey` / `SetKey` / `SetInterp` / `AddChannel` / `SetTrackHeader`)。

`ApplyOp` が拒否する主な条件: ID の形式不正・重複(**全要素で一意**)/ 添字・挿入位置の範囲外 / 対象(バインディング・トラック・チャンネル・クリップ…)が無い / NaN・inf / 負のハンドル `dt` / 負の `dur` / params が非オブジェクトまたは予約語を含む / 族の不一致(カーブ族にクリップ等)。

**テスト(`TestUndoFuzz`)**: 全 35 種を含むランダムな txn(1〜3 op)を **10,000 回**実行(成功 6,860・拒否 3,140。キー数は 450 で頭打ちにして偏りと実行時間を抑える。拒否した txn がシーケンスを変えていないことを 3 反復に 1 回、直前のコピーとのバイト比較で検査)。97 回に 1 度、シリアライズ→パースで作り直して続行(履歴は有効なまま)。全 Undo で 4 段に 1 回(と最後の 100 段は毎回)状態ハッシュが一致し、**最終的に初期状態とバイト一致**。全 Redo も同様で**最終状態とバイト一致**。全 35 種の op が最低 1 回は成功していることも検査。同じ seed なら最終状態がバイト一致(ID の発行を含めて決定的)。`SEQUENCER_FUZZ_ITERATIONS` で反復数を減らせる(ASAN + Debug 用)。

---

## 14. `SeqValue`(予約種別のパラメータ)

`null` / `bool` / `number`(double)/ `string` / `array` / `object`。**オブジェクトのキーは常に辞書順・一意**(= メモリ上も正準)。クリップ・イベント・マーカー・バインディング・トラックの `params` はこれ。`aim` の `target` などのネストも持てる(深さ ≤ 32)。NaN / inf は持てない。

---

## 15. 旧 `sequence_author` 台本の変換 `ConvertLegacySpec`

`tools/mcp-server/sequence.ts` の `SequenceSpec`(16 種のトラック)→ `Sequence`。純関数・決定論的(ID の seed = 台本名の FNV-1a。同じ台本・同じオプションなら同じバイト列)。`ConvertResult::notes`(`Info` / `Lossy` / `Warning` / `Error`)に**ロスレスでない点を全部残す**。`ConvertOptions` の `initialPosition` / `initialRotation` / `initialShader` / `initialPost` / `initialExposure` / `initialTimeScale` で「再生開始時の現在値」を焼く。

| 旧 type | 新形式 | ロスレスでない点 |
|---|---|---|
| `camera` | `transform` の `position.{x,y,z}`(2 キー: `[t0, from, e:<ease>]` + `[t1, to, s]`)+ 注視があれば `aim` トラック + 全区間の `cuts[]` 1 件 | **`from` 省略**: 直前のカメラ/move の終点 → `initialPosition[name]` の順で解決。どちらも無ければ開始キーを打たず終点にホールド(**Lossy**)。`lookAt`(点)と `lookAtName` の併記は `lookAtName` 優先(Warning)。注視は `aim`(制約族)の `start`/`end`(旧トラックの区間)に落とす。**`aim` の評価は S2a**(区間後の扱いも S2a で決める)。旧式は `isActive` を触らないが、新式は `cuts[]` でカメラを選ぶ(設計 §7.2 の写像) |
| `fade` | scene の `post` トラック `exposure`(`black` 0 / `white` 8 / `clear` 1。線形) | 先行 fade が無い `clear` は 0 から始める(旧仕様の再現)。それ以外の開始露出は `initialPost["exposure"]` → `initialExposure`(既定 1.0。**仮定を Info に残す**) |
| `post` | scene の `post` トラックのチャンネル(`XxxOn` は自動) | 開始値(旧式は `post.get` の現在値)は `initialPost[key]` で焼く。無ければ **Lossy** |
| `timeScale` | scene の `timeScale` トラック `value`(`dur > 0` は線形 2 キー、`dur == 0` は Step) | 終了時の 1.0 復帰・`<name>:done` は `SequencePlayer` の責務(Info) |
| `shake` | カメラの `shake` クリップ `{amp, freq, seed: 0}` | **Lossy**: 旧式は sin の位相をシーケンスの経過時間から取る。新式は `seed` 固定の純関数で、式が同一とは限らない(S2a で旧式に合わせる) |
| `vfx` | `vfx` クリップ(`mode: "burst"`, `preset`, `scale`, `position` または `atName` のバインディング) | |
| `vfxPlay` / `vfxStop` | `vfx` クリップ(`mode: "emitter"`, `layer`)。play と stop を (target, layer) で対にして `dur` を決める | 対応する stop が無い play は終端まで(**Lossy**: 旧式は終了後も放出し続ける)。play の無い stop は Warning で捨てる |
| `shaderParam` | `property`(`MeshRenderer.<param>`, チャンネル `value`) | 開始値は `initialShader["target.param"]`。無ければ **Lossy** |
| `sound` | scene の `audio` クリップ(`dur = 0` の点。bgm は `bus: "music"`・`loop` は旧式の `!== false`、sfx は `volume` と `loop`(旧式の `=== true`)) | 音声の長さが不明 → `dur = 0`(S4 で解決)。バス名 `music` は設計書の例に合わせた仮定(**未確認**) |
| `move` | `transform` の `position.*` | `from` 省略は camera と同じ(**Lossy**) |
| `rotate` | `transform` の `rotation.*`(Euler 度) | 開始回転は `initialRotation`。無ければ **Lossy** |
| `light` | `property`(`Light.intensity` / `Light.color`(Color, `r,g,b`)。`outQuad`) | 旧式は `Lighting.tweenIntensity/Color`(`outQuad`・スケール済み dt = `timeScale` の影響を受ける)。ライトの種類は総称パス `Light.*`(S5 で解決)。開始値は **Lossy** |
| `event` | `event` トラックの `emit`(`data.value`) | |
| `scene` | `event` の `loadScene`(`name` = パス、`fade` param) | |
| `log` | `event` の `log`(`name` = 文言) | |

- イージング: `in/out/inOut/outBack/outBounce` → `inQuad/outQuad/inOutQuad/outBack/outBounce`(**式が同一**。省略時の `inOut` → `inOutQuad`。fade / timeScale は linear、light は outQuad)。
- 全体: `range = [0, 台本の長さ]`、`meta.legacy = {camera, loop, doneEvent}`(旧式の設定は `SequencePlayer` へ移す)。時計は旧式が `time.realDt`(スケール非適用)なので、`SequencePlayer` は `clock: "real"` で作る。
- **往復は保証しない**(新 → 旧は非対応)。不正なトラックは `Error` を残して捨て、他のトラックの変換は続ける。台本が JSON として読めない・`name` 不正・`tracks` 空は `ok = false`。
- 検証(`TestConvert`): 設計書の `SEQUENCE_EXAMPLE`(BossReveal)と、move / rotate / shaderParam / light / vfxPlay+Stop / sound / log / scene / 連続 fade / 点の lookAt を含む台本を変換 → **旧 Lua の式(`from + (to − from) · EASE(k)`)と同じ値**(カメラ・フェード・timeScale・post・move・rotate)、クリップ・イベントの時刻、往復でバイト一致、ゴールデン一致。

---

## 16. 検査 `ValidateSequence`

`Error`(読み込み・`ApplyOp` が拒否): ID 重複/形式不正 / トラック構造(族の整合・チャンネル名の一意・キー昇順・値の有限性・Bool/Int の Step)/ カットの参照先なし・`end < start` / `ticksPerSecond ≤ 0` / `frameRate` が 1..1000 の外 / `range` の逆転 / 時刻が `±2^42` の外。
`Warning`: カットの重なり / `frameRate` がティックに整数で乗らない / クォータニオンの 4 チャンネルが揃っていない。

設計書 §2.5 のうち、**未解決バインディング**は §12、**重複チャンネル所有・`timeScale` と game クロックの併用・`aim` の循環・存在しないアセット・`AnimatorController` 付きへの animation・名前の重複**は適用層/エンジン依存なので S1b 以降(`Validate` の拡張)。

---

## 17. 設計書からの差分(S0 で決めたこと)

1. **トラック種別**: 設計書の `cameraDof` は作らず、DoF/フォーカス距離/FOV は `camera` 種別のチャンネル(`dofAperture` `dofFocalLength` `dofBlurSize` `dofFocusDist` `fov` …)にした(依頼の「Camera(FOV・DoF・フォーカス距離)」に合わせた)。設計書の `property` + `CameraComponent.fovDegrees` も使える(旧台本の変換は camera を Transform、FOV は使わない)。`post` / `timeScale` / `light` / `aim` / `shake` / `subsequence` は設計書どおり。
2. **Loop の終端イベント**: 設計書 §3.5 は `UiAnim` に倣い (prev, end] と書くが、`Loop` は範囲を `[start, end)` として **`t == end` のイベントは鳴らさない**(次の周回の頭と同じ瞬間で二重になるため)。終端で鳴らしたいときは `end − 1` tick に置くか `Once` を使う。`PingPong` は端も含み 1 回だけ。
3. **逆再生でも発火する**(既定。`fireBackward = false` で無効化)。前進の鏡像 `[u1, u0)`。
4. **ID は全要素で 1 つの名前空間**(設計書は接頭辞ごとに分けるが、衝突検査を単純にするため。接頭辞は慣例のまま)。
5. **キーは ID を持たない**(タプル形式のため)。`SeqOp` は (トラック ID, チャンネル名, 添字) で指し、同時刻キーは添字で順序を厳密に扱う。
6. **`DeleteBinding` はカットが参照していると拒否**(dangling なカットを作らない。UI は 1 つの txn でカットを先に消す)。
7. **クォータニオンの Auto / Bezier は squad**(Bezier のハンドルは無視)。設計書は「slerp」までしか触れていない。等間隔を仮定した接線なので、キー間隔が極端に不均一だと速度が不均一になる。
8. **未知フィールド**: トラック/クリップ/イベント/バインディング/マーカーは params に保持(設計書の「未知フィールドは無視」より保守的。将来の予約種別を失わない)。
