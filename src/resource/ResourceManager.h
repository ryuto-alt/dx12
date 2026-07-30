#pragma once

#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "resource/TextureLoader.h"   // TextureUsage（BC 圧縮の形式選択に使う）

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class Mesh;
struct Material;
class Skeleton;
class AnimationClip;
class NodeGraph;
class NodeAnimationClip;
class GraphicsDevice;
class DescriptorHeap;

// モデルキャッシュ（同一パスのモデルを共有）
struct CachedModel
{
    std::vector<std::unique_ptr<Mesh>>          meshes;
    std::vector<std::unique_ptr<Material>>      materials;
    std::unique_ptr<Skeleton>                   skeleton;   // null = static mesh
    std::vector<std::unique_ptr<AnimationClip>> animClips;

    // Node animation（skeleton が null でアニメーションがある場合）
    std::unique_ptr<NodeGraph>                         nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>>    nodeAnimClips;
};

class ResourceManager
{
public:
    void Initialize(GraphicsDevice* device, DescriptorHeap* srvHeap,
                    ID3D12GraphicsCommandList* cmdList);

    // usage は BC 圧縮の形式選択に使う（既定 Unknown = 圧縮しない＝従来どおり）。
    // マテリアルのスロットが分かっている呼び出し側（ModelLoader / .dxmat / Inspector の上書き）
    // だけが BaseColor / Normal / NonColor を渡すこと。
    // maxDimension > 0 = 長辺をそこまで縮めて読む（0 = 等倍）。アセットブラウザの
    // サムネイル専用。キャッシュキーにも混ぜてあるので、同じ画像を等倍で要求する経路
    // （UI 画像・マテリアル）が縮小版を掴むことはない。
    Texture* GetOrLoadTexture(
        const std::wstring& filePath,
        ID3D12GraphicsCommandList* cmdList,
        bool srgb = true,
        TextureUsage usage = TextureUsage::Unknown,
        u32 maxDimension = 0);

    // モデル読み込み（キャッシュ付き）
    const CachedModel* GetOrLoadModel(
        const std::string& filePath,
        ID3D12GraphicsCommandList* cmdList);

    // 埋め込みテクスチャ用（usage は GetOrLoadTexture と同じ意味＝BC 圧縮の形式選択）
    Texture* GetOrLoadEmbeddedTexture(
        const std::string& key,
        const uint8_t* data, size_t dataSize,
        const char* formatHint,
        ID3D12GraphicsCommandList* cmdList,
        bool srgb = true,
        TextureUsage usage = TextureUsage::Unknown);

    Texture* GetDefaultWhiteTexture() const { return m_defaultWhite.get(); }
    Texture* GetDefaultNormalTexture() const { return m_defaultNormal.get(); }
    Texture* GetDefaultMetalRoughnessTexture() const { return m_defaultMetalRoughness.get(); }
    GraphicsDevice* GetDevice() const { return m_device; }
    DescriptorHeap* GetSrvHeap() const { return m_srvHeap; }

    void FinishUploads();
    // 直近のフレームで新しいアップロード(テクスチャ/モデル)を積んだか。
    // true のフレームだけ WaitIdle+FinishUploads する＝定常時の毎フレーム全同期を撤廃。
    bool HasPendingUploads() const { return m_uploadsPending; }

    // 毎フレーム末尾用: 今フレームで新規ロードされたテクスチャのステージングだけを
    // 遅延解放キューへ回す。FinishUploads(全キャッシュ走査)と違い O(新規ロード数)。
    void DeferPendingUploads();

private:
    GraphicsDevice*  m_device  = nullptr;
    DescriptorHeap*  m_srvHeap = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<Texture>> m_textureCache;
    std::unique_ptr<Texture> m_defaultWhite;
    std::unique_ptr<Texture> m_defaultNormal;         // (128,128,255,255) = flat normal
    std::unique_ptr<Texture> m_defaultMetalRoughness; // (0,128,0,255) = non-metal, mid-rough
    std::unordered_map<std::string, std::unique_ptr<CachedModel>> m_modelCache;

    // 今フレームでロードされ、アップロードステージングが未回収のテクスチャ
    // (キャッシュは解放されないので生ポインタで安全)。毎フレーム末尾の
    // DeferPendingUploads がフェンス連動の遅延解放キューへ回す。
    std::vector<Texture*> m_pendingUploads;
    // 今フレームでロードされ、VB/IB ステージングが未回収のメッシュ（キャッシュ所有なので生ポインタ）
    std::vector<Mesh*> m_pendingMeshUploads;
    bool m_uploadsPending = false;  // FinishUploads/DeferPendingUploads で false に戻す
};

} // namespace dx12e
