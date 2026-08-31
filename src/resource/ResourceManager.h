#pragma once

#include <cstdint>
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

// dx12_reload_assets の結果。どれを読み直したか / どれを見送ったかを MCP へそのまま返す。
struct AssetReloadResult
{
    int checkedTextures = 0;
    int checkedModels   = 0;
    // 読み直したもの（assets 相対に直す前の実パス。呼び出し側が相対化する）
    std::vector<std::string> textures;
    std::vector<std::string> models;
    // 読み直せなかったもの（"パス: 理由"）。黙って飛ばすと「効かないツール」になる。
    std::vector<std::string> skipped;
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

    // ---- ホットリロード（dx12_reload_assets の実体）----------------------
    // ディスクの更新時刻が読み込み時より新しいキャッシュだけを読み直す（force で全件）。
    // prefixUtf8 が空でなければ、その前方一致のパスだけを対象にする（正規化済み generic 形式）。
    //
    // ★テクスチャは **同じ Texture オブジェクト / 同じ SRV インデックスのまま** 中身を
    //   差し替える（Texture::AdoptFrom）。Material の 3 連続 SRV ブロックはリソースを
    //   握り直す必要があるので、テクスチャを 1 枚でも読み直したらモデルキャッシュ側の
    //   マテリアルブロックを全部張り直す（3 回の CreateSRV だけ＝安い）。
    // ★モデルは CachedModel を作り直すので Mesh*/Material* が **全部変わる**。
    //   MeshRenderer の再バインドと BLAS キャッシュの無効化は呼び出し側
    //   （Application::McpReloadAssets）の仕事。ここは reloaded に載せて知らせるだけ。
    AssetReloadResult ReloadChangedAssets(ID3D12GraphicsCommandList* cmdList,
                                          const std::string& prefixUtf8, bool force);

    // GetOrLoadModel と同じ正規化（MeshRenderer.modelPath からキャッシュキーを引くのに使う）
    static std::string NormalizeModelKey(const std::string& filePath);

    // 読み直したモデルのキー → 新しい CachedModel（ReloadChangedAssets の後だけ有効）
    const CachedModel* FindModel(const std::string& key) const;

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
    // キャッシュ 1 件。ホットリロードのために「どのファイルをどの設定で読んだか」と
    // 読んだ時点の更新時刻を控える（texture が null = ロード失敗を覚えている枠）。
    struct TextureCacheEntry
    {
        std::unique_ptr<Texture> texture;
        std::wstring  path;                       // 実ファイルの絶対パス（埋め込みは空 = 再読込対象外）
        bool          srgb  = true;
        TextureUsage  usage = TextureUsage::Unknown;
        u32           maxDimension = 0;
        int64_t       stamp = 0;                  // 読んだ時点の last_write_time（0 = 不明）
        bool          plain2d = true;             // 2D 単枚のみ張り直せる（cube/array は SRV の張り方が別）
    };
    std::unordered_map<std::wstring, TextureCacheEntry> m_textureCache;
    std::unique_ptr<Texture> m_defaultWhite;
    std::unique_ptr<Texture> m_defaultNormal;         // (128,128,255,255) = flat normal
    std::unique_ptr<Texture> m_defaultMetalRoughness; // (0,128,0,255) = non-metal, mid-rough
    struct ModelCacheEntry
    {
        std::unique_ptr<CachedModel> model;
        std::string path;    // 実際に読んだファイルパス
        int64_t     stamp = 0;
    };
    std::unordered_map<std::string, ModelCacheEntry> m_modelCache;

    // マテリアルの 3 連続 SRV ブロックを今のテクスチャ実体で張り直す（テクスチャ再読込後）
    void RefreshMaterialSrvBlocks();

    // 今フレームでロードされ、アップロードステージングが未回収のテクスチャ
    // (キャッシュは解放されないので生ポインタで安全)。毎フレーム末尾の
    // DeferPendingUploads がフェンス連動の遅延解放キューへ回す。
    std::vector<Texture*> m_pendingUploads;
    // 今フレームでロードされ、VB/IB ステージングが未回収のメッシュ（キャッシュ所有なので生ポインタ）
    std::vector<Mesh*> m_pendingMeshUploads;
    bool m_uploadsPending = false;  // FinishUploads/DeferPendingUploads で false に戻す
};

} // namespace dx12e
