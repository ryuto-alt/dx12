#pragma once

#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "animation/SpriteAnimAsset.h"
#include "core/Types.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class DescriptorHeap;
class EditorContext;
class ResourceManager;
class UiAnimRuntime;

// スプライトシート(.spranim)エディタ。
// テクスチャを cols x rows のグリッドで切り、コマをクリックで拾って名前付きシーケンス
// （idle / run / attack …）を組み立てる。既存の Sprite2D::animFrames が「連続した N コマ」
// しか作れないのに対し、こちらは任意順・可変フレーム長・複数シーケンスを1アセットに持てる。
//
//   左: アセット一覧 + シーケンス一覧
//   中: シートのグリッド表示（クリックでコマを追加、Ctrl+クリックで除去）
//   右: 選択シーケンスの設定（fps / モード / フレーム列 / holds）+ 再生プレビュー
class SpriteSheetEditorPanel
{
public:
    // resources/srvHeap/cmdList はテクスチャを ImTextureID に解決するために使う
    // （UISystem と同じ経路: GetOrLoadTexture → SRV index → GPU ハンドル）。
    void RenderWindow(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                      ResourceManager* resources, DescriptorHeap* srvHeap,
                      ID3D12GraphicsCommandList* cmdList, UiAnimRuntime* runtime);

private:
    void RefreshAssetList(const std::string& assetsDir);
    void NewAsset();
    bool LoadAsset(const std::string& path);
    bool SaveAsset(const std::string& path, UiAnimRuntime* runtime);

    void DrawToolbar(EditorContext& ctx, const std::string& assetsDir, UiAnimRuntime* runtime);
    void DrawSeqList();
    void DrawSheetGrid(const std::string& assetsDir, ResourceManager* resources,
                       DescriptorHeap* srvHeap, ID3D12GraphicsCommandList* cmdList);
    void DrawSeqEditor(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir,
                       ResourceManager* resources, DescriptorHeap* srvHeap,
                       ID3D12GraphicsCommandList* cmdList);

    SpriteAnimSheet m_sheet;
    std::string m_currentPath;      // 空 = 未保存
    std::vector<std::string> m_assetNames;
    bool m_assetListLoaded = false;
    char m_nameBuf[128] = "NewSheet";
    char m_seqNameBuf[64] = "idle";

    int m_selSeq = -1;              // 選択中のシーケンス（-1 = 無し）
    f32 m_previewTime = 0.0f;
    bool m_previewPlaying = true;
    f32 m_gridZoom = 1.0f;
    bool m_showCellIndex = true;

    std::string m_statusMsg;
    f32 m_statusFlash = 0.0f;
};

} // namespace dx12e
