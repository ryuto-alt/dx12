#pragma once

#include <dxgiformat.h>

#include <string>
#include <memory>
#include <vector>
#include <cstdint>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class GraphicsDevice;

// テクスチャの用途。BC 圧縮の形式選択に使う。
// 用途が分かっている経路(モデルのマテリアル / .dxmat / Inspector のテクスチャ上書き)だけ圧縮し、
// 判定できない経路(UI 画像・アイコン・スプライト・3D LUT 等)は Unknown = 無圧縮で安全側に倒す。
enum class TextureUsage
{
    Unknown = 0,  // 用途不明 → 圧縮しない
    BaseColor,    // アルベド / ベースカラー(sRGB) → BC7_UNORM_SRGB
    Normal,       // 法線マップ(linear, xy のみ) → BC5_UNORM（z はシェーダで再構成）
    NonColor,     // ORM / metallic-roughness / マスク(linear) → BC7_UNORM
};

// MCP asset_info 用: GPU を使わずテクスチャのメタ情報だけ読む(DirectXTex のみ)。
struct TextureProbeInfo
{
    bool        ok = false;
    std::string error;
    uint32_t width = 0, height = 0, mipLevels = 0, arraySize = 0;
    std::string format;      // DXGI_FORMAT 名(未知の値は数値)
    bool isCubemap = false;
};

class TextureLoader
{
public:
    // ---- BC 圧縮（取り込み時に DirectXTex の Compress を通し、結果を .dds でディスクキャッシュする）----
    // settings.json の "texture_compression" で切り替える。
    //   0 = 無圧縮（ハードウェア/ツール差で絵が壊れた時の逃げ道。従来の R8G8B8A8 で読む）
    //   1 = 高速（既定。BC7 は TEX_COMPRESS_BC7_QUICK ＝ mode6 のみ）
    //   2 = 高品質（BC7 の全モード探索。実測で 1 の 30〜35 倍遅い）
    // ★ 1 と 2 は .texcache のキーが別なので、切り替えると一度だけ作り直しになる。
    enum class CompressionMode : int { Off = 0, Fast = 1, HighQuality = 2 };
    static void SetCompressionMode(int mode);
    static CompressionMode GetCompressionMode();
    static bool IsCompressionEnabled();

    // 用途 + 元フォーマットから BC 形式を決める純関数（DXGI_FORMAT_UNKNOWN = 圧縮しない）。
    // float 系(HDR)は用途に関わらず BC6H_UF16。テストから直接叩けるよう public にしてある。
    // 読み込み時の色空間指定を、実際のリソース形式へ確定させる。
    // srgb=false は「sRGB を付けない」ではなく「sRGB を外す」でなければならない。
    // 詳しい理由は .cpp 側の実装コメントを参照。
    static DXGI_FORMAT SelectViewFormat(DXGI_FORMAT fmt, bool srgb);

    static DXGI_FORMAT SelectCompressedFormat(TextureUsage usage, DXGI_FORMAT srcFormat, bool srgb);

    // BC の 4x4 ブロック制約 / 2D 単体かどうかで圧縮可否を判定する純関数。
    static bool IsCompressibleSize(size_t width, size_t height, size_t arraySize, size_t depth);

    // 長辺を maxDim に収めるサイズを縦横比を保って求める純関数。
    // 縮小が不要（maxDim==0 / 既に収まっている / サイズ 0）なら false を返し out は触らない。
    static bool ComputeDownscale(size_t width, size_t height, uint32_t maxDim,
                                 size_t& outWidth, size_t& outHeight);

    // BC 圧縮ディスクキャッシュ（assets/.texcache/）のキーに使う「ファイルの中身」のハッシュ。
    // 読めない/空なら 0（= キャッシュしない）。
    //
    // ★このキーに**パスや更新時刻を混ぜてはいけない**。git は checkout したファイルの
    //   mtime をその時刻に書き換えるので、mtime が入っているとブランチを行き来するだけで
    //   中身が同じでも全ミスし、1 枚あたり数秒の BC7 圧縮がやり直しになる。
    //   （回帰テスト: tests/texture_compress_test.cpp の Test_CacheKeyIgnoresMtime）
    //
    // 同じファイルは (srgb, usage) 違いで何度も読まれるため、実装側で
    // (パス, サイズ, 更新時刻) をキーにメモ化してある。スレッドセーフ。
    static uint64_t ContentHashForCacheKey(const std::wstring& filePath);

    // BC 圧縮ディスクキャッシュだけを先に作る（GPU リソースは作らない）。
    //
    // 「デコード → 縮小 → ミップ生成 → BC 圧縮 → assets/.texcache へ .dds を書く」までを
    // 実行して結果を捨てる。D3D12 に一切触れないので【ワーカースレッドから呼べる】。
    // これを事前に回しておくと、後から LoadFromFile が走ったときにキャッシュヒットになり、
    // メインスレッドはアップロードだけで済む。
    //
    // 引数は LoadFromFile と同じ意味にすること。特に srgb / usage / maxDimension が
    // 実際の読み込みとズレるとキャッシュキーが変わり、先読みが無駄になる。
    //
    // ★呼び出し側はスレッドごとに CoInitializeEx を済ませておくこと（WIC が要求する）。
    // ★同じファイルを複数スレッドから同時に渡さないこと（同一 .dds へ同時に書き込む）。
    enum class PrewarmResult
    {
        Compressed,     // 圧縮してキャッシュを作った（時間がかかったのはこれ）
        AlreadyCached,  // 既にキャッシュがあった（何もしていない）
        Skipped,        // 対象外（.dds / 用途不明）
        Failed,         // 読めなかった
    };
    static PrewarmResult PrewarmCompressedCache(const std::wstring& filePath, bool srgb,
                                                TextureUsage usage, uint32_t maxDimension = 0);

    static TextureProbeInfo Probe(const std::wstring& filePath);

    // MCP read_texture 用: 対応形式(dds/tga/hdr/WIC 系)を PNG へ変換して保存する。
    // 長辺が maxSize を超える場合はアスペクト比を保って縮小。成功で true。
    static bool ConvertToPng(const std::wstring& srcPath, const std::wstring& dstPngPath,
                             uint32_t maxSize, std::string& outError,
                             uint32_t& outWidth, uint32_t& outHeight);

    // maxDimension > 0 なら、長辺がそれを超える画像を読み込み時点で縮小する（0 = 等倍＝従来どおり）。
    // 用途はサムネイル。80px のセルに 4K テクスチャを等倍で置くと VRAM が一瞬で溶ける。
    // ★BC 圧縮済み / 配列 / ボリュームは縮小しない（DirectXTex の Resize が扱えない）。
    static std::unique_ptr<Texture> LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filePath,
        bool srgb = true,   // false = linear (normal/metalRoughness maps)
        TextureUsage usage = TextureUsage::Unknown,
        uint32_t maxDimension = 0);

    // IBL 環境キューブ用：.dds の TEXTURECUBE を 6 面×全 mip でロードする。
    // SRV(TextureCube) は呼び出し側で Texture::CreateCubeSRV を使って張ること。
    static std::unique_ptr<Texture> LoadCubeFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filePath,
        bool srgb = false);

    // FBX 埋め込みテクスチャ / VFS(pak・ルーズファイル)用：メモリバッファから読み込み。
    // cacheKey は BC 圧縮結果のディスクキャッシュを引くための識別子（通常は元ファイルのパス）。
    // 空文字なら圧縮はするがキャッシュしない（＝毎回圧縮する。用途不明なら usage=Unknown で圧縮自体しない）。
    static std::unique_ptr<Texture> LoadFromMemory(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const uint8_t* data, size_t dataSize,
        const char* formatHint,
        bool srgb = true,
        TextureUsage usage = TextureUsage::Unknown,
        const std::string& cacheKey = std::string(),
        uint32_t maxDimension = 0);

    // 地形レイヤー配列用：CPU 側で組み上げた RGBA8 スライス列から Texture2DArray を作る。
    //   ミップ生成 → BC 圧縮（.texcache にキャッシュ）→ GPU アップロードまで一括で行う。
    // slices の各要素は width*height*4 バイトの RGBA8（先頭 = スライス 0）。
    // SRV は呼び出し側が Texture::CreateArraySRV で張ること。
    // contentHash は「元素材が変わったらキャッシュを捨てる」ための識別子（0 ならキャッシュしない）。
    static std::unique_ptr<Texture> CreateArrayFromRGBA(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::vector<const uint8_t*>& slices,
        uint32_t width, uint32_t height,
        bool srgb,
        TextureUsage usage,
        const std::string& cacheKey,
        uint64_t contentHash);

    // VFS / pak 経由キューブマップ用：メモリバッファから .dds キューブマップを読み込む。
    // IsCubemap() && arraySize==6 でなければ nullptr を返す。
    static std::unique_ptr<Texture> LoadCubeFromMemory(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const uint8_t* data, size_t dataSize,
        bool srgb = false);
};

} // namespace dx12e
