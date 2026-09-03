# 演出レシピ集 — シェーダー × イベント

「部屋に入った瞬間まぶしくする」のような **その場かぎりの演出** を、**Lua を 1 行も書かずに**
作るためのレシピ集。使うのは 2 つだけ:

1. **カスタムシェーダーの名前付きパラメーター**（v1.12.3〜）
   HLSL に `float _Flash; // @range(0,1)` と書くと、その名前で Inspector にスライダーが出る。
2. **Trigger の `AnimShaderParam` / `SetShaderParam` アクション**（v1.12.4〜）
   その名前を指定して、値を時間をかけて動かす。

仕組みの説明は [`AUTHORING.md` の 2 章](AUTHORING.md)（Trigger + Action）と
[6 章](AUTHORING.md)（カスタムシェーダー）にある。ここは**動くものをそのまま貼れる**ことだけを目的にする。

> このページの HLSL は全て DXC（`vs_6_0` / `ps_6_0`）でコンパイル確認済み。
> `<project>/assets/shaders/` に置いて保存すれば、そのままホットリロードされる。

---

## 0. 共通の手順

どのレシピも流れは同じ。

1. `<project>/assets/shaders/<名前>.hlsl` を作る
   （エディタなら **ツールバー「ファイル > 新規シェーダー」**、または アセットブラウザ右クリック）
2. **割り当てる**
   - 画面全体 → カメラの Inspector 「画面シェーダー」
   - 1 個のモデル → その `MeshRenderer` の Inspector 「Shader」
3. Inspector に生えたスライダーを**手で動かして見た目を決める**（ここが一番大事）
4. Trigger を置いて、アクションでそのパラメーターを動かす

### アクションの読み方

| フィールド | `SetShaderParam` (11) | `AnimShaderParam` (12) |
|---|---|---|
| `target` | 対象エンティティ（空 = Filter 対象） | 同左 |
| `str` | パラメーター名（**コンボから選ぶ**） | 同左 |
| `num` | 代入する値 | **開始値**（発火した瞬間に入る） |
| `vec[0]` | — | **終了値** |
| `vec[1]` | — | **秒数** |
| `vec[2]` | — | イージング `0`=等速 `1`=減速 `2`=加速 `3`=両端ゆるめ |

Inspector ではパラメーター名は**手打ちではなくコンボ**で選ぶ。対象のシェーダーが実際に
宣言している名前しか出ないので、打ち間違いで空振りすることがない。

---

## 1. 部屋に入ったらまぶしい

一番作りたいやつ。中心から白が広がって、目が慣れるように戻る。

**`assets/shaders/Blind.hlsl`**

```hlsl
Texture2D    gScreen : register(t0);
Texture2D    gDepth  : register(t1);
SamplerState gLinear : register(s0);
SamplerState gPoint  : register(s1);

cbuffer ScreenShaderCB : register(b0)
{
    float4 resolution;
    float4 timeParams;
    float  _Blind;        // @range(0,1)   まぶしさ ← トリガーが動かす
    float3 _LightColor;   // @color        光の色
    float4 cameraParams;
    float4 uvOffsetScale;
};

float3 SampleScreen(float2 uv)
{
    return gScreen.Sample(gLinear, uv * uvOffsetScale.zw + uvOffsetScale.xy).rgb;
}

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 col = SampleScreen(i.uv);
    float  t   = saturate(_Blind);

    // 中心ほど強く光る（画面比を補正して真円にする）
    float2 d = (i.uv - 0.5) * float2(timeParams.z, 1.0);
    float  r = length(d) * 1.4;

    // 白飛びは中心から。t が下がるにつれて外周から引いていく
    float bloomMask = saturate(1.0 - r / max(t * 1.6, 1e-4));
    col = lerp(col, _LightColor, saturate(bloomMask * t));

    // 全体のかぶり（目が慣れていく感じ）
    col += _LightColor * t * 0.35;

    // 白飛び中は色が薄くなる
    float gray = dot(col, float3(0.299, 0.587, 0.114));
    col = lerp(col, gray.xxx, t * 0.4);

    return float4(col, 1.0);
}
```

**割り当て**: カメラ（`MainCamera`）の Inspector 「画面シェーダー」→ `Blind.hlsl`。
`_LightColor` を白にして、`_Blind` を手で 1 まで上げて見え方を確認する。**平常時は 0 に戻しておくこと**。

**トリガー**: 部屋の入口に Trigger を置く。

```json
{
  "name": "RoomEntrance",
  "transform": { "position": [0, 1, 10], "rotation": [0,0,0], "scale": [1,1,1] },
  "trigger": {
    "shape": 0,
    "halfExtents": [2.0, 2.0, 0.5],
    "filter": "Player",
    "once": true,
    "actions": [
      { "when": 0, "type": 12, "target": "MainCamera", "str": "_Blind",
        "num": 1.0, "vec": [0.0, 0.8, 1.0] }
    ]
  }
}
```

入った瞬間 `_Blind = 1`（真っ白）→ 0.8 秒かけて**減速**しながら 0 へ。
`once: true` なので初回だけ。毎回光らせたいなら `false`。

> **エディタで組む場合**: Hierarchy「＋追加 → Trigger」→ アクション追加 →
> いつ=`入った時 Enter` / 何を=`AnimShaderParam` / 対象=`MainCamera` /
> パラメーター名=`_Blind` / 開始値=`1` / 終了値=`0` / 秒数=`0.8` / イージング=`減速 out`

### 派生: 暗転・被弾の赤フラッシュ

同じ形の、もっと単純な塗りつぶし版。色を変えるだけで用途が変わる。

**`assets/shaders/Flash.hlsl`**（`ScreenShaderCB` と `SampleScreen` / `VSMain` は上と同じ）

```hlsl
    float  _Flash;        // @range(0,1)   0=素通し / 1=_FlashColor で塗りつぶし
    float3 _FlashColor;   // @color        白=まぶしい / 黒=暗転 / 赤=被弾
```

```hlsl
float4 PSMain(VSOut i) : SV_TARGET
{
    float3 col = SampleScreen(i.uv);
    col = lerp(col, _FlashColor, saturate(_Flash));
    return float4(col, 1.0);
}
```

- **暗転**: `_FlashColor` を黒に。`0 → 1` で暗くして、`1 → 0` で明ける
- **被弾**: `_FlashColor` を赤に。`0.6 → 0` を `0.25` 秒・**減速**で。短いほど「殴られた」感じになる

---

## 2. 物が光る（合図・拾えるアイテム・起動した装置）

扉やクリスタルなど、**1 個のモデル**を光らせる。

**`assets/shaders/Glow.hlsl`**

```hlsl
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    float  _Glow;        // @range(0,6)   発光の強さ ← トリガーが動かす
    float3 _GlowColor;   // @color        光る色
    float4 shaderParams; // 未使用（Lua: scene:setMeshParams の枠）
};

cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float3   lightDir;   float time;
    float3   lightColor; float ambientStrength;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV  : SV_POSITION;
    float3 worldNormal : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    o.positionSV  = mul(float4(input.position, 1.0f), mvp);
    o.worldNormal = normalize(mul(input.normal, (float3x3)model));
    o.color       = input.color;
    o.texCoord    = input.texCoord;
    return o;
}

float4 PSMain(PSInput i) : SV_TARGET
{
    float4 albedo = g_albedo.Sample(g_sampler, i.texCoord) * i.color;

    float3 N = normalize(i.worldNormal);
    float  ndotl = max(dot(N, normalize(-lightDir)), 0.0f);
    float3 col = albedo.rgb * (lightColor * ndotl + ambientStrength);

    // 縁ほど強く光らせると「発光している物体」に見える。
    // ビュー空間の法線の z が 0 に近い面＝カメラから見て横を向いている＝輪郭。
    float3 Nv  = normalize(mul((float3x3)view, N));
    float  rim = pow(1.0 - saturate(abs(Nv.z)), 2.0);

    col += _GlowColor * _Glow * (0.35 + rim);
    return float4(col, albedo.a);
}
```

**割り当て**: 光らせたいメッシュの Inspector 「Shader」→ `Glow.hlsl`。

**トリガー**: 対象をそのメッシュにするだけ。

```json
{ "when": 0, "type": 12, "target": "Crystal", "str": "_Glow",
  "num": 0.0, "vec": [3.0, 0.4, 2.0] }
```

`0 → 3` を 0.4 秒・**加速**（`vec[2] = 2`）で。じわっと点いて、最後にぐっと明るくなる。

> `_Glow` を **1 より大きく**すると HDR に振り切れてブルームが乗る。`@range(0,6)` にしてあるのはそのため。
> ポストプロセスのブルームが切ってあると光って見えないので、その場合は Post Process 窓を確認する。

---

## 3. 溶けて消える／現れる（撃破・転送・アイテム出現）

**`assets/shaders/Dissolve.hlsl`**

```hlsl
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    float  _Dissolve;     // @range(0,1)      0=そのまま / 1=完全に消える
    float3 _EdgeColor;    // @color           溶ける境界の発光色
    float  _NoiseScale;   // @range(0.5,20)   模様の細かさ
    float  _EdgeWidth;    // @range(0.01,0.3) 光る縁の太さ
    float2 _reserved;
};

cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float3   lightDir;   float time;
    float3   lightColor; float ambientStrength;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV  : SV_POSITION;
    float3 worldNormal : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float3 worldPos    : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    o.positionSV  = mul(float4(input.position, 1.0f), mvp);
    o.worldNormal = normalize(mul(input.normal, (float3x3)model));
    o.color       = input.color;
    o.texCoord    = input.texCoord;
    o.worldPos    = mul(float4(input.position, 1.0f), model).xyz;
    return o;
}

// テクスチャを使わない手続き的ノイズ（追加のレジスタを宣言しなくて済む）
float Hash3(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float Noise3(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(Hash3(i + float3(0,0,0)), Hash3(i + float3(1,0,0)), f.x),
                     lerp(Hash3(i + float3(0,1,0)), Hash3(i + float3(1,1,0)), f.x), f.y),
                lerp(lerp(Hash3(i + float3(0,0,1)), Hash3(i + float3(1,0,1)), f.x),
                     lerp(Hash3(i + float3(0,1,1)), Hash3(i + float3(1,1,1)), f.x), f.y), f.z);
}

float4 PSMain(PSInput i) : SV_TARGET
{
    float scale = max(_NoiseScale, 0.001);
    float n = Noise3(i.worldPos * scale);

    // ★n がしきい値を下回った画素を捨てる = 穴が開いて広がっていく
    clip(n - _Dissolve);

    float4 albedo = g_albedo.Sample(g_sampler, i.texCoord) * i.color;
    float3 N = normalize(i.worldNormal);
    float  ndotl = max(dot(N, normalize(-lightDir)), 0.0f);
    float3 col = albedo.rgb * (lightColor * ndotl + ambientStrength);

    // 消える寸前の縁を光らせる（HDR に振り切ってブルームを焚く）
    float edge = smoothstep(0.0, max(_EdgeWidth, 1e-4), n - _Dissolve);
    col = lerp(_EdgeColor * 6.0, col, edge);

    return float4(col, albedo.a);
}
```

**消す**（撃破・回収）:
```json
{ "when": 0, "type": 12, "target": "Enemy", "str": "_Dissolve",
  "num": 0.0, "vec": [1.05, 0.7, 0.0] }
```
終了値を **1.0 より少し大きく**（`1.05`）しておくと、最後に取り残しの画素が出ない。

**現れる**（転送・出現）: 開始と終了を入れ替えるだけ。
```json
{ "when": 0, "type": 12, "target": "Item", "str": "_Dissolve",
  "num": 1.05, "vec": [0.0, 0.5, 1.0] }
```

> `clip()` は画素を捨てるだけでジオメトリは残る。当たり判定は消えないので、
> 本当に消したいなら `Destroy` アクションを別に足すか、Lua 側で消す。
> `_NoiseScale` はモデルの大きさに合わせて調整する（小さい物ほど大きい値）。

---

## 4. 危険地帯（居る間ずっと画面の縁が脈打つ）

**繰り返す演出は、繰り返し自体をシェーダーの時計で作り、トリガーからは「強さ」だけ送る。**
これがこの手の演出のいちばん素直な形になる。

**`assets/shaders/Danger.hlsl`**

```hlsl
Texture2D    gScreen : register(t0);
Texture2D    gDepth  : register(t1);
SamplerState gLinear : register(s0);
SamplerState gPoint  : register(s1);

cbuffer ScreenShaderCB : register(b0)
{
    float4 resolution;
    float4 timeParams;    // x = 経過秒
    float  _Danger;       // @range(0,1)   0=無し / 1=最大。トリガーはここだけ動かす
    float3 _DangerColor;  // @color        脈打つ色（赤など）
    float4 cameraParams;
    float4 uvOffsetScale;
};

float3 SampleScreen(float2 uv)
{
    return gScreen.Sample(gLinear, uv * uvOffsetScale.zw + uvOffsetScale.xy).rgb;
}

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 col = SampleScreen(i.uv);
    float  amt = saturate(_Danger);

    // 脈は【シェーダー側の時計】で作る。トリガーは強さだけ送ればよい。
    float pulse = 0.5 + 0.5 * sin(timeParams.x * 6.0);

    // 画面の縁ほど強く（中心の視界は塞がない）
    float2 d    = (i.uv - 0.5) * float2(timeParams.z, 1.0);
    float  edge = saturate(length(d) * 1.6 - 0.35);

    col = lerp(col, _DangerColor, edge * amt * (0.35 + 0.45 * pulse));
    return float4(col, 1.0);
}
```

**トリガー**: 入ったら強さを上げ、出たら戻す。

```json
"actions": [
  { "when": 0, "type": 12, "target": "MainCamera", "str": "_Danger",
    "num": 0.0, "vec": [1.0, 0.3, 1.0] },
  { "when": 1, "type": 12, "target": "MainCamera", "str": "_Danger",
    "num": 1.0, "vec": [0.0, 0.4, 1.0] }
]
```

`when: 0`（Enter）で 0→1、`when: 1`（Exit）で 1→0。**居る間の脈はシェーダーが勝手に打つ**ので
`Stay` アクションは要らない。

> **`Stay` を使う場合**: `Stay` は毎フレーム発火するが、`AnimShaderParam` は
> **同じ指示のあいだは積み直さず走り続ける**ので、範囲に入っている間に 1 回ぶんのアニメが進む。
> 「入っている間ずっと少しずつ濃くなる」のような一方向の変化に向く。
> 往復する脈を `Stay` で作ろうとすると効かないので、上のようにシェーダー側の時計を使う。

---

## 5. 複数のシェーダーを 1 台のカメラで使いたい

画面シェーダーはカメラに**1 つだけ**しか割り当てられない。まぶしい・危険地帯・被弾を
同時に使いたい場合は、**1 本の .hlsl にまとめて、パラメーターを分ける**。

```hlsl
    float  _Blind;        // @range(0,1)
    float  _Danger;       // @range(0,1)
    float  _Hit;          // @range(0,1)
    float  _reserved;     // 残り 1 個
```

画面シェーダーの自由枠は **float 4 個**（`ScreenShaderCB` の `params` の位置）。
色を持たせたいなら定数として HLSL に直接書くか、枠を使い切らないように配分する。

> メッシュ側（`MeshRenderer`）の自由枠は **float 8 個**。こちらはモデルごとに別々の
> シェーダーを割り当てられるので、この制約はあまり問題にならない。

---

## 6. うまくいかないとき

| 症状 | 見るところ |
|---|---|
| アクションのパラメーター名コンボが空 | 対象にシェーダーが割り当たっていない / シェーダーがまだコンパイルできていない。まず Inspector の Shader 欄にスライダーが出ているか確認する |
| トリガーは動くのに見た目が変わらない | コンソールに「パラメーター "…" が見つかりません。使える名前: …」が出ていないか。名前を手打ちした場合に起きる |
| 割り当てたのに何も起きない | Inspector の Shader 欄の下に**赤い枠**で理由が出る。「書式を見る」で宣言してよいレジスタ一覧が読める |
| 光らせても明るくならない | Post Process のブルームが切れている。値も `1` を超えないと HDR に振り切れない |
| Play を止めたら変な絵で固まった | 進行中の変化は Play 停止で破棄される。それでも残るなら Inspector の値そのものが変わっている（Undo で戻せる） |
| 保存しても項目が増えない | 自由枠の外に宣言している。メッシュは `mvp` / `model` の後ろ、画面は `resolution` / `timeParams` の後ろでないと拾われない |

シェーダーのコンパイルエラーは**エディタのコンソールパネルに赤字**で出る。行番号付き。
ヘッドレスで確認したいときは `DX12Engine.exe --validate <scene.json>`。

---

## 関連

- [`AUTHORING.md`](AUTHORING.md) — Trigger + Action の全アクション一覧、カスタムシェーダーの契約
- [`API_REFERENCE.md`](API_REFERENCE.md) — シーン JSON のフィールド一覧
- [`UI_STYLE_GUIDE.md`](UI_STYLE_GUIDE.md) — UI 側の演出（`tweenUi` / `uifx`）
