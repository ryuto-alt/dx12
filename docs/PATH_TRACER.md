# DXR パストレーサー(地上真値レンダラ)

パリティ基盤 Q1a。「UE に匹敵するグラフィック」の合否判定の基準になる、**エンジン内のパストレーサー**。
UE は再インストールしない(2026-09-30 のユーザー決定)ので、基準は「UE の並べ比較」ではなく「同じシード・同じシーンの地上真値」。
Lumen も同様にパストレーサーとの比較で品質を語る。設計の位置づけは `docs/GRAPHICS_PARITY_DESIGN.md` §3(UE 起動系は不要)。

- 実装: `src/renderer/PathTracer.{h,cpp}`(ディスパッチ・累積・読み戻し)/ `src/renderer/pt/`(専用 BLAS・TLAS の構築 `PtSceneBuilder`、共有レイアウト `PtShared.h`、画像出力 `PtImageIO.h`)/ `shaders/pt/`(`PathTrace.hlsl` = 光輸送、`PtCommon.hlsli` = BRDF・乱数、`PtCopy.hlsl`)
- Application 側の糊: `src/core/ApplicationPathTracer.cpp` + `PathTracerHost.h`(スナップショット・ジョブ・出力)
- MCP: `render_reference` / `render_reference_status` / `render_reference_cancel`(`src/core/mcp/ApplicationMcpPathTracer.cpp`。TS は `tools/mcp-server/toolset/pathTracer.ts`)
- エディタ: ツール > リファレンスレンダー(`src/editor/panels/PathTracerPanel.cpp`)
- 検証: `tests/pt_reference_test.cpp`(GPU vs 解析解 / 独立 CPU リファレンス `tests/pt/PtRef.h`)

**既定 OFF**。要求が来るまで GPU リソースを 1 バイトも確保せず、通常の描画経路には触れない
(PT ジョブの前後で決定論スクショが 1 ビットも変わらないことを確認済み)。

---

## 1. 使い方

### MCP

```
render_reference {spp:1024, bounces:8, size:[1920,1080], output:"C:/tmp/ref/ps1"}       # 今の描画カメラで
render_reference {spp:64, size:[640,360], camera:{position:[0,2,-6], target:[0,1,0], fovDeg:45}}
render_reference_status {}                 # {state, progress{phase,pct,message,etaSec}, samples, gpu{msPerSpp}, output{files[]}, ...}
render_reference_status {preview:true}     # 途中経過を <output>.preview.png へ
render_reference_cancel {save:true}        # そこまでの累積を保存して止める
```

`render_reference` は**即座に返る**(state:"requested")。進捗はポーリング(`render_reference_status`)。TS ツール
(`dx12_render_reference` など)は `waitSec` を付けると完了まで待つ(2 秒ごとに読み、進捗通知も送る)。
`progress` の形は M6 のジョブ API の Progress(`{phase,pct,message,etaSec}`)と同じなので、エンジンメソッドを叩くジョブ種別へそのまま載せられる。

### 引数

| 引数 | 既定 | 意味 |
|---|---|---|
| `spp` | 256 | 画素あたりのサンプル数(目標)。誤差は 1/√N |
| `bounces` | 8 | 散乱頂点の上限。**1 = 直接光のみ / N = 最大 N-1 回の間接バウンス**(ロシアンルーレット付き) |
| `size` | [1920,1080] | 出力解像度。ビューポートの矩形に依存しない |
| `camera` | 今の描画カメラ | `{position,target,fovDeg?,lensRadius?,focusDist?}`。fovDeg は**垂直**FOV。省略すると `m_camera`(エディタ / ゲームの描画カメラ)。正射・2D ビューは未対応 |
| `output` | `<project>/.dx12/pt/render_<日時>` | 拡張子なしの基準パス |
| `seed` | 1 | 乱数シード |
| `maxRadiance` | 0 | 1 サンプルの放射輝度の上限。**0 = クランプ無し**(GT の既定) |
| `frameBudgetMs` | 12 | 1 フレームに PT へ使う GPU 時間の上限。小さいほどエディタが軽い |
| `maxSeconds` | 0 | 実行時間の上限(0 = 無制限)。超えたらそこまでを保存(`truncated:true`) |
| `formats` | ["pfm","png"] | `pfm` / `exr` / `png`。メタ `.json` は常に書く |
| `exposure` | 1 | プレビュー PNG の露出。線形 HDR には掛けない |
| `lightFalloff` | engine | 点/スポットの減衰。`engine` = フォワードと同じ / `physical` = 逆二乗 |
| `sunAngularRadiusDeg` | PCSS 設定 | 太陽の角半径。省略 = PCSS ON なら `lightTanAngle`、OFF ならデルタ光 |
| `russianRoulette` | true | 不偏。false で全経路を bounces まで追う |
| `forceLambert` / `normalMaps` | false / true | 検証用 |
| `quantizeLikeForward` | true | 材質値をフォワードの b2 と同じ 8bit 量子化にする(下記) |
| `background` | true | カメラから見える空(skybox の設定に従う)。false = 黒 |
| `tileSize` / `samplesPerDispatch` | 256 / 1 | 1 ディスパッチの大きさ(TDR 対策の分割単位) |

### 出力

`<base>.pfm`(**線形 float RGB**。比較ツールの入力。行は下から上の標準 PFM)/ `<base>.exr`(無圧縮 float32)/ `<base>.png`(8bit の
簡易プレビュー = 露出 → 簡易 ACES → sRGB。**比較には使わない**)/ `<base>.json`(設定・カメラ・太陽・環境・シーン統計・所要時間・NaN 数)。
値は**シーン参照の線形 HDR**(トーンマップ前・露出前。フォワードの sceneRT と同じ位置)。

---

## 2. アルゴリズム

- **inline RayQuery**(cs_6_6 / バインドレス / 専用ルートシグネチャ)。1 スレッド = 1 画素、1 ディスパッチ = 1 タイル × `samplesPerDispatch` サンプル。
- **累積**: `RWStructuredBuffer<float4>`(rgb = 放射輝度の和 / a = サンプル数。RGBA32F)。ジョブ単位で確保し直す。
- **直接光(NEE)**: 太陽(円盤光 or デルタ)/ 点・スポット(期待寄与に比例して 1 灯を選ぶ 1 パスのリザーバ。灯数上限なし)/ エミッシブ三角形
  (面積 × 輝度で選ぶ Walker alias 表 = O(1)。面光源)/ 環境(コサイン重み方向)。
- **MIS**: エミッシブ面と環境は BSDF サンプリングとの**パワーヒューリスティック**。太陽・点・スポットはデルタ光(MIS なし)。
- **多重バウンス**: `bounces` 個の散乱頂点まで。深さ 3 以降にロシアンルーレット(生存確率 = スループット最大成分を [0.05,0.95] にクランプ。不偏)。
- **BSDF = フォワードの PBR そのもの**(`PtEvalBrdf` は `Lighting.hlsli::ShadePunctual` と `PBR.hlsli` の式の写し):
  GGX NDF + Smith-Schlick-GGX(k=(r+1)²/8)+ Fresnel-Schlick、拡散は `kD·albedo/π`(kD=(1−F)(1−metallic))、
  roughness 下限 0.04、`F0 = lerp(0.04, albedo, metallic)`、エネルギー補償なし・異方性なし。
  重要度サンプリングは「拡散(コサイン)と GGX NDF の半ベクトルの混合」で、混合確率は Fresnel と albedo の比。
  **比較で「アルゴリズム差だけ」を測るため、BRDF は意図的にフォワードに合わせてある**(UE の BRDF には合わせない。Q2 の校正の仕事)。
- **半透明**: `Blend` は**確率的な通過**(確率 = アルファ × 不透明度で当たり、外れたら素通し。期待値はアルファブレンドと同じ)。シャドウレイにも効く。
  `Mask` はアルファテスト(RayQuery の候補ループ内 = any-hit 相当)。どちらもインスタンスの `FORCE_NON_OPAQUE` で候補ループへ入る。
- **屈折(ガラス)**: 未実装(任意項目。v1 は不透明 / マスク / ブレンドまで)。
- **法線マップ**: フォワードの `PerturbNormal`(接空間 z を 0.35 で打ち止め)と同じ。スキンドは接線がバインドポーズのままなので無効。
- **テクスチャ**: アルベド / 法線 / metal-rough(G=roughness, B=metallic)/ emissive をバインドレスで引く(mip0・リニア・wrap)。
  上書きテクスチャ・`.dxmat`・UV スクロール・連番アニメ・色ティントもフォワードと同じ優先順で解決する。
- **面の向き**: 幾何法線は頂点法線の側に揃える(巻き方向に依らない)。反射は両面。**発光は表(法線側)だけ**(`emitBoth` 材質フラグで両面)。
  幾何法線の裏へ出る BSDF サンプル / 影レイは捨てる(粗いメッシュ + 滑らかな法線で面の内側へ漏れないため。端で数 % 暗くなる)。
- **自己交差**: Ray Tracing Gems ch.6 の整数オフセット。

### 専用 TLAS を持つ理由

`RaytracingScene` の TLAS は RT 影 / RT-AO / DDGI 向けの排他ハイブリッドで、半透明・アルファテストを外し、全ジオメトリを OPAQUE で作り、
材質(metallic / roughness / 法線 / MR / emissive / 不透明度)は `GeometryInfo`(16B)に無い。さらにジョブは数分〜数十分続き、その間に共有 BLAS
キャッシュは別の用途で作り直される。そこで**ジョブ開始時点のスナップショット**(BLAS はメッシュ単位で重複排除 + TLAS + 材質・光・エミッシブの表)を専用に持つ。
`RaytracingScene` 本体には触れていない。スキンドは SkinningCompute の変形後頂点をジョブ専用バッファへ写し(`PtCopy.hlsl`)、その写しから BLAS を建てる
(ジョブ中にポーズが変わっても BLAS とシェーダが読む頂点が一致する)。

---

## 3. 光の単位の対応(フォワード ⇔ PT)

| もの | フォワード(`Lighting.hlsli`) | PT | 備考 |
|---|---|---|---|
| 太陽 | `Lo = BRDF · (color·intensity) · N·L`(最初の DirectionalLight 1 個) | 放射照度 `E = color·intensity`。拡散面の放射輝度は `albedo/π · E · cosθ` | π で割らない単位。PCSS ON なら角半径 = `atan(lightTanAngle)` の円盤光 |
| 点 / スポット | `Lo = BRDF · (color·intensity · att) · N·L`、`att = saturate(1−d/range)²`(スポットはコーン² も) | 同じ式(`lightFalloff:"engine"`) | `physical` = `att = 1/d²`(範囲窓なし)。**逆二乗ではない**のがエンジンの単位 |
| 環境(IBL あり) | 拡散 = 放射照度キューブ(コサイン畳み込み = 放射輝度の平均)× `iblIntensity` | 環境キューブの**放射輝度**を `iblIntensity` 倍でサンプル(拡散も鏡面も) | 一様な空なら `albedo · L_sky` で一致 |
| 環境(IBL なし) | `ambientStrength`(DirectionalLight.ambient)を定数の拡散環境光に | **一様な空の放射輝度 = ambient**(遮蔽される) | フォワードは遮蔽なし(SSAO / DDGI 次第)。差は AO と GI |
| 空の見た目 | skybox(`skyboxIntensity`、`drawSkybox` OFF なら sceneRT のクリア色 = 黒) | 同じ | `background:false` で黒 |
| 発光 | `color·intensity · (emissiveTexture)` をライティングの後に加算(照らさない) | 同じ値を**放射輝度**として持つ(他の面も照らす = 面光源) | b2 の 8bit 量子化(色 8bit・強度は二乗曲線 8bit)を再現(`quantizeLikeForward`)。強度 1 → 1.0087 |
| 不透明度 / カットオフ / 色ティント | b2 の 8bit 量子化 | 同じ(`quantizeLikeForward`) | false で元の float |
| 露出・トーンマップ | uber パスで適用 | **なし**(線形出力) | フォワードの sceneRT と同じ位置 |

これらは Q2(光の物理単位の校正)への申し送りでもある: UE の lux / cd との変換は `E`(太陽)と `color·intensity`(点)へ掛ける定数として導ける。

---

## 4. 再現性(決定論)

- 乱数 = PCG ハッシュ(画素 × サンプル番号 × シード)。サンプル番号ごとに独立なので、**タイル分割・サンプル分割・フレーム予算に依らない**。
- 同じ GPU・同じドライバ・同じ設定 → **ビット一致**(同一設定の再実行、フレーム予算 5ms と 40ms のどちらも PFM の MD5 が一致することを確認)。
  異なる GPU / ドライバ間のビット一致は保証しない(浮動小数点の最適化の違い)。テストの許容は分割違いで最大 1e-6 台。
- 累積は 32bit float の和(サンプル数が数万を超えると加算誤差が出る。画素あたり 1e5 spp 以下を推奨)。
- ジョブ開始時のスナップショットで固定される(ジョブ中にシーンを編集しても絵は変わらない。シーン切替 / メッシュ解放は中止する)。

---

## 5. 制約(v1)

- **仮想ジオメトリ(VG)のシーンは、プロキシ(低ポリの `MeshRenderer`)でトレースされる**。全 LOD0 の BLAS は別課題(`VirtualGeometry` を持つインスタンスは `scene.vgProxyInstances` に数える)。
- **カスタムシェーダ材質 / スプラット地形 / マテリアルグラフ材質は標準の PBR で近似**(`customShaderInstances` / `splatTerrainInstances` に数える)。
- **スキンド**: ジョブ開始時のポーズで固定。法線は「バインドポーズの補間法線を面法線の回転で追従させた近似」、法線マップは無効。
- **半透明**は確率的通過(屈折なし)。パーティクル / スプライト / デカール / フォグ / ボリュームは含まない。
- **環境の NEE はコサイン重み方向**(重要度サンプリングされた HDRI ではない)。小さく非常に明るい太陽を含む HDRI は分散が大きい → 太陽は DirectionalLight で表現する。
- 正射カメラ・2D ビューは未対応。被写界深度は薄レンズ(`lensRadius`)のみ、モーションブラーなし。
- 解像度 × 16B の累積バッファ(1080p で 33MB)+ BLAS/TLAS(シーンによる。Sponza 26 万三角形で数十 MB)。
- ジョブは 1 つずつ。別のジョブ実行中の要求は `E_BUSY` 相当のエラー。

---

## 6. 検証(合否基準の数値。`ctest -R PathTracerGpuTests`)

GPU が無い / DXR 1.1・SM 6.6・Tier 3 が無い環境では SKIP(0 で終わる)。RTX 5060 の実測:

| 検証 | 期待 | 結果 |
|---|---|---|
| (a) 白い炉(ランバート albedo=1 の球 + 一様な空 L=1) | 放射輝度 = 1 | 球の平均 0.99988、背景 1.000000、NaN 0 |
| (d) GGX 白い炉(metallic=1, albedo=1。単一散乱の方向アルベド E(μ=1)) | 数値積分と一致 | roughness 0.15/0.3/0.5/0.7/1.0 で **E = 0.9966/0.9885/0.865/0.640/0.307**(数値積分に対して ±1.3% 以内)。**エネルギー損失 1−E = 0.14%/2.4%/14%/36%/69%**(マルチスキャッタ補償が無い既知の欠損。UE との差の主要因の 1 つ) |
| (c) 点光源 + ランバート平面(閉形式) | `albedo/π · color · att · cosθ` | 最大相対誤差 0.29%(エンジン式)/ 0.57%(逆二乗) |
| 太陽 + 一様な空 + ランバート平面 | `albedo/π·E·cosθ + albedo·sky` | 完全一致(小数 5 桁) |
| 閉じた発光箱(全面 ρ=0.5・Le=1) | `L(B) = Le(1−ρ^(B+1))/(1−ρ)` | B=1/2/4/8: 誤差 −0.04%〜−0.03%、B=24(RR): +0.08%(エミッシブ NEE + MIS + RR + 多重バウンスの総合検査) |
| 半透明 Blend α=0.5 / 0.25、Mask α<cutoff / ≥cutoff | 通過確率 × 発光 | 1.0026(期待 1.0)/ 1.5014(1.5)/ 2.0000 / 0.0000 |
| (b) コーナーボックス(面光源 + 間接光 + 光沢球)vs 独立 CPU リファレンス(48×48・1024spp) | 一致 | 全体平均比 **1.0007**、RGB 比 1.0006〜1.0008、8×8 ブロック RMSE 相対 **1.4%** |
| 太陽 + 点 2 + スポット + 一様な空 + 粗い/光沢球 vs CPU | 一致 | 全体平均比 **0.9998**、ブロック RMSE 相対 0.8% |
| 収束(同じ spp で独立 2 枚の差から標準偏差) | 傾き −0.5 | **−0.525**(16〜4096 spp) |
| 決定論 | 同じ設定 → 同じ結果 | 最大差 **0**(ビット一致)/ 分割違い 9.5e-7 |

CPU リファレンス(`tests/pt/PtRef.h`)は GPU 版と**別に書いた**倍精度・BVH 付きの実装で、サンプリング戦略を意図的に変えてある
(点光源は全灯明示評価 / エミッシブは CDF + 二分探索 / 環境は球面一様 / 乱数は mt19937_64)。BRDF の式・MIS・bounces の意味・アルファ / 発光 / 両面の規則だけを共有する。
CPU 側は論理コア/4 スレッド。`DX12E_PT_TEST_QUICK=1` で重い比較を縮小、`DX12E_PT_TEST_DUMP=<dir>` で画像(PFM/PNG)を書き出す。

### フォワードとの整合(エンジン内。ptdirect 系の使い捨てシーン)

同じシーン・同じカメラで、フォワード(`screenshot`、tonemapper=2 で `pow(x,1/2.2)` を逆変換して線形化)と PT を比較:

| シーン | PT 設定 | 全体平均比(PT/フォワード) | 16×16 ブロック比の中央値 / p5 / p95 |
|---|---|---|---|
| 太陽 + 点 2(影つき)、ambient=0、IBL なし = 直接光だけ | bounces=1 | **1.010** | 1.0015 / 0.980 / 1.010 |
| 太陽 + 既定の手続き空(IBL)+ 球 3 個 | bounces=8 | **1.005** | 0.9998 / 0.940 / 1.149 |

**大きな系統誤差は無い**(光の単位・BRDF が揃っている証拠)。残る差の要因: ①フォワードの GI は SSGI / DDGI / SSAO(既定 OFF)で、PT は真の間接光と遮蔽
(球の下・床の隅が PT の方が暗く、白い壁の近くが PT の方が明るい)②フォワードの鏡面は SSR(OFF)+ IBL で、PT は真の映り込み(金属球に床が映る)
③影の縁(CSM の PCF とバイアス / PCSS OFF なら PT の影は太陽が点光源の硬い影)④エディタのグリッド・軸線(フォワードの絵にだけ描かれる)
⑤スペキュラのピーク(フォワードは 8bit / fp16 の丸め)。

### 性能(RTX 5060、1 spp あたりの GPU 時間。`gpu.msPerSpp`)

| シーン | 解像度 | bounces | ms/spp |
|---|---|---|---|
| コーナーボックス(約 100 三角形) | 1080p | 1 / 6 | 1.2 / 3.0 |
| Sponza(26 万三角形・103 インスタンス・マスクの葉あり) | 1080p | 1 / 6 | **4.9 / 13.6** |

1080p / 1024spp / 6 bounces の Sponza は約 14 秒(全速)。既定のフレーム予算 12ms では、エディタを約 40fps に保ったまま約 30 秒で終わる。
1 ディスパッチ = 256×256 タイル × 1 サンプル(Sponza で 1 ms 台)なので TDR には遠い。

---

## 7. Q1b(比較ツール)/ Q2(校正)への申し送り

- 比較の入力は `<base>.pfm`(線形 float)。HDR-FLIP / LDR-FLIP / SSIM / ΔE2000 はこれとフォワードの線形 float スクショ(Q2 で新設予定)で取る。
  当面のフォワード側は `screenshot`(tonemapper=2)を逆ガンマして使える(上の整合表の方法。8bit なので暗部の精度は低い)。
- しきい値は「PT 同士のノイズ床の 1.5 倍以上」で決める: 同じシーンを `seed` を変えて 2 枚描き、その差がノイズ床(`spp` を増やせば 1/√N で下がる)。
- 校正(Q2)の入口: ①太陽 `E`・点 `color·intensity` の単位 ②BRDF の差(UE はマルチスキャッタ補償あり = 上表の 1−E)③エミッシブ(強度の 8bit 量子化)④露出。
- スキンドのポーズ / パーティクル / フォグなど PT に含まれないものは、比較シーンから外すか凍結モードにする。

## 7b. 大気の空(A1 / 物理ベース大気)

シーン設定 `atmosphere.enabled`(`docs/ATMOSPHERE.md`)が ON のとき、PT の環境は**フォワードと同じ大気の LUT**から引く(従来の環境キューブ / 一様な空は使わない)。

- **別 .cso**: `PathTraceAtmos_CS.cso`(`-D PT_ATMOSPHERE=1`)。従来の `PathTrace_CS.cso` は DXIL が 1 ビットも変わらない(大気 OFF の PT 出力は変更前後で MD5 一致を実測)。
  ジョブが大気を使うときだけ `PathTracer` がこちらの PSO を選ぶ(`JobDesc.env.atmosSrvBase` ≠ `kNoIndex`。定数 `pt::kFlagAtmosphere` = 128、`JobConstants.atmosSrvBase`)。
- **環境**: ライティング(環境 NEE・BSDF ヒット)= `AtEnvRadiance(dir) × iblIntensity`(太陽円盤なし=太陽は DirectionalLight が担当)。
  一次レイのミス(背景)= `SkyRadiance(dir)`(Sky-View LUT + 太陽円盤 + 月 + 星 + 夜空の下限。`skyLuminanceScale × skyboxIntensity` を含む。`background:false` / `drawSkybox:false` なら 0)。
  `AtmosphereCommon.hlsli` を `AT_BINDLESS` で読み、LUT は `ResourceDescriptorHeap[atmosSrvBase + 0..4]`、パラメータは `[+6]` の ByteAddressBuffer(カーネル先頭で `AtLoadParams()`)。
- **エアリアルパースペクティブ**: 一次レイがジオメトリに当たった距離で AP LUT を引き、`L = L·Tp + Lp·E·apStrength`(フォワードの `PSAp` と同じ式・`apStartDepth` 以遠)。**一次レイのみ**(二次以降のバウンスには掛けない)。空(ミス)には掛けない。
- **太陽**: `DirectionalLight`(大気が向き・色・強度を毎フレーム駆動)。角半径は `sunAngularRadius`(`sunAngularRadiusDeg` 未指定のとき)。
- **制約**: LUT は**エンジンが直近のフレームで作ったもの**(PT は `PrepareFrame` で走り `AtmosphereRecordFrame` より前 = 前フレームの LUT)。ジョブの間に時刻・カメラを動かすと絵が食い違う(パリティ撮影は固定カメラ・時刻)。
  AP LUT はエンジンカメラの froxel なので、`camera` 引数でエンジンと違うカメラを指定した PT は AP がずれる。環境 NEE は従来どおりコサイン重み(明るい太陽のアウレオールは分散が出る)。
  空の比較は同じ LUT を引くので LUT 自体の精度は検証できない(LUT の精度は `AtmosphereGpuTests` が CPU の数値積分と突き合わせる)。PT は独立な地面の照明(環境 NEE・遮蔽・多重バウンス)を持つので、地面の IBL・AP の当て方は独立に検証される。
- パリティシーン: `tools/parity/scenes/ps2_sky_h06|h09|h12|h17|h19.json`(生成器 `tools/parity/gen/ps2_sky.py`。`--emit-specs` で仕様とマスクを書き出す)。

---

## 8. 罠(実際に踏んだもの)

- **`small` / `large` は Windows ヘッダのマクロ**(`rpcndr.h`)。変数名に使うとコンパイルエラーになる。
- **`RtBindless.hlsli` は使えない**(GeometryInfo が 16B でテクスチャはアルベドのみ)。専用の `PtInstance` / `PtMaterial` を引く。
- **接線が法線と平行 / ゼロの頂点(Sponza)で `normalize` が NaN** → 基底を作り直す(NaN サンプルは 0 にして `nanSamples` で数える)。
- **`WritePreviewPng` などのインライン画像 IO を MCP ハンドラのラムダから呼ぶと MSVC(VS 18)が内部エラー(C1001)** → `PtHost::WritePreview` に出した。
- 「同じ設定なのに結果が違う」は、環境 NEE の有無で乱数の消費順が変わるため(黒い環境なら NEE を省く)。設定を揃えること。
- スキンドは SkinningCompute の出力が**フレームごとに書き換わる**ので、そのまま BLAS の入力にするとジョブ中に頂点と BLAS がずれる → 専用バッファへ写す。
