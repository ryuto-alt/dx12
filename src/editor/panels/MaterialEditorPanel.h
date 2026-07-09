#pragma once

#include <string>

#include "core/Types.h"
#include "resource/MaterialAssetIO.h"

namespace dx12e
{

class EditorContext;
class AssetBrowserPanel;
class MaterialAssetManager;

// マテリアルアセット(assets/materials/*.dxmat)を見ながら編集する独立フローティング窓
// （ツール > マテリアルエディタ、またはアセットブラウザで .dxmat をダブルクリックで開く）。
// VfxEditorPanel と違い専用3Dプレビューは持たない（シーンに割当済みのメッシュがそのままプレビューになる:
// スカラー変更はドラッグ中 MaterialAssetManager::UpdateScalarsOnly で即反映、確定/テクスチャ変更で
// ディスクへ保存 + Invalidate してホットリロードさせる）。
class MaterialEditorPanel
{
public:
    void Initialize(MaterialAssetManager* materialAssetManager, AssetBrowserPanel* assetBrowser);

    // ctx.pendingOpenMaterialPath があれば消費してロードし、showMaterialEditor を立てる。
    // showMaterialEditor が false なら(消費後も含め)即 return。
    void RenderWindow(EditorContext& ctx, const std::string& assetsDir);

private:
    void NewAsset();
    bool LoadAsset(const std::string& relPath, const std::string& assetsDir);
    bool SaveAsset(const std::string& assetsDir);  // m_currentPath が空なら m_nameBuf からパスを決める
    void DrawTextureSlot(const std::string& assetsDir, const char* label, std::string& texRelPath);

    MaterialAssetManager* m_materialAssetManager = nullptr;
    AssetBrowserPanel*    m_assetBrowser = nullptr;

    MaterialAssetData m_current;
    std::string m_currentPath;        // assets 相対。空 = 未保存の新規アセット
    char        m_nameBuf[128] = "NewMaterial";
    std::string m_statusMsg;
    f32         m_statusFlash = 0.0f;
};

} // namespace dx12e
