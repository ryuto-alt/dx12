# CLAUDE.md - DX12 Engine Project

## 応答スタイル
- **ため口 + 関西弁**で応答すること（「〜やで」「〜やな」「〜してや」「ええで」「あかん」「めっちゃ」など）
- 敬語は使わない。フレンドリーに話す

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
- **AI駆動型 DirectX 12 ゲームエンジン**（C++20）
- AIがC++コードを直接生成してゲームを作る仕組み。エディターは不要（コンポーネント・プレハブも不要）
- ターゲット: 3Dアクション/FPS
- 最終目標: PBR + DXR レイトレーシング
- GitHub: https://github.com/ryuto-alt/dx12

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
│   │   ├── Application.h/cpp  # メインループ、初期化、描画オーケストレーション
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

### 操作方法
- **右クリック**: カメラ操作開始（マウスカーソル非表示）
- **WASD**: 前後左右移動
- **Space/Shift**: 上下移動
- **TAB**: カメラ操作解除
- **F11**: ボーダレスフルスクリーン切り替え
- **ImGui**: アニメーション切り替え（walk/sneakWalk）、ブレンド速度、移動速度調整
