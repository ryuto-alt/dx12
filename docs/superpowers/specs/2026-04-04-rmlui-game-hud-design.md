# RmlUi ゲームHUD統合設計

## 概要

RmlUi（HTML/CSSベースUIライブラリ）をDX12エンジンに統合し、ゲーム用HUDシステムを構築する。
ImGuiはエディタUI専用として残し、RmlUiはゲームUI専用とする。

## 技術選定

- **RmlUi**: MIT License, HTML/CSS記法, FreeType依存, Luaプラグインあり
- **導入方法**: vcpkg (`rmlui[freetype]`)
- **DX12バックエンド**: PR #648 がマージ済みだが、サンプル用設計のため RenderInterface を自前実装

## アーキテクチャ

### 新規ファイル

```
src/gui/
├── RmlRenderer.h/cpp      # Rml::RenderInterface 実装（DX12描画）
├── RmlSystem.h/cpp         # Rml::SystemInterface 実装（時間・ログ）
└── RmlUIManager.h/cpp      # 統括（初期化・更新・入力・Context管理）

shaders/ui/
└── UI.hlsl                 # 2D UI用 VS/PS（ortho投影 + premultiplied alpha）

assets/ui/
├── hud.rml                 # ゲームHUDマークアップ
└── hud.rcss                # ゲームHUDスタイル
```

### 描画パイプライン統合

```
Application::Render()
  ├─ Shadow Pass
  ├─ 3D Scene (Forward PBR)
  ├─ Physics Debug Lines
  ├─ **RmlUi Render** ← NEW（深度テストOFF, アルファブレンドON）
  ├─ ImGui Render（エディタモード時のみ）
  └─ Present
```

### モジュール依存

```
RmlUIManager → RmlRenderer, RmlSystem, InputSystem, Window
RmlRenderer → GraphicsDevice, DescriptorHeap, CommandList, TextureLoader, Buffer
RmlSystem → GameClock, Logger
ScriptEngine → RmlUIManager（Luaバインド経由）
```

## RenderInterface 実装詳細

### 必須メソッド

| メソッド | 実装内容 |
|---------|---------|
| `CompileGeometry(vertices, indices)` | D3D12MA で VB/IB 作成、ハンドル返却 |
| `RenderGeometry(handle, translation, texture)` | 専用PSO + ortho行列で DrawIndexed |
| `ReleaseGeometry(handle)` | VB/IB 解放 |
| `LoadTexture(dims, source)` | TextureLoader でファイル読込 → SRV作成 |
| `GenerateTexture(data, dims)` | RGBAピクセルデータ → Upload → SRV作成 |
| `ReleaseTexture(handle)` | テクスチャ + SRV解放 |
| `EnableScissorRegion(enable)` | RSSetScissorRects |
| `SetScissorRegion(region)` | ScissorRect設定 |

### RmlUi 頂点フォーマット

```cpp
struct Rml::Vertex {
    Vector2f position;           // 2D screen-space
    ColourbPremultiplied colour; // RGBA8 premultiplied
    Vector2f tex_coord;          // UV
};
```

### 専用リソース

- **RootSignature**: b0(ortho行列 + translation), t0(テクスチャSRV), s0(linear clamp)
- **PSO**: 深度テストOFF, premultiplied alpha blend, CULL_NONE
- **シェーダー**: ortho projection VS + texture sample PS（テクスチャ無しはwhite fallback）

## SystemInterface 実装

```cpp
double GetElapsedTime() override {
    return m_gameClock->GetTotalTime();  // GameClock から取得
}
bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
    // Logger に転送
}
```

## 入力ブリッジ

Window::WndProc から RmlUi Context へ入力を転送:

- `WM_MOUSEMOVE` → `context->ProcessMouseMove(x, y, modifiers)`
- `WM_LBUTTONDOWN/UP` → `context->ProcessMouseButtonDown/Up(0, modifiers)`
- `WM_KEYDOWN/UP` → `context->ProcessKeyDown/Up(ConvertKey(vk), modifiers)`
- `WM_CHAR` → `context->ProcessTextInput(character)`

入力優先度: RmlUi → ImGui → ゲーム入力（UIが消費したら後続には渡さない）

## Luaバインド

ScriptEngine に `ui` テーブルを追加:

```lua
-- DOM要素のテキスト更新
ui:setText("ammo", "30 / 90")

-- CSS プロパティ変更（HPバーの幅など）
ui:setProperty("health-fill", "width", tostring(hp/maxHp * 100) .. "%")

-- 要素の表示/非表示
ui:show("game-over-screen")
ui:hide("game-over-screen")

-- ドキュメント読み込み/切り替え
ui:loadDocument("assets/ui/main_menu.rml")
ui:closeDocument("main_menu")
```

## フォント

- FreeType経由でTTF/OTFを読み込み
- 日本語フォント対応（Meiryo or Noto Sans JP）
- `Rml::LoadFontFace()` で登録

## ImGuiとの共存

- ImGui: エディタモード(`!m_isGameMode`)時のみ描画
- RmlUi: ゲームモード(`m_isGameMode`)時のみ描画
- 両方が同時に描画されることは基本的にない
- SRVヒープは共有（1024スロット、十分な空き）
