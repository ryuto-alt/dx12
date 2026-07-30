#pragma once

#include <string>
#include <vector>
#include <DirectXMath.h>
#include <entt/entt.hpp>

#include "core/Types.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "renderer/ParticleSystem.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class CommandList;
class EditorContext;
class ResourceManager;
class Scene;   // SpawnEntityCommand（Undo）に渡すだけ

// 名前付き VFX プリセット（assets/vfx/*.json）。ParticleEmitter コンポーネントの
// フィールドをほぼそのまま保持しつつ、Lua fx:burst{} 生成用のフィールド（burstCount）も
// 併せ持つ上位互換のデータ形式。配置エンティティへの「適用」と Lua コード生成の両方に使える。
struct VfxAsset
{
    std::string name = "NewEffect";

    int  kind  = 1;   // ParticleKind（既定 = Fire）
    int  blend = 0;   // ParticleBlend
    int  orient = 0;  // 粒子の向き 0=ビルボード 1=水平(地面) 2=垂直(+Z)

    // 配置エンティティ（継続放出）用
    f32  rate        = 30.0f;
    bool playOnStart  = true;
    bool looping      = true;
    f32  duration     = 1.0f;
    // Lua fx:burst{} 生成用（一度に発生させる粒子数）
    int  burstCount   = 24;

    DirectX::XMFLOAT3 dir{0.0f, 1.0f, 0.0f};
    f32  spread   = 0.4f;
    f32  speed    = 3.0f;
    f32  speedVar = 0.4f;

    f32  size    = 0.3f;
    f32  sizeMid = -1.0f;   // >=0 で3キーサイズカーブ
    f32  sizeEnd = 0.0f;
    f32  life    = 0.8f;
    f32  lifeVar = 0.3f;

    DirectX::XMFLOAT3 color{1.0f, 0.6f, 0.2f};
    DirectX::XMFLOAT3 colorMid{1.0f, 0.6f, 0.2f};
    DirectX::XMFLOAT3 colorEnd{1.0f, 0.12f, 0.05f};
    bool hasColorMid = false;
    f32  intensity   = 3.0f;

    f32  gravity = 0.0f;
    f32  drag    = 1.0f;
    f32  up      = 0.0f;
    f32  stretch = 0.0f;

    f32  turbStrength = 0.0f;
    f32  turbFreq     = 1.0f;

    f32  flicker     = 0.0f;
    f32  flickerFreq = 18.0f;

    f32  distort    = 0.0f;
    bool light      = false;
    f32  lightRange = 3.0f;
    std::string texturePath;   // assets 相対パス。空ならプロシージャル質感(kind依存)
};

// パーティクルエフェクトを見ながら作れる専用ツール窓（ツール > パーティクルエディタ）。
// 独立したプレビュー枠（自前の ParticleSystem インスタンス・オフスクリーン RenderTarget・
// オービットカメラ）を持ち、assets/vfx/*.json への名前付き保存、配置済みエンティティへの
// 適用、Lua fx:burst{} コード生成に対応する。
class VfxEditorPanel
{
public:
    // device/srvHeap/resourceManager は Application が所有するものをそのまま渡す
    // （プレビューRTのSRVをここへ確保する。resourceManager はテクスチャ貼り付けプレビュー用）。
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager,
                    const std::wstring& shaderDir);

    // シェーダーホットリロード用。プレビュー用 ParticleSystem の PSO を作り直すだけの薄い委譲。
    void RecreatePipelines(GraphicsDevice& device);

    // 3D プレビューのオフスクリーン描画。ImGui BeginFrame より前、メインフレームの
    // コマンドリストが開いている間に呼ぶこと。呼んだ後は呼び出し側でメインの
    // RTV/ビューポートを元に戻すこと（本パネルは自分の RT にしかバインドしない）。
    void RenderPreview3D(EditorContext& ctx, CommandList& cmd, f32 dt);

    // ImGui ウィンドウ本体（アセット一覧・プレビュー表示・パラメータ編集・操作ボタン）。
    // ctx.showVfxEditor が false なら即 return（呼び出し側でのガードは不要）。
    void RenderWindow(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                      Scene* scene);

private:
    void RefreshAssetList(const std::string& assetsDir);
    void NewAsset();
    bool LoadAsset(const std::string& path);
    bool SaveAsset(const std::string& path);
    void ApplyToSelected(entt::registry& reg, EditorContext& ctx);
    // Scene* と assetsDir は SpawnEntityCommand（Undo）に必要。
    // 無いと「新規エンティティとして配置」が Ctrl+Z で消えない。
    void SpawnEntity(entt::registry& reg, EditorContext& ctx,
                     Scene* scene, const std::string& assetsDir);
    std::string BuildLuaSnippet() const;

    void DrawColorGradient();
    void DrawSizeCurve();

    GraphicsDevice* m_device  = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    bool m_gfxInitialized = false;

    static constexpr u32 kPreviewSize = 512;
    DescriptorHeap  m_previewRtvHeap;
    RenderTarget    m_previewRT;
    ParticleSystem  m_previewParticles;

    f32 m_camYaw   = -2.6f;
    f32 m_camPitch = 0.35f;
    f32 m_camDist  = 6.0f;
    // オービットドラッグ開始位置(ImGui座標)。ドラッグ中はカーソルをここへ固定+非表示(無限回転)。
    f32 m_orbitAnchorX = 0.0f, m_orbitAnchorY = 0.0f;
    DirectX::XMFLOAT3 m_camTarget{0.0f, 1.0f, 0.0f};
    f32 m_emitAccum   = 0.0f;
    f32 m_previewTime = 0.0f;

    VfxAsset    m_current;
    std::string m_currentPath;   // 空 = 未保存の新規アセット

    std::vector<std::string> m_assetNames;   // 拡張子抜きファイル名一覧（assets/vfx/ 直下）
    bool m_assetListLoaded = false;
    char m_nameBuf[128] = "NewEffect";
    std::string m_statusMsg;
    f32 m_statusFlash = 0.0f;
};

} // namespace dx12e
