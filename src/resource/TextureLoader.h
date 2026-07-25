#pragma once

#include <dxgiformat.h>

#include <string>
#include <memory>
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
    // settings.json の "texture_compression"(0/1、既定 1) で切り替える逃げ道。
    // ハードウェア/ツール差で問題が出たら 0 にすれば従来どおり無圧縮で読む。
    static void SetCompressionEnabled(bool enabled);
    static bool IsCompressionEnabled();

    // 用途 + 元フォーマットから BC 形式を決める純関数（DXGI_FORMAT_UNKNOWN = 圧縮しない）。
    // float 系(HDR)は用途に関わらず BC6H_UF16。テストから直接叩けるよう public にしてある。
    static DXGI_FORMAT SelectCompressedFormat(TextureUsage usage, DXGI_FORMAT srcFormat, bool srgb);

    // BC の 4x4 ブロック制約 / 2D 単体かどうかで圧縮可否を判定する純関数。
    static bool IsCompressibleSize(size_t width, size_t height, size_t arraySize, size_t depth);

    static TextureProbeInfo Probe(const std::wstring& filePath);

    // MCP read_texture 用: 対応形式(dds/tga/hdr/WIC 系)を PNG へ変換して保存する。
    // 長辺が maxSize を超える場合はアスペクト比を保って縮小。成功で true。
    static bool ConvertToPng(const std::wstring& srcPath, const std::wstring& dstPngPath,
                             uint32_t maxSize, std::string& outError,
                             uint32_t& outWidth, uint32_t& outHeight);

    static std::unique_ptr<Texture> LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::wstring& filePath,
        bool srgb = true,   // false = linear (normal/metalRoughness maps)
        TextureUsage usage = TextureUsage::Unknown);

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
        const std::string& cacheKey = std::string());

    // VFS / pak 経由キューブマップ用：メモリバッファから .dds キューブマップを読み込む。
    // IsCubemap() && arraySize==6 でなければ nullptr を返す。
    static std::unique_ptr<Texture> LoadCubeFromMemory(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const uint8_t* data, size_t dataSize,
        bool srgb = false);
};

} // namespace dx12e
