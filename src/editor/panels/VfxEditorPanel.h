#pragma once

#include <string>
#include <vector>
#include <DirectXMath.h>
#include <entt/entt.hpp>

#include "core/Types.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "renderer/ParticleSystem.h"
#include "editor/VfxApplyLogic.h"   // VfxAsset と「適用」の純ロジック（W1-A15）

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class CommandList;
class EditorContext;
class ResourceManager;
class Scene;   // SpawnEntityCommand（Undo）に渡すだけ

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
    // asNewLayer=false … 選択エンティティの 1 枚目を置き換える（従来の「適用」）
    // asNewLayer=true  … レイヤーとして追加する（炎の上に煙を重ねる、等）
    // W1-A15: 置き換える側（asNewLayer=false）で 1 枚目に中身があるときは確認（モーダル）を挟む。
    //   MCP のセッションがあり（aiSessionEver）かつ仮想入力中は AI がボタンを押せないので出さずに適用する（Undo で戻せる）。
    void RequestApply(entt::registry& reg, EditorContext& ctx, bool asNewLayer);
    void ApplyToSelected(entt::registry& reg, EditorContext& ctx, bool asNewLayer = false);
    void ApplyToEntity(entt::registry& reg, EditorContext& ctx, entt::entity e, bool asNewLayer);
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
    bool         m_openReplaceConfirm = false;   // 次のフレームで確認モーダルを開く
    entt::registry* m_confirmReg = nullptr;       // 確認を出したときのレジストリ（入れ替わり検出）
    ParticleEmitter m_confirmBefore{};            // 確認を出したときの放出器（同じ id の別物への誤適用を防ぐ）
    entt::entity m_confirmEntity = entt::null;   // 確認の対象（確認中に選択が変わっても元の相手に適用する）
};

} // namespace dx12e
