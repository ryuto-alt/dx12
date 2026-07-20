#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "core/Types.h"
#include "resource/MaterialAssetIO.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class ResourceManager;
class GraphicsDevice;
class DescriptorHeap;

// .dxmat（マテリアルアセット）のロード・SRVブロック管理・エディタ用ホットリロードを担う。
// テクスチャ解決/SRV割当は Application::EnsureMaterialOverrideSrv と同じ方針
// (albedo=sRGB、normal・metalRoughness=linear、欠落分は ResourceManager のデフォルトへフォールバック)。
class MaterialAssetManager
{
public:
    struct Entry
    {
        MaterialAssetData data;
        u32  srvBlockStart = 0xFFFFFFFF;  // albedo/normal/metalRoughness の連続3スロット先頭
        bool hasNormalTex  = false;       // pbrFlags のビット1相当(法線マップの有無)
        bool hasMRTex      = false;       // pbrFlags のビット2相当(metalRoughnessテクスチャの有無)
        bool valid         = false;
        bool attempted     = false;               // ロードを一度試みたか(失敗時の毎フレーム再試行を防ぐ)
        std::filesystem::file_time_type mtime{};  // ホットリロード検知用(エディタのみ使用)
    };

    void Initialize(ResourceManager* resourceManager, GraphicsDevice* device, DescriptorHeap* srvHeap);

    // relPath 例 "materials/red_brick_03.dxmat"（assets 相対）。
    // 未ロードなら vfs::ReadAsset でロードして SRV ブロックを構築する。cmdList はロード時のみ使用。
    // 失敗時は valid=false の Entry を返す(呼び出し側は焼き込み Material 等へフォールバックすること)。
    const Entry* GetOrLoad(const std::string& relPath, ID3D12GraphicsCommandList* cmdList);

    // エディタでの保存直後に呼ぶ。SRV ブロックは使い回し、次回 GetOrLoad で再ロードする。
    void Invalidate(const std::string& relPath);

    // スカラーのみ変更したい場合(マテリアルエディタのライブスライダー等)、SRV再構築を避けて
    // キャッシュ済みデータを直接書き換える。相手が未ロードなら何もしない。
    void UpdateScalarsOnly(const std::string& relPath, f32 metallic, f32 roughness,
                            f32 uvTilingU, f32 uvTilingV);

    // エディタのみ: 0.5秒間隔でロード済みエントリの mtime を見て変化があれば自動 Invalidate する。
    void PollHotReload(f32 dt, ID3D12GraphicsCommandList* cmdList);

private:
    void LoadInto(Entry& entry, const std::string& relPath, ID3D12GraphicsCommandList* cmdList);

    ResourceManager* m_resourceManager = nullptr;
    GraphicsDevice*  m_device  = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;

    std::unordered_map<std::string, Entry> m_cache;  // キー = 正規化済み relPath
    f32 m_pollTimer = 0.0f;
};

} // namespace dx12e
