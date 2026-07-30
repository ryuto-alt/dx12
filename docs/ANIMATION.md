# アニメーション（スケルタル）

キャラクターの「リアルさ」はシェーダより**アニメーション**で決まる。この文書は
スケルタルアニメの基盤（ポーズ空間ブレンド / ステートマシン `.animfsm` /
ブレンドツリー / レイヤー / フット IK）の仕様と使い方をまとめる。

> 2D スプライトの `.spranim` と UI の `.uianim` は別系統。`docs/AUTHORING.md` を参照。
> ボーンの無いモデルの「ノードアニメ」（`NodeAnimationComp`）も別系統で、ここでは扱わない。

---

## 1. 全体像

```
[clip A] --SamplePose--> Pose_A --\
[clip B] --SamplePose--> Pose_B ---> Blend --> Pose --> Layers --> FK --> global[] --> IK --> skinning[]
[clip C] --SamplePose--> Pose_C --/
```

中核は **`AnimPose`（ローカル空間 TRS の配列）** という中間表現
（`src/animation/AnimPose.h`）。位置=lerp / 回転=**slerp** / スケール=lerp を
「ローカル TRS の段階で」行い、そのあと FK でボーン行列を合成する。

> ⚠️ 2026-07-26 より前は、クロスフェードが**最終スキニング行列**を要素ごとに
> 線形補間していた。行列の線形補間は回転を保存しないので、遷移のたびに骨が縮んでいた
> （90 度離れた 2 姿勢を t=0.5 で混ぜると骨長が 1.000 → **0.707**）。
> ローカル TRS ポーズを挟むことで解消した。この中間表現は IK・ブレンドツリー・
> レイヤー・ラグドールの全部が必要とする。

**行列規約（絶対に崩さないこと）**: ローカル行列は `S * R * T`（行ベクトル規約）、
グローバルは `local * global[parent]`、最後に `XMMatrixTranspose` して HLSL の列優先へ渡す。

**ボーン上限**: 256（`Skeleton::kMaxBones`）。超えると警告が出て、超過分はアップロードされない。

---

## 2. 2 つの使い方

### (a) 何も足さない — 単一クリップ + クロスフェード（従来どおり）

`SkeletalAnimation` はモデルのロード時に自動で付く。Lua / MCP から直接クリップを切り替える。

```lua
e:playAnimByName("Walk", 0.25)   -- 0.25 秒でクロスフェード
e:setLooping(true)
e:setAnimSpeed(1.5)
```

### (b) `AnimatorController` を足す — データ駆動のステートマシン

`.animfsm`（JSON アセット）にステート・遷移・条件・ブレンドツリー・レイヤー・
イベントを書き、Lua は**パラメータだけ**を書く。

```lua
e:setAnimFloat("speed", math.sqrt(vx*vx + vz*vz))
e:setAnimBool("grounded", physics:isGrounded(e))
e:setAnimTrigger("jump")
```

**FSM の構造を Lua で組む API は無い**（真実の源を `.animfsm` に一本化するため）。
グラフエディタ UI も作らない — このエンジンは「ゲームの中身は全部データ
（シーン JSON + `.lua`）」という設計思想で、人間はテキストエディタで、
Claude Code も同じテキストで編集できる方が整合する。
Inspector は**読み取り専用のライブ表示**（現在ステート / レイヤー重み / パラメータ）に留めている。

---

## 3. `.animfsm` のスキーマ

`AnimatorController.graphPath` に assets 相対で指定する（例 `animfsm/fox_locomotion.animfsm`）。
未知のフィールドは無視され、欠落は既定値になる。JSON コメント（`//`）が使える。

```jsonc
{
  "version": 1,

  // ---- パラメータ（Lua / MCP から書く値）----
  "parameters": [
    { "name": "speed",    "type": "float",   "default": 0 },
    { "name": "grounded", "type": "bool",    "default": true },
    { "name": "jump",     "type": "trigger" }          // trigger の初期値は必ず false
  ],

  // ---- クリップに埋めるイベント（足音等）----
  // glTF/FBX にイベントを埋める手段が無いのでここに書く。時刻は秒。
  "clipEvents": {
    "Walk": [ { "time": 0.25, "name": "footstep", "string": "left" },
              { "time": 0.75, "name": "footstep", "string": "right", "float": 0.5 } ]
  },

  // ---- 別ファイルから追加で読むクリップ ----
  // 名前一致でリターゲット無しに読む（同じスケルトンであること）
  "extraClips": [ { "path": "models/human/sneakWalk.gltf", "name": "sneakWalk" } ],

  // ---- レイヤー（0 番が基礎。1 番以降が上に乗る）----
  "layers": [
    {
      "name": "Base",
      "weight": 1.0,
      "blend": "override",          // "override" | "additive"
      "referenceClip": "",          // additive の参照ポーズ（空 = バインドポーズ）
      "mask": null,                 // 下記 §6
      "defaultState": "Idle",

      "states": [
        { "name": "Idle", "clip": "Survey", "loop": true, "speed": 1.0 },
        { "name": "Jump", "clip": "Jump",   "loop": false, "speed": 1.0 },

        // ブレンドツリーのステート（clip の代わり）
        { "name": "Locomotion", "loop": true,
          "blendTree": {
            "type": "1d", "param": "speed",
            "samples": [ { "value": 0.0, "clip": "Survey" },
                         { "value": 1.4, "clip": "Walk"   },
                         { "value": 4.5, "clip": "Run"    } ],
            "syncPhase": true, "speedMatch": true } },

        // 2D ブレンドツリー（ストレイフ移動・8方向ロコモーション）。
        //   "2d"      = Freeform Cartesian（前後左右の速度をそのまま座標にする）
        //   "2dPolar" = Freeform Directional（向きの違いを強く効かせる。polarAlpha で調整）
        // ★paramY を書かないと 1D として扱う（全サンプルが y=0 の直線に並んで
        //   1D と区別が付かない結果になるため、黙って変な絵を出さない）。
        // ★speedMatch は 1D 専用。2D では「どの軸が速度か」がツリー次第で決まらないので無視される。
        { "name": "Strafe", "loop": true,
          "blendTree": {
            "type": "2d", "param": "moveX", "paramY": "moveZ",
            "samples": [ { "value":  0.0, "valueY":  0.0, "clip": "Idle"      },
                         { "value":  0.0, "valueY":  1.0, "clip": "RunFwd"    },
                         { "value":  0.0, "valueY": -1.0, "clip": "RunBack"   },
                         { "value": -1.0, "valueY":  0.0, "clip": "RunLeft"   },
                         { "value":  1.0, "valueY":  0.0, "clip": "RunRight"  } ],
            "syncPhase": true } }
      ],

      // 評価は**宣言順**。最初に成立したものが勝つ。
      // 「被弾/ジャンプのトリガを必ず勝たせたい」なら、その遷移を先に書くこと。
      "transitions": [
        { "from": "Idle", "to": "Locomotion", "duration": 0.15,
          "conditions": [ { "param": "speed", "op": "gt", "value": 0.1 } ] },

        { "from": "Locomotion", "to": "Idle", "duration": 0.20,
          "conditions": [ { "param": "speed", "op": "le", "value": 0.1 } ] },

        { "from": "*", "to": "Jump", "duration": 0.10,          // "*" = Any State
          "conditions": [ { "param": "jump", "op": "trigger" } ] },

        { "from": "Jump", "to": "Idle", "duration": 0.20,
          "exitTime": 0.8,                 // 現ステートの正規化時間がここに達してから
          "interruptible": false,          // この遷移中は別の遷移へ割り込ませない
          "conditions": [ { "param": "grounded", "op": "isTrue" } ] }
      ]
    }
  ]
}
```

### 条件の演算子（`op`）

| op | 意味 | 対象 |
|---|---|---|
| `gt` `lt` `ge` `le` `eq` `ne` | `value` との数値比較 | float |
| `isTrue` `isFalse` | 真偽 | bool |
| `trigger` | 立っていれば成立。**発火した遷移が消費する**（1 回だけ）。★遷移が成立しなかった場合は**消費されず立ちっぱなしになる**（Unity と同じ）。被弾中にもう一度 `setAnimTrigger("hit")` を呼ぶと、そのステートを抜けた直後にもう一度のけぞる。連打・多段ヒットで踏むので、要らなくなったら`setAnimBool("hit", false)` で自分で降ろすこと | trigger |

- 存在しないパラメータを参照した条件は**黙って不成立**（エラーにしない＝既存 Lua API と同じ流儀）。
- `conditions` が 0 件なら常に成立 → `exitTime` だけで遷移する形になる。

### 遷移のルール

- **評価順は宣言順**。優先度はデータ側の責任。
- `from: "*"`（Any State）は**自分自身へは遷移しない**。
- `exitTime` は現ステートの**正規化時間（0..1）**。ループするステートなら毎周この窓に入る。
- `duration <= 0` は即時遷移（0 除算・NaN を作らない）。
- `interruptible: false` の遷移が進行中の間は、新しい遷移を一切選ばない。

---

## 4. ブレンドツリー（1D）

`speed` のようなパラメータ 1 軸に沿って複数クリップを混ぜる。歩き→走りの
スナップ切り替えが消え、さらに**足の滑りが大幅に減る**。

| フィールド | 意味 |
|---|---|
| `param` | 入力パラメータ名（float） |
| `samples[].value` | 軸上の位置。**この値は「そのクリップ本来の移動速度(m/s)」にする**と `speedMatch` が効く |
| `samples[].clip` | クリップ名 |
| `samples[].speed` | ★**未実装**。JSON は読まれて往復もするが、再生レートには一切反映されない（位相同期 `syncPhase` と両立しないため保留中）。走りだけ速く回したいときはステート側の `speed` か `speedMatch` を使う |
| `syncPhase` | 全クリップを正規化時間で**位相同期**する（既定 true）。**切ると walk と run の接地がずれて足が 2 本に見える** |
| `speedMatch` | 実パラメータ値 ÷ 補間された基準速度 で再生レートを補正する（既定 false）。ルートモーション無しで足滑りを消す本命 |

重みは区間線形補間（`Eval1DBlend`）。範囲外は端でクランプ、重みの合計は必ず 1。

---

## 5. アニメーションイベント（足音など）

`clipEvents` に書いたイベントは、そのクリップが再生されるとき `(前フレーム, 今フレーム]`
の区間に入った時点で **EventBus** へ発火する。Lua は既存の口で受ける:

```lua
events:on("footstep", function(ev)
    local side = ev.string        -- "left" / "right"
    scene:playSound("audio/sfx/foot_" .. side .. ".wav")
end)
```

ペイロード: `string`（`stringParam`）、`float`、`clip`、`layer`、`time`。
`source` は発火したエンティティ。

- 境界は**開いた左・閉じた右**で統一しているので、ループ 1 周につきちょうど 1 回。
- ラップしたフレームは `(prev, end] + [0, cur]` の 2 区間を見る（取りこぼさない）。
- **クロスフェード中は「重みが優勢な側」のクリップからだけ拾う**。両方から拾うと
  遷移中だけ足音が二重に鳴るため。
- `AnimatorController.eventChannel` を設定すると、イベント名の前にその文字列が付く
  （`"player."` → `"player.footstep"`）。複数キャラを鳴らし分けたいときに使う。

---

## 6. レイヤーとボーンマスク（上半身だけ別アニメ）

レイヤー 1 番以降は下のレイヤーの結果に重ねる。

```jsonc
{ "name": "UpperBody", "weight": 1.0, "blend": "override",
  "mask": { "include": ["Spine1"], "includeChildren": true, "weight": 1.0 },
  "defaultState": "Aim",
  "states": [ { "name": "Aim", "clip": "aim_center", "loop": true } ],
  "transitions": [] }
```

- `include` に挙げたボーン（と `includeChildren: true` ならその子孫すべて）だけが
  そのレイヤーの影響を受ける。**マスク外のボーンは下位レイヤーの値がビット単位でそのまま残る**。
- `blend: "additive"` は「参照ポーズからの差分」を下位に加算する。
  `referenceClip` を指定しないとバインドポーズが参照になる（多くの場合それでは意図と違うので、
  加算レイヤーを使うなら**必ず `referenceClip` を指定すること**）。
- レイヤー重みは実行時に動かせる: `e:setAnimLayerWeight(1, 0.5)`（構え動作のフェードイン等）。

---

## 7. フット IK（接地補正）

`FootIK` コンポーネント。地面へレイキャストして足首の高さと向きを合わせ、
届かない側に合わせて腰を下げる。斜面・階段で効果が大きい。

詳細は `FootIK` のインスペクタと `docs/API_REFERENCE.md` を参照。要点だけ:

- **Play 中しか動かない**（`PhysicsSystem` が body を持つのが Play 中だけのため）。
  エディタのシーンビューでは接地しない。これは仕様。
- ボーン名が空なら一般的な命名（`mixamorig:LeftFoot` / `foot.L` / `Bip01 L Foot` 等）から
  自動推定する。外れたら `FootIK` の文字列フィールドで明示指定する。
  推定結果は `dx12_get_anim_state` の `footIK` に出る。
- ★**`leftToeBone` / `rightToeBone` だけは指定しても効かない**。ボーンの解決までは走るが、
  `ApplyFootIK` は hip/knee/foot の 2 ボーン解しか使わず toe を一度も読まない
  （つま先ピボット用の受け口として置いてあるだけ。MCP のスキーマにも RESERVED と明記）。
- 面法線が要るので `PhysicsSystem::Raycast` を使う（本物の面法線を返す）。
  （かつて法線を `(0,1,0)` で固定して返すフェイク版が併存していたが、`4e55f4b` で削除し
   1 本に統一済み。Lua の `physics:raycast` も本物の法線を返す）

---

## 8. Lua API

| API | 意味 |
|---|---|
| `e:playAnim(clipIndex, blend)` | クリップ番号で再生（クロスフェード） |
| `e:playAnimByName(name, blend)` | クリップ名で再生 |
| `e:setLooping(loop)` | ループ ON/OFF |
| `e:setAnimSpeed(speed)` | 再生速度倍率（2.0 で 2 倍速、0 で一時停止） |
| `e:getAnimCount()` / `e:getAnimName(i)` | クリップ数 / 名前 |
| `e:setAnimFloat(name, v)` | FSM の float パラメータ |
| `e:setAnimBool(name, v)` | FSM の bool パラメータ |
| `e:setAnimTrigger(name)` | 1 回だけ立つトリガ |
| `e:getAnimFloat(name)` / `e:getAnimBool(name)` | 読み出し（無ければ 0 / false） |
| `e:getAnimStateName(layer?)` | 現在のステート名 |
| `e:getAnimNormalizedTime(layer?)` | 現ステートの正規化時間 0..1（攻撃の当たり判定窓に） |
| `e:playAnimState(name, blend?)` | ステートへ強制遷移（デバッグ / カットシーン） |
| `e:setAnimLayerWeight(layer, w)` / `e:getAnimLayerWeight(layer)` | レイヤー重み |

`AnimatorController` が無いときは全部**黙って no-op / 既定値**を返す（既存 API と同じ流儀）。

---

## 9. MCP ツール

| ツール | 用途 |
|---|---|
| `dx12_get_anim_state` | クリップ一覧 + 現在のステート / レイヤー / パラメータ / ボーン数 |
| `dx12_play_anim` | `clip`/`clipName` でクロスフェード、または `state` で FSM 遷移 |
| `dx12_set_anim_param` | FSM パラメータを外から叩いて遷移を検証する |
| `dx12_describe_anim_graph` | `.animfsm` の構造（ステート/遷移/条件/ブレンドツリー/レイヤー）を JSON で返す |

`.animfsm` を**書き込む** MCP ツールは意図的に作っていない。ただの JSON なので
通常の Write/Edit で編集し、`dx12_open_scene` でシーンを開き直せば読み直される。

---

## 10. よくある落とし穴

| 症状 | 原因 |
|---|---|
| キャラが T ポーズのまま | `.animfsm` のクリップ名がモデルのクリップ名と一致していない。`dx12_get_anim_state` の `clips` を見る。解決失敗はエンジンログに `AnimGraph: 解決できない参照 [...]` として出る |
| 遷移しない | 条件のパラメータ名が `parameters` に無い（存在しないパラメータの条件は常に不成立）。`dx12_get_anim_state` の `parameters` に出ているか確認 |
| トリガが効かない | 別の遷移が宣言順で先に成立している。`.animfsm` でその遷移を上に移す |
| 歩き→走りで足が 2 本に見える | ブレンドツリーの `syncPhase` が false |
| 足が滑る | `speedMatch` が false、または `samples[].value` がクリップ本来の移動速度になっていない |
| 遷移のたびに足音が 2 回鳴る | 起きないはず（優勢側からしか拾わない）。起きたらレイヤーが二重に同じクリップを鳴らしている |
| 複製したキャラが変な状態から始まる | 起きないはず（`AnimatorController` のコピーは実行状態を捨てて読み直す）|
