# CLAUDE.md - DX12 Engine Project

## 応答スタイル
- **標準語の日本語**で応答すること。関西弁・方言は使わない
- 簡潔に、落ち着いた説明で。過度なキャラ付けや砕けすぎた語尾は不要

## 専用エージェント・スキルのクイック呼び出し

ユーザーが「専用エージェント使って」「エージェントで」「スキルで」と言ったら、タスク内容に応じて以下を即座に呼び出す：

### エージェント（Agent tool）
| トリガーワード | subagent_type | 用途 |
|---|---|---|
| 「設計して」「アーキテクチャ」 | `feature-dev:code-architect` | 機能設計・アーキテクチャ設計 |
| 「レビューして」「コードレビュー」 | `feature-dev:code-reviewer` | コードレビュー |
| 「コード調べて」「解析して」 | `feature-dev:code-explorer` | コードベース調査・解析 |
| 「調べて」「検索して」「探して」 | `Explore` | コードベース探索 |
| 「計画して」「プラン」 | `Plan` | 実装計画の策定 |

### スキル（Skill tool）
| トリガーワード | skill名 | 用途 |
|---|---|---|
| 「コミットして」 | `commit` | gitコミット作成 |
| 「PR作って」「プルリク」 | `commit-push-pr` | コミット→プッシュ→PR作成 |
| 「レビューして」(PR) | `code-review` | PRコードレビュー |
| 「機能開発」「フィーチャー」 | `feature-dev` | ガイド付き機能開発 |
| 「シンプルにして」「整理して」 | `simplify` | コード品質改善 |

### 判断基準
- 迷ったら聞かずにまず最適なエージェントを起動する
- 複数のエージェントが並行で動かせる場合は並行起動する
- 「全部やって」と言われたら、architect → 実装 → reviewer の順で進める

---

## プロジェクト概要
- **DirectX 12 ゲームエンジン + Unity ライクなエディタ**（C++20）
- ECS (entt) ベース。エディタでシーン構築 → Lua スクリプトでゲームロジック → Play/Stop で即実行
- ターゲット: 3Dアクション/FPS
- 最終目標: PBR + DXR レイトレーシング
- GitHub: https://github.com/ryuto-alt/dx12

## データ駆動オーサリング（人間 & Claude Code 両対応）
ゲームの中身は全部データ（シーン JSON + `.lua` コンポーネント）。同じものを人間はエディタで、
Claude Code はテキストで作れる。新機能の作法はここを参照:
- **[`docs/AUTHORING.md`](docs/AUTHORING.md)** … 配置エフェクト `ParticleEmitter` / イベント `Trigger`+Action /
  エンティティ参照プロパティ(`type="entity"`) / イベントバス `events` / ヘッドレス検証 `--validate` /
  カスタムシェーダー(`assets/shaders/*.hlsl`、保存で自動ホットリロード) /
  地形(`.hf`)・スカルプト(`.smsh`)の作り方と当たり判定
- [`docs/SCRIPT_COMPONENTS.md`](docs/SCRIPT_COMPONENTS.md) … プロパティ付き Lua コンポーネント / プレハブ
- **[`docs/UI_STYLE_GUIDE.md`](docs/UI_STYLE_GUIDE.md)** … ゲームUIを作るとき必読。ジャンル別デザイン語彙(形/色/字/動き)→
  エンジン機能の対応表・モーション相場・アンチパターン(実在ゲームのリサーチベース)
- 検証: `DX12Engine.exe --validate <scene.json>`（参照切れ・スクリプト不在をヘッドレスで報告。終了コード 0/1）
- **超詳細診断**: `DX12Engine.exe --ui-tests-deep --project <dir>`（またはエディタの `ツール > エンジン診断 > 🔬 超詳細診断`）
  「UI は動くけど絵が間違っている」を拾う担当（UI 自動テストは絵を一切見ない）。
  実体は `src/gui/DeepDiagnostics.{h,cpp}`。検査は 14 種（`DeepDiag::AllCheckIds()` の順）:
  | ID | 見るもの |
  |---|---|
  | `shaders` | `.cso` の存在・破損・`.hlsl` より古くないか（ビルドし忘れ） |
  | `textures` | 読めない/サイズ0/ミップ無し、法線・metalRoughness が sRGB 保存＝ライティングが狂う |
  | `models` | メッシュ0・頂点0・AABB が NaN・ボーン 256 超（SkinningBuffer が無言で切り捨てる） |
  | `gamma` | バックバッファが `_SRGB` ＝ガンマ二重適用（全体が白っぽい） |
  | `scene_assets` | パスは合っているのに `meshes` が空＝画面に出ない / **アセット参照切れ**（モデル・テクスチャ・マテリアル・シェーダに加えて、音声・UI画像・フォント・ボタン効果音・パーティクル画像・.uianim・.spranim・.animfsm・.prefab・環境マップ・LUT・デカールアトラス） |
  | `lighting` | 灯数の上限超過（**合計 1024 灯**／クラスタ 1 マス **128 灯**。超過分は**無言で**描画されない）・DirectionalLight が 0/2 個以上・影スロット超過（spot 4 / point 2 は据え置き）・強度0/range0 の効かないライト・コーン角の内外逆転・IBL 無しの金属 |
  | `terrain` | `.hf` の存在/ヘッダ整合/サイズ、コンポーネントとの解像度食い違い、4 の倍数（Jolt の要求）、コライダー用 RigidBody の有無、**複数の地形が同じ `.hf` / `.splat` を共有していないか**、レイヤーセット(`.terrainlayers`)の参照切れ |
  | `picking` | CPU 頂点キャッシュ無し（AABB 判定に落ちる）・スケール0/NaN の Transform（レイが素通り）・原点から極端にズレたジオメトリ |
  | `instancing` | `render_instancing` 設定・適格ドローの割合・**不適格の支配的な理由ランキング** |
  | `scripts` | `.lua` の `end`/`until`/括弧/文字列の閉じ忘れ（字句だけ追う。式の文法は見ない） |
  | `dxr` | GPU の DXR Tier / シェーダモデル・レイトレーシングのゲート通過状況・TLAS のインスタンス数と加速構造の VRAM・**TLAS に入らなかったもの**（スキンド / 半透明 / 上限超過）。「RT 影を ON にしたのにキャラの影が変わらない」の答えはここ |
  | `render_health` | **「シーンビューが真っ暗 / カメラが何も映らない」の原因を名指しする**。render_debug の出しっぱなし・露出0・ティント黒・自動露出レンジの逆転・光源ゼロ・シーン矩形の潰れ・SRV ヒープ枯渇（枯渇**前**に警告）・カメラの NaN や極端な座標・MCP のカメラ乗っ取り残り |
  | `hiz_occlusion` | Hi-Z オクルージョンカリングが**「ON にしたのに効いていない」「ON にしたせいで遅くなっている」**を名指しする。★本命は「プリパスを要求しているのがオクルージョンだけ」＝TAA/SSAO/SSR/DXR がどれも無効な状態で ON にしていて、プリパスぶんの描画コールが増えて損をしているケース。ほかに遮蔽率ほぼ0 / 述語を1本も張れていない（全部インスタンシングのバッチ）/ 正射・2Dビューで自動無効 |
  - `DeepDiag::RunAll(app, only)` が全部まとめて**機械可読な JSON** を返す（`version`/`engine`/`checks[]`/`summary`）。
    `only` はカンマ区切りの検査 ID で絞り込み（例 `"lighting,terrain,picking"`）。
    失敗判定は `summary.errors > 0` だけ見ればよい（注意/情報は赤くしない方針）
  - 呼ぶ側の注意: メインスレッドから呼ぶ / `instancing` は 1 フレーム描画した後でないと測れない /
    `textures` `models` は assets 全 Probe なので数十秒かかることがある（`only` で外す）

## ビルド方法
```bash
# 前提: vcpkg がインストール済み（VCPKG_ROOT 環境変数）
# Visual Studio 2026 (v18) + Windows SDK 10.0.26100.0

# CMake configure（Visual Studio ジェネレータ）
cmake -B build/debug -G "Visual Studio 18 2026" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows

# ビルド
cmake --build build/debug --config Debug

# 実行
build/debug/Debug/DX12Engine.exe
```

### プロファイリング（Tracy）
CPU のどこで時間を食っているかを**フレーム単位の時系列**で見る。
`dx12_perf_stats` は「N フレーム平均を 8 スロットに畳んだ数値」で、Update の中身
（Lua / 物理 / アニメ / パーティクル / オーディオ）が 1 個に潰れているため、
内訳を見るにはこちらが要る。

```bash
cmake --preset windows-tracy      # 別ディレクトリ build/tracy へ建てる
cmake --build build/tracy
build/tracy/DX12Engine.exe
# Tracy.exe（公式リリースの prebuilt）を起動して Connect
```

- **既定は OFF**。`cmake --preset windows-release` には Tracy のコードは 1 バイトも入らない
- vcpkg の `profiling` フィーチャ経由。通常ビルドでは tracy をダウンロードすらしない
- `default-features:false` で **crash-handler フィーチャを切っている**。Tracy の
  crash handler は VEH を張り SEH より先に走るので、有効にすると `CrashHandler.cpp` の
  `SetUnhandledExceptionFilter` がクラッシュレポートを奪われる
- `on-demand` フィーチャ有効＝プロファイラを繋ぐまでのオーバーヘッドはほぼゼロ
- `BuildGame` は exe 隣の DLL を無条件コピーするので、`tracyclient.dll` は
  `kDllExcludeList`（`ApplicationProject.cpp`）で配布物から除外している
- ゾーンは `src/core/Profiler.h` の `DX12_PROFILE_ZONE_N()`。既存の `CpuScopeTimer` は
  消さず**同じ行に並べる**（前者は AI が読む数値、後者は人間が読むタイムライン）

### vcpkg セットアップ（初回のみ）
```bash
cd ~
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.bat
# VCPKG_ROOT を ~/vcpkg に設定
```

### 注意事項
- assimp は FetchContent で取り込み（vcpkg の pugixml が CMake 4.2 の tar バグで失敗するため）
- `/utf-8` 等のコンパイルオプションはグローバルではなくターゲット別に適用（FetchContent との競合回避）
- SHADER_DIR / ASSETS_DIR マクロは CMake から Core ターゲットに渡される（絶対パス）
- PIX パス: `build/debug/Debug/DX12Engine.exe`、Working Dir: プロジェクトルート

---

## アーキテクチャ

### ディレクトリ構造
```
dx12/
├── CMakeLists.txt          # ルートCMake + DXCシェーダーコンパイル
├── CMakePresets.json        # Ninja/MSVC プリセット
├── vcpkg.json               # 依存パッケージ（spdlog, directx-headers, D3D12MA, DirectXTex, ImGui）
├── src/
│   ├── main.cpp             # WinMain エントリポイント
│   ├── core/                # アプリ基盤
│   │   ├── Application.h       # クラス定義（実装は下記の TU 群へ分割済み）
│   │   ├── ApplicationInternal.h/cpp  # 分割の全体像 + TU 共有ヘルパ(dx12e::appdetail)
│   │   ├── Application.cpp     # ctor / Initialize / Run / Update / Shutdown
│   │   ├── ApplicationPipeline.cpp  # PSO再生成、レンダー解像度、PSO/SRVキャッシュ
│   │   ├── ApplicationRender.cpp    # Render() と描画の下請け
│   │   ├── ApplicationScene.cpp     # シーンロード、Play⇔Editor 遷移、永続化
│   │   ├── ApplicationProject.cpp   # プロジェクト、バージョン管理、ゲームビルド
│   │   ├── mcp/ApplicationMcp*.cpp  # MCP ディスパッチ表（テーマ別 8 ファイル）
│   │   ├── Window.h/cpp       # Win32ウィンドウ、F11フルスクリーン
│   │   ├── Logger.h/cpp       # spdlog ラッパー
│   │   ├── GameClock.h/cpp    # delta time、FPS
│   │   ├── Assert.h           # ThrowIfFailed、DX_ASSERT
│   │   └── Types.h            # u8/u16/u32/u64/f32/f64 alias
│   ├── graphics/            # DX12 ラッパー（描画知識ゼロ）
│   │   ├── GraphicsDevice.h/cpp    # ID3D12Device5 + D3D12MA + DXR検出
│   │   ├── CommandQueue.h/cpp      # Fence同期
│   │   ├── SwapChain.h/cpp         # トリプルバッファ FLIP_DISCARD
│   │   ├── DescriptorHeap.h/cpp    # RTV/DSV/CBV_SRV_UAV ヒープ
│   │   ├── FrameResources.h/cpp    # フレーム多重化アロケーター
│   │   ├── GpuResource.h/cpp       # D3D12MA RAII基底
│   │   ├── Buffer.h/cpp            # Vertex/Index/Constant Buffer
│   │   ├── Texture.h/cpp           # 2Dテクスチャ + SRV
│   │   ├── RootSignature.h/cpp     # 4スロット: b0(MVP+Model), b1(PerFrame), t0(Texture), t1(Bones)
│   │   ├── PipelineState.h/cpp     # Builder パターン PSO
│   │   └── CommandList.h/cpp       # 薄いファサード
│   ├── renderer/            # 描画ロジック
│   │   ├── Mesh.h/cpp       # Vertex(pos/norm/color/uv/boneIdx/boneWgt), InputLayout
│   │   ├── Camera.h/cpp     # FPSカメラ（yaw/pitch、WASD移動）
│   │   └── Material.h/cpp   # albedoTexture 参照
│   ├── animation/           # スケルタルアニメーション
│   │   ├── Skeleton.h/cpp        # ボーン階層、inverseBindPose
│   │   ├── AnimationClip.h/cpp   # キーフレーム（pos/rot/scale）
│   │   ├── Animator.h/cpp        # 補間 + グローバル行列 + ブレンド(CrossFadeTo)
│   │   └── SkinningBuffer.h/cpp  # StructuredBuffer<float4x4> GPU転送
│   ├── resource/            # アセット管理
│   │   ├── ShaderCompiler.h/cpp    # .cso 読み込み
│   │   ├── TextureLoader.h/cpp     # DirectXTex (WIC/DDS)
│   │   ├── ModelLoader.h/cpp       # Assimp (glTF/OBJ) + ボーン/アニメ抽出
│   │   └── ResourceManager.h/cpp   # テクスチャキャッシュ + デフォルト白テクスチャ
│   ├── input/               # 入力システム
│   │   └── InputSystem.h/cpp  # Raw Input マウス + キーボード
│   └── gui/                 # デバッグUI
│       └── ImGuiManager.h/cpp  # ImGui DX12バックエンド統合
├── shaders/
│   └── forward/
│       ├── Forward.hlsl          # ランバートライティング（スタティックメッシュ用）
│       └── ForwardSkinned.hlsl   # GPU Skinning + ランバート（スケルタル用）
└── assets/
    └── models/human/
        ├── walk.gltf/bin     # 歩行アニメーション（Mixamoリグ）
        ├── sneakWalk.gltf/bin # スニーク歩行アニメーション
        └── white.png          # デフォルトテクスチャ
```

### モジュール依存関係
```
DX12Engine → Core, Graphics, Renderer, Animation, Resource, Input, Gui
Resource → Core, Graphics, Animation, assimp(FetchContent), DirectXTex
Animation → Graphics, DirectXMath
Renderer → Graphics, DirectXMath
Gui → Graphics, imgui::imgui
Input → Core
Graphics → Core, directx-headers, D3D12MA
Core → spdlog
```

### RootSignature レイアウト
初期実装(Phase 3A)時点の4スロット構成は下記の通りやったけど、**現在は Cook-Torrance PBR
(法線マップ/metallic-roughness/IBL/CSM/SSAO込み)の9スロット構成**に拡張済み。詳細は
`src/graphics/RootSignature.h/cpp`(`kSlotXxx` 定数群)を参照。
```
Slot 0: RootConstants b0 (32 DWORD = MVP + Model行列) - ALL可視
Slot 1: CBV b1 (PerFrame: view/proj/lightDir/time/lightColor/ambient) - ALL可視
Slot 2: DescriptorTable SRV t0 (Albedoテクスチャ) - PIXEL可視
Slot 3: DescriptorTable SRV t1 (ボーン行列 StructuredBuffer) - VERTEX可視
Static Sampler s0: LINEAR WRAP - PIXEL可視
```

### 描画フロー
```
Application::Render()
  ├─ FrameResources::BeginFrame (GPU同期 + コマンドリストリセット)
  ├─ バリア PRESENT → RENDER_TARGET
  ├─ ClearRTV + ClearDSV
  ├─ SetRootSignature + SetPipelineState (Skinned or Static)
  ├─ SetDescriptorHeap (ShaderVisible SRV)
  ├─ PerFrame CB 更新 (view/proj/light)
  ├─ SkinningBuffer 更新 (ボーン行列)
  ├─ for each Mesh: SetPerObject(MVP+Model) + SetSRV(texture) + DrawIndexedInstanced
  ├─ ImGui BeginFrame → UI描画 → EndFrame
  ├─ バリア RENDER_TARGET → PRESENT
  └─ ExecuteCommandList → Present → EndFrame
```

---

## コーディング規約
- C++20、`namespace dx12e` で全クラスを囲む
- `ComPtr<T>` でCOM管理
- `PascalCase`(型/メソッド)、`camelCase`(ローカル変数)、`m_`プレフィックス(メンバ)
- HRESULT は必ず `ThrowIfFailed` でチェック
- `#pragma once` でインクルードガード
- `<directx/d3d12.h>` を使用（Windows SDK の `<d3d12.h>` ではない）
- `DirectXMath` 使用、`using namespace DirectX` は `.cpp` 内のみ
- `/W4 /WX` でビルド。未使用パラメータは `/*param*/` で抑制
- `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `UNICODE`, `_UNICODE` は CMake でターゲット別定義
- ヘッダーに `#define WIN32_LEAN_AND_MEAN` 等は書かない
- Logger は `char*` ベース（`wchar_t` リテラル `L"..."` は使えない）
- D3D12MA でGPUリソース確保
- D3D12 Debug Layer + GPU-Based Validation をデバッグビルドで有効化

---

## 完了済みフェーズ

| # | フェーズ | 内容 | コミット |
|---|---------|------|---------|
| 1 | Phase 1 | DX12基盤 + Win32ウィンドウ + クリア描画 | `9f3f163` |
| 2 | Phase 2 | メッシュ描画 (PSO, RootSig, 回転カラーBox) | `1a509c3` |
| 3 | Phase 3A | テクスチャ + Assimpモデル読み込み + ランバートライティング | `8e8eb3f` |
| 4 | Skeletal | スケルタルアニメーション再生 (GPU Skinning in VS) | `798b947` |
| 5 | Blend+ImGui | アニメーションブレンド(CrossFade) + ImGui UI | `f6b3d62` |
| 6 | Fullscreen | F11ボーダレスフルスクリーン + リサイズ対応 | `67c120e` |
| 7 | FPSCamera | WASD+マウスFPSカメラ + Raw Input + InputSystem | `902b7cd` |
| 8 | Editor基盤 | Hierarchy/Inspector/AssetBrowser/ギズモ/Play-Stop/Lua アタッチ | (多数) |
| 9 | Editor実用化 | ディープ複製/削除Undo完全復元/コライダー編集/Add Component/親子階層のワールド変換反映/マルチ選択移動 | `0889516`〜`ebfc50f` |

## 次のステップ候補（優先度順）

| 優先度 | 内容 | 詳細 |
|--------|------|------|
| **B** | PBR Deferred Rendering | GBuffer パス、Deferred Lighting (Compute)、PBR BRDF |
| **C** | 物理エンジン (Jolt) | 剛体、コリジョン、重力 |
| **D** | シャドウマップ (CSM) | Cascaded Shadow Maps |
| **E** | DXR レイトレーシング | BLAS/TLAS、レイトレ影・反射 |
| **F** | IBL + 環境マップ | スカイボックス、プリフィルタ EnvMap、BRDF LUT |

---

## 既知の技術的注意点

### ビルド関連
- **CMake 4.2 の tar バグ**: vcpkg の pugixml が展開できない → assimp は FetchContent で入れてる
- **assimp static リンク**: `BUILD_SHARED_LIBS=OFF` で DLL 依存を回避
- **コンパイルオプション**: `add_compile_options` ではなく `target_compile_options` でターゲット別適用（FetchContent の assimp との `/utf-8` 競合回避）

### DX12 関連
- **aiMatrix4x4 → XMFLOAT4X4 変換は転置が必要**: Assimp は translation を最後の列に、DirectXMath は最後の行に格納
- **アニメーション時間**: Assimp のキーフレームは ticks 単位。`deltaTime * ticksPerSecond` で変換
- **aiQuaternion (w,x,y,z) → XMFLOAT4 (x,y,z,w)**: 順序リマップ必須
- **SwapChain::Resize**: RTVハンドルは初回 Allocate で確保したものを再利用（Resize時に再Allocateしない）
- **DescriptorHeap**: `AllocateIndex()` でインデックスを取得 → `GetCpuHandle(idx)` / `GetGpuHandle(idx)` でハンドル取得
- **デフォルト白テクスチャ**: テクスチャ無しメッシュで SRV slot 未初期化エラーを防ぐ
- **unique_ptr + 前方宣言**: コンストラクタ/デストラクタを `.cpp` で定義しないと incomplete type エラー

### Hi-Z オクルージョンカリング（既定 OFF）
壁の裏に完全に隠れた描画を GPU 側で落とす。`settings.json` の `"render_occlusion_culling"` /
MCP `dx12_set_occlusion`。実装は `src/renderer/HiZPass.{h,cpp}`（深度ピラミッド）+
`OcclusionCullPass.{h,cpp}`（可視性判定）+ `HiZMath.h`（純関数・`tests/hiz_math_test.cpp` で検証）。

- **深度規約は標準 Z（0=near / 1=far）**。根拠は `Camera.cpp` の `XMMatrixPerspectiveFovLH` 素通し /
  DSV クリア値が全箇所 `1.0f` / 深度比較が `LESS` 系のみ（`GREATER` 系はリポジトリに 0 件）。
  したがって**ピラミッドは max 縮約**、遮蔽判定は `箱の最近点 > タイルの最遠面`。
  リバース Z へ移行するなら `HiZMath.h` と `shaders/hiz/*.hlsl` の両方を直すこと（テストが落ちて気付ける）
- **`shaders/hiz/HiZCull.hlsl` と `src/renderer/HiZMath.h` は式が一対一で対応している。片方だけ
  直すと誤カリングになる**（両ファイルの先頭に対応表がある）
- **2 フェーズ方式は使っていない**。このエンジンは深度プリパスが前方パスとビット厳密に一致する
  （同じ `m_drawItems` / 同じジッタ付き `camVPJ` / 同じ LOD）ので、プリパス後にピラミッドを建てれば
  同一フレーム・同一カメラの遮蔽情報になる。2 フェーズは遮蔽物パスを別途払いたくないための技法で、
  ここでは既に払っている
- 判定結果は **D3D12 のプレディケーション**へ直接渡す（クエリも読み戻しも不要）。ルートシグネチャは
  1 DWORD も増えない（既に 61/64 使用済みなので `ExecuteIndirect` 化は非現実的だった）
- **インスタンシングされた描画はバッチの合成 AABB で判定する**。バッチは 1 ドローコールなので
  述語もバッチ単位でしか張れない。区間は `BuildDrawList` のソート直後に `m_drawBatches` へ確定する
- ★**落とし穴**: ON にすると深度プリパスが強制的に走る。TAA/SSAO/SSR/DXR がどれも無効なシーンで
  ON にすると、プリパスぶんの描画コールが増えて**遅くなる**（実測: city_blocks で drawCalls
  1068→1543、fps 604→549）。プリパスが元々走っているシーンなら追加コストは `gpuPassMs.hiZ` の
  0.04ms だけで `mainScene` が 3 割減る（実測: Nocturne で 0.23→0.16ms、drawCalls は 4656 のまま）
- ★**どちらのテストシーンも GPU 律速ではない**（CPU バウンド）ので fps は改善しない。
  効いてくるのは GPU 律速になってから
- 影パス（描画コールの約 90%）には未対応。光源視点の Hi-Z が別途要る

### 操作方法

#### カメラ
- **右クリック + WASD**: フライスルーカメラ（Space/Shift で上下、右クリック中はカーソル非表示）
- **F**: 選択エンティティにフォーカス（AABB から距離自動算出）
- **F11**: ボーダレスフルスクリーン切り替え
- **F1**: Play 中の一時停止 / 再開（時間だけ止めてシーンビューを飛び回れる。Lua / 物理 / アニメ / パーティクル /
  ネット / 空間オーディオが揃って止まる。一時停止中はエディタアイコンとアクティブカメラの錐台も描く。
  実体は `EditorContext::paused` の 1 フラグで、`Application::Update` の分岐を `Editor || paused` にして
  エディタ側へ流し込むのが肝。★ゲームがマウスを掴んでいるとツールバーのボタンは押せないのでキーが本命。
  マウスキャプチャは退避・復元する。`m_isGameMode`（配布ランタイム）では無効）

#### エディタ
- **W / E / R**: ギズモモード切替（移動 / 回転 / スケール）
- **T**: ギズモのローカル/ワールド空間切替、**Ctrl ドラッグ**: スナップ
  - スナップ量（移動 m / 回転 deg / スケール倍率）と「常にスナップ」は**「エンジン設定」窓 > ギズモ**
    （メニュー「表示 > エンジン設定」）。既定は 1.0 / 15 / 0.1 で従来のハードコード値と同じ
  - **マルチ選択でも移動だけでなく回転/スケールが効く**（選択群の中心を仮想ピボットにして群ごと変換）
  - ドラッグ中は増分（`T X +1.234` / `R …` / `S …`）が数値でオーバーレイ表示。掴める軸はホバーで太く光る
- **左クリック**: ピッキング選択（**Ctrl+クリック**: マルチ選択）。当たり判定は**三角形単位**
  （`src/editor/ScenePick.{h,cpp}`。ライト/カメラ/Empty はアイコンのスクリーン半径 18px で判定）
- **同じ場所を連続クリック**: 重なりの**循環選択**（手前 → 奥へ 1 段ずつ）。
  4px 以内・1.2 秒以内・重なり列が前回と同じ間だけ継続する。Ctrl+クリック中は循環しない（常に最前面）
- **Ctrl+Z / Ctrl+Y**: Undo / Redo（Transform・コンポーネント編集・追加/削除・複製・リネーム・親子変更を網羅）
- **Ctrl+D**: 複製、**Ctrl+C / Ctrl+V**: コピー / ペースト（全コンポーネントのディープコピー）
- **Del**: 削除（子も一括削除。Undo でサブツリー丸ごと復元）
- **Ctrl+S**: シーン保存、**Ctrl+N**: 新規シーン
- **Hierarchy D&D**: 親子付け（空白へドロップで解除）、**.lua D&D**: スクリプトアタッチ
- **ダブルクリック**: リネーム
- **Inspector「✚ コンポーネント追加」**: ライト/カメラ/RigidBody/コライダーを後付け
- **コンポーネントヘッダ右クリック**: コンポーネント削除

#### ライティング（シーンビュー上で直接いじる）
- **L 押しっぱなし + マウス移動**: 太陽（最初の `DirectionalLight`）の向きを回す。
  方位/高度をビュー上端にオーバーレイ表示。離した時に Undo が 1 エントリ積まれる
  （UE の `Ctrl+L` 相当。`Ctrl+L` は「新規スクリプト」で埋まっているので `L` 単独）
- **ライトを選択 → 丸ハンドルをドラッグ**: スポット = 外/内コーン角・距離、
  ポイント = 距離、平行光 = 向き。ドラッグ確定時に Undo 1 エントリ
- **ツール > ライティング**: ライト一覧（灯数の上限警告つき・目玉で一時ミュート）/
  太陽（時刻・方位・高度・色温度）/ 影 / スカイ・IBL / プリセット（昼・夕暮れ・夜・
  屋内・ホラー・スタジオ）/ 3点ライト設置。時刻カーブは Lua の `Lighting.setTimeOfDay` と同式
- 同パネルの「全ライトの影響範囲を表示」で、選択していないライトの range / コーンも薄く出る
- 実装: `src/editor/LightHandles.{h,cpp}`（ビューポート操作）/ `src/editor/LightMath.h`（純関数・
  `tests/light_math_test.cpp` で検証）/ `src/editor/panels/LightingPanel.{h,cpp}`（パネル）

#### 地形 / スカルプトのブラシ（ビューポートで彫る）
窓を開いている間（かつ「ブラシ有効」ON の間）だけビューポートのクリックをブラシが横取りする。
どちらも**同じキー割り当て**。窓を閉じるか「ブラシ有効」を OFF にすれば通常の選択に戻る。
- **左ドラッグ**: 彫る / **Shift**: 逆方向（盛る↔削る）/ **Ctrl**: 押している間だけ「ならす Smooth」
  - ★地形の**浸食(Erode)だけ**は強さ・ぼかし・X/Zミラー・Shift を使わない（安息角と反復回数のみ）
- **`[` / `]`**: ブラシ半径を 0.85 倍 / 1.18 倍（カーソルがビューポート上にある時だけ）
- Undo/Redo は**ストローク単位**（押下〜離すで 1 エントリ。触った所の差分だけを持つ）
- **地形**: ヒエラルキー「✚ エンティティ追加 > Terrain（地形・山を作る）」→ 地形ツール窓。
  ブラシ 6 種（盛る/削る/ならす/平らに/ノイズ/浸食）。`Terrain` 付きを選ぶと窓が自動で開く
- **地形のテクスチャ**: 地形ツール窓「テクスチャ（レイヤー）」で `.terrainlayers`（4 層の PBR 素材）を
  割り当てると、草/土/岩/雪をスプラット重みで混ぜて描く（高さブレンド + マクロ変化 + 距離タイリング +
  トライプラナー + POM）。「ペイントモード」ON で左ドラッグがレイヤーのペイントになる。
  詳細は [`docs/AUTHORING.md` §10.5.1](docs/AUTHORING.md)。**未割当なら従来の見た目のまま**
- **スカルプト（異形）**: 同メニュー「Sculpt（異形・洞窟・アーチ・岩）」→ スカルプト窓。
  ブラシ 8 種（盛る/引っぱる/押し込む/ならす/平らに/つまむ/ノイズ/掴む）+ X/Y/Z ミラー対称。
  「掴む Grab」だけは掴んだ後に表面から外れても追従する（視線に垂直な平面上のカーソル移動量）

#### エディタ実装メモ
- エンティティの生成/複製/ペースト/削除 Undo/Redo は **フレーム境界で遅延実行**
  （モデルロードに有効な cmdList が必要なため）。EditorContext の pending* キュー経由
- シーン保存は JSON（SceneSerializer）。親子関係は配列インデックス参照で保存
- プリミティブは MeshRenderer.modelPath のマーカー（`__primitive_box__` 等）で種別判定
- 親子階層のワールド行列は `ComputeWorldMatrix(reg, e)`（描画/ギズモ/ピッキングで使用）
- 削除 Undo は SerializeEntity の JSON スナップショットから全コンポーネント復元
- **ビューポートツールの追加口**: `EditorContext::viewportToolHandlers`（`std::function<bool(const ViewportInput&)>`
  の配列）。SceneViewPanel が **UI 編集・3D ピッキングより前に**先頭から順に呼び、`true` を返した時点で
  打ち切って以降の選択操作を行わない。各パネルの `Render` 初回で 1 度だけ push する。
  **現在の登録順 = ライトハンドル → 地形ブラシ → スカルプトブラシ**（＝先に登録した方がクリックを取る。
  地形とスカルプトの窓を両方開くと地形が先に食う）。新しいビューポートツールもここへ足す
- **地形/スカルプトの実データはシーン JSON に入らない**: 高さ配列は `assets/terrain/*.hf`、頂点配列は
  `assets/sculpt/*.smsh`（どちらもバイナリ・読み込みは vfs 経由＝配布では game.pak から読む）。
  JSON にはパスとパラメータだけ。**ストロークを離した瞬間に自動保存**される（これを消化しないと
  Play→Stop のシーン再構築で彫った内容が保存済みファイルまで巻き戻る）
- **再構築のタイミング**: 描画メッシュは `_meshDirty` を各パネルの `Render` が毎フレーム消化
  （窓を閉じていても回るので Ctrl+Z の結果が必ず絵に出る）。コライダーは `_colliderDirty` を
  `PhysicsSystem::Update` の頭（`RefreshTerrainColliders` / `RefreshSculptColliders`）が消化＝
  **Play 中のみ・ストローク終了時にだけ**作り直す（ドラッグ中に毎フレーム形状を作らない）。
  地形は Jolt の `HeightFieldShape`、スカルプトは `MeshShape`（動く剛体に付いている場合だけ凸包へフォールバック）
- ピッキング/ギズモの CPU 時間は `dx12_perf_stats` の `cpuScopeMs` に `picking` / `gizmo` として出る
  （どちらも `editorUi` の内数＝二重計上。「エディタ UI が重い」の内訳を名指しするため）

---

## MCP(AI ブリッジ)

起動中のエディタ(`DX12Engine.exe`)を Claude Code / Codex から MCP 経由で直接操作できる。
エンティティの生成・配置・コンポーネント設定・Lua アタッチ・Play/Stop・スクショ取得まで AI から実行可能。

- **詳細リファレンス**: [`docs/MCP.md`](docs/MCP.md) — ツール全一覧・遅延同期の仕組み・error_code・セキュリティ
- **AI エージェント運用ガイド**: [`tools/mcp-server/AGENTS.md`](tools/mcp-server/AGENTS.md) — 典型ワークフロー・禁止パターン・よくある間違い
- **ポート**: 自動採番(8787〜8797)。確定値は `%TEMP%\dx12_mcp.port` に書かれる。`DX12_MCP_PORT` 環境変数で上書き可。
- **認証**: なし(localhost 専用・開発機前提)。ゲーム(封印ランタイム)ではブリッジは起動しない。
- **配布**: MCP サーバはエンジン配布物に**同梱しない**。別リポジトリ [ryuto-alt/dx12-mcp](https://github.com/ryuto-alt/dx12-mcp) で配布する。
  ソース・オブ・トゥルースは本リポジトリの `tools/mcp-server`。**MCP サーバを変更したら `tools/mcp-server/publish.ps1` で dx12-mcp へ同期すること。**
