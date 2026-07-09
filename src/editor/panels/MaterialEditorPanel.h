#pragma once

#include <string>

#include "core/Types.h"
#include "resource/MaterialAssetIO.h"
#include "editor/panels/MaterialPreviewRenderer.h"

namespace dx12e
{

class EditorContext;
class AssetBrowserPanel;
class MaterialAssetManager;
class GraphicsDevice;
class DescriptorHeap;
class ResourceManager;
class CommandList;

// マテリアルアセット(assets/materials/*.dxmat)を見ながら編集する独立フローティング窓
// （ツール > マテリアルエディタ、またはアセットブラウザで .dxmat をダブルクリックで開く）。
// 専用3Dプレビュー(球体/平面、MaterialPreviewRenderer)を持つ。スカラー変更はドラッグ中
// MaterialAssetManager::UpdateScalarsOnly で即反映、確定/テクスチャ変更でディスクへ保存 +
// Invalidate してホットリロードさせる。
class MaterialEditorPanel
{
public:
    void Initialize(MaterialAssetManager* materialAssetManager, AssetBrowserPanel* assetBrowser,
                    GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager);

    // 3Dプレビューのオフスクリーン描画。ImGui BeginFrame より前、メインフレームのコマンドリストが
    // 開いている間に呼ぶこと(VfxEditorPanel::RenderPreview3D と同じ運用)。
    void RenderPreview3D(EditorContext& ctx, CommandList& cmd);

    // ctx.pendingOpenMaterialPath があれば消費してロードし、showMaterialEditor を立てる。
    // showMaterialEditor が false なら(消費後も含め)即 return。
    void RenderWindow(EditorContext& ctx, const std::string& assetsDir);

    // アセットブラウザの球体サムネイル(GetOrQueueThumbnail/RenderPendingThumbnails)を
    // 使い回すための公開アクセサ。プレビューレンダラーはこのパネルが所有する。
    MaterialPreviewRenderer& GetPreviewRenderer() { return m_preview; }

private:
    void NewAsset();
    bool LoadAsset(const std::string& relPath, const std::string& assetsDir);
    bool SaveAsset(const std::string& assetsDir);  // m_currentPath が空なら m_nameBuf からパスを決める
    void DrawTextureSlot(const std::string& assetsDir, const char* label, std::string& texRelPath);
    void ImportFromImage(const std::string& assetsDir);  // 「画像から作成」: PNG/JPGを選んでワンクリックで.dxmat化

    MaterialAssetManager* m_materialAssetManager = nullptr;
    AssetBrowserPanel*    m_assetBrowser = nullptr;

    MaterialAssetData m_current;
    std::string m_currentPath;        // assets 相対。空 = 未保存の新規アセット
    char        m_nameBuf[128] = "NewMaterial";
    std::string m_statusMsg;
    f32         m_statusFlash = 0.0f;

    // 3Dプレビュー
    MaterialPreviewRenderer m_preview;
    MaterialPreviewShape    m_previewShape = MaterialPreviewShape::Sphere;
    f32 m_camYaw = 0.6f, m_camPitch = 0.3f, m_camDist = 3.0f;
    // オービットドラッグ開始位置(ImGui座標)。ドラッグ中はカーソルをここへ固定+非表示にして
    // 無限回転できるようにする(離すと同じ位置にカーソルが再表示される。Unrealと同じ操作感)。
    f32 m_orbitAnchorX = 0.0f, m_orbitAnchorY = 0.0f;
};

} // namespace dx12e
