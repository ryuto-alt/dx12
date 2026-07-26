#include "resource/TextureLoader.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "graphics/Texture.h"
#include "graphics/GraphicsDevice.h"

#include <DirectXTex.h>

#include <vector>
#include <algorithm>   // ConvertToPng の縮小サイズ計算
#include <atomic>      // 圧縮モードのスイッチ
#include <chrono>      // BC 圧縮の所要時間ログ
#include <cstdio>      // Probe のエラーメッセージ整形
#include <cstring>     // ハッシュの memcpy
#include <cwctype>     // 拡張子の小文字化
#include <filesystem>  // .texcache の作成/存在確認
#include <string>

namespace dx12e
{

namespace
{

// settings.json の "texture_compression"(0=無圧縮 / 1=高速 / 2=高品質、既定 1)。
// Application がプロジェクトロード時に反映する。
std::atomic<int> g_compressionMode{1};
// キャッシュディレクトリを作れなかった時の警告を 1 度だけ出すためのフラグ。
std::atomic<bool> g_cacheDirWarned{false};

// mip が 1 枚しか無い非圧縮画像はフルミップチェーンを生成する（遠景のシマー/
// エイリアシング防止）。BC 圧縮はデコード無しで再生成できないためそのまま。
// 生成失敗は品質低下のみなので mip1 のまま続行する。
void EnsureMipChain(DirectX::ScratchImage& scratch, bool srgb)
{
    const DirectX::TexMetadata& meta = scratch.GetMetadata();
    if (meta.mipLevels > 1) return;
    if (DirectX::IsCompressed(meta.format)) return;
    if (meta.width <= 1 && meta.height <= 1) return;

    // sRGB コンテンツはガンマ考慮のフィルタで縮小しないと mip が暗くなる
    const DirectX::TEX_FILTER_FLAGS filter =
        srgb ? DirectX::TEX_FILTER_SRGB : DirectX::TEX_FILTER_DEFAULT;

    DirectX::ScratchImage mipChain;
    if (SUCCEEDED(DirectX::GenerateMipMaps(
            scratch.GetImages(), scratch.GetImageCount(), meta, filter, 0, mipChain)))
    {
        scratch = std::move(mipChain);
    }
}

// arraySize × mipLevels の全 subresource を組む（D3D12 の順序: item major, mip minor）
std::vector<D3D12_SUBRESOURCE_DATA> BuildSubresources(const DirectX::ScratchImage& scratch)
{
    const DirectX::TexMetadata& meta = scratch.GetMetadata();
    std::vector<D3D12_SUBRESOURCE_DATA> subs;
    subs.reserve(meta.arraySize * meta.mipLevels);
    for (size_t item = 0; item < meta.arraySize; ++item)
    {
        for (size_t mip = 0; mip < meta.mipLevels; ++mip)
        {
            const DirectX::Image* img = scratch.GetImage(mip, item, 0);
            if (!img)
                return {};
            subs.push_back({img->pixels,
                            static_cast<LONG_PTR>(img->rowPitch),
                            static_cast<LONG_PTR>(img->slicePitch)});
        }
    }
    return subs;
}

// 拡張子で適切な DirectXTex ローダを選ぶ(Probe / ConvertToPng 共用)。
HRESULT LoadScratchByExt(const std::wstring& filePath, DirectX::ScratchImage& scratch)
{
    const size_t dot = filePath.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? filePath.substr(dot) : L"";
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
    if (ext == L".dds")
        return DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, scratch);
    if (ext == L".tga")
        return DirectX::LoadFromTGAFile(filePath.c_str(), nullptr, scratch);
    if (ext == L".hdr")
        return DirectX::LoadFromHDRFile(filePath.c_str(), nullptr, scratch);
    return DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, scratch);
}

// 同上のメタデータ専用版(画素を読まないので速い)。
HRESULT LoadMetadataByExt(const std::wstring& filePath, DirectX::TexMetadata& meta)
{
    const size_t dot = filePath.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? filePath.substr(dot) : L"";
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
    if (ext == L".dds")
        return DirectX::GetMetadataFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, meta);
    if (ext == L".tga")
        return DirectX::GetMetadataFromTGAFile(filePath.c_str(), meta);
    if (ext == L".hdr")
        return DirectX::GetMetadataFromHDRFile(filePath.c_str(), meta);
    return DirectX::GetMetadataFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, meta);
}

// よく使う DXGI_FORMAT だけ名前を返す(それ以外は数値)。asset_info の表示用。
std::string DxgiFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM:      return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:  return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32_FLOAT:     return "R32G32B32_FLOAT";
    case DXGI_FORMAT_R8_UNORM:            return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM:          return "R8G8_UNORM";
    case DXGI_FORMAT_BC1_UNORM:           return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB:      return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC3_UNORM:           return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB:      return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_BC5_UNORM:           return "BC5_UNORM";
    case DXGI_FORMAT_BC6H_UF16:           return "BC6H_UF16";
    case DXGI_FORMAT_BC7_UNORM:           return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB:      return "BC7_UNORM_SRGB";
    default: return "DXGI_FORMAT(" + std::to_string(static_cast<int>(f)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BC 圧縮 + .dds ディスクキャッシュ
//  ★ BC7 の CPU 圧縮は「数秒」では終わらない。実測（i7-14700F / 20 コア /
//     TEX_COMPRESS_PARALLEL 有効・Sponza の実テクスチャ 1024²+全ミップ）:
//        全モード探索  37.4 s（PSNR 43.3 dB）
//        mode6 のみ     1.1 s（PSNR 40.4 dB）  ← TEX_COMPRESS_BC7_QUICK
//        BC1（参考）    0.003 s（PSNR 15.7 dB）
//     並列化は既に効いている（同じ画像の単スレッド版は 190 s）。遅さの正体は
//     DirectXTex の BC7 コーデックのモード全探索そのもの。
//     そこで既定は QUICK(mode6) にし、"texture_compression"=2 で全探索へ戻せるようにした。
//  毎起動でやると話にならないので、
//  「元データのハッシュ + 用途 + 出力形式 + 品質」をキーに .dds を吐いて 2 回目以降はそれを読む。
//  置き場は .thumbcache と同じ流儀（エディタ = assets/.texcache/、
//  pak 配布ゲーム = assets/ がディスクに無いので exe 隣の .texcache/）。
// ─────────────────────────────────────────────────────────────────────────────

// 64bit FNV-1a（8 バイトずつ回して大きな画像でも 1ms 前後で済ませる）。暗号強度は不要。
uint64_t HashBytes(const uint8_t* data, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= len; i += 8)
    {
        uint64_t block = 0;
        std::memcpy(&block, data + i, 8);
        h = (h ^ block) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < len; ++i)
        h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

uint64_t HashString(const std::string& s)
{
    return HashBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// キャッシュ置き場（末尾 "/" 付き）。作成に失敗したら空文字を返す＝キャッシュ無しで動く。
std::string CacheDir()
{
    namespace fs = std::filesystem;
    // pak 配布では assets/ がディスクに存在しないので exe 隣へ。エディタは .thumbcache と同じ場所。
    const std::string dir = vfs::InGameMode()
        ? (PathResolver::BaseDir() + ".texcache/")
        : (PathResolver::AssetsDir() + ".texcache/");

    std::error_code ec;
    if (!fs::exists(dir, ec))
        fs::create_directories(dir, ec);
    if (ec || !fs::exists(dir, ec))
    {
        if (!g_cacheDirWarned.exchange(true))
            Logger::Warn("BC 圧縮キャッシュのフォルダを作れません（毎回圧縮します）: {}", dir);
        return {};
    }
    return dir;
}

// 圧縮前にソース形式を BC コーデックが素直に食える形へ揃える（BC6H=float16, それ以外=RGBA8）。
bool ConvertForCompression(DirectX::ScratchImage& scratch, DXGI_FORMAT dstFormat)
{
    using namespace DirectX;
    const DXGI_FORMAT want = (dstFormat == DXGI_FORMAT_BC6H_UF16)
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (scratch.GetMetadata().format == want)
        return true;

    ScratchImage converted;
    const HRESULT hr = Convert(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
                               want, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
    if (FAILED(hr))
        return false;
    scratch = std::move(converted);
    return true;
}

// scratch を BC へ圧縮する（成功したら scratch を差し替える）。
// contentHash: 元データのハッシュ。cacheKey: キャッシュ名の衝突ドメインを分ける識別子（通常は元パス）。
// 失敗しても品質/VRAM が従来どおりになるだけなので、常に「何もしない」でフォールバックする。
// allowArray: 配列テクスチャ（地形レイヤー配列）も圧縮対象にする。既定は false ＝従来どおり
//   arraySize>1 は圧縮しない（IBL の焼き済み .dds を二重圧縮しないため）。
void CompressInPlace(DirectX::ScratchImage& scratch, TextureUsage usage, bool srgb,
                     uint64_t contentHash, const std::string& cacheKey,
                     bool allowArray = false)
{
    using namespace DirectX;
    const int quality = g_compressionMode.load();
    if (quality == 0 || usage == TextureUsage::Unknown)
        return;

    const TexMetadata& meta = scratch.GetMetadata();
    if (IsCompressed(meta.format))
        return;   // 既に BC（DDS 入力 or キャッシュ読み込み済み）
    const bool sizeOk = allowArray
        ? (meta.depth == 1 && meta.width >= 4 && meta.height >= 4
           && (meta.width % 4) == 0 && (meta.height % 4) == 0)
        : TextureLoader::IsCompressibleSize(meta.width, meta.height, meta.arraySize, meta.depth);
    if (!sizeOk)
        return;

    const DXGI_FORMAT dst = TextureLoader::SelectCompressedFormat(usage, meta.format, srgb);
    if (dst == DXGI_FORMAT_UNKNOWN)
        return;

    // ---- キャッシュヒット判定（形式まで含めてキーに入れるので、用途を変えたら自動で作り直る）----
    std::string cachePath;
    if (!cacheKey.empty())
    {
        const std::string dir = CacheDir();
        if (!dir.empty())
        {
            // ★ 品質をキーに含める（含めないと texture_compression を 1↔2 で切り替えても
            //   古い品質のキャッシュを読み続けて設定が効かない）。
            const std::string key = cacheKey + "|" + std::to_string(contentHash)
                                  + "|u" + std::to_string(static_cast<int>(usage))
                                  + "|f" + std::to_string(static_cast<int>(dst))
                                  + "|a" + std::to_string(static_cast<unsigned>(meta.arraySize))
                                  + "|q" + std::to_string(quality) + "|v2";
            char name[40];
            snprintf(name, sizeof(name), "t%016llx.dds",
                     static_cast<unsigned long long>(HashString(key)));
            cachePath = dir + name;

            std::error_code ec;
            if (std::filesystem::exists(cachePath, ec))
            {
                ScratchImage cached;
                if (SUCCEEDED(LoadFromDDSFile(PathResolver::Utf8ToWide(cachePath).c_str(),
                                              DDS_FLAGS_NONE, nullptr, cached))
                    && cached.GetMetadata().format    == dst
                    && cached.GetMetadata().width     == meta.width
                    && cached.GetMetadata().height    == meta.height
                    && cached.GetMetadata().arraySize == meta.arraySize)
                {
                    scratch = std::move(cached);
                    return;
                }
                // 壊れた/古いキャッシュは無視して作り直す
            }
        }
    }

    // ---- 圧縮（BC7/BC6H は重いので TEX_COMPRESS_PARALLEL 必須）----
    if (!ConvertForCompression(scratch, dst))
        return;

    TEX_COMPRESS_FLAGS flags = TEX_COMPRESS_PARALLEL;
    if (IsSRGB(dst))
        flags |= TEX_COMPRESS_SRGB;   // 誤差評価を知覚空間で行う（sRGB 入力 → sRGB 出力）
    // 高速モードは BC7 の探索を mode6 だけに絞る（30〜35 倍速く、PSNR は 3〜4 dB 落ちる）。
    // このフラグは BC7 専用（BC6HBC7.cpp の D3DXEncodeBC7 でのみ参照される）。BC5/BC1 は元から一瞬。
    if (quality <= 1)
        flags |= TEX_COMPRESS_BC7_QUICK;

    ScratchImage compressed;
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = Compress(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
                                dst, flags, TEX_THRESHOLD_DEFAULT, compressed);
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (elapsedMs > 1000.0)
    {
        // 1 秒を超えたら必ず記録に残す（「初回ロードが固まった」の切り分けが一瞬で済む）。
        Logger::Info("BC 圧縮 {:.1f}s: {}x{} {} q={} ({})", elapsedMs / 1000.0,
                     static_cast<unsigned>(meta.width), static_cast<unsigned>(meta.height),
                     DxgiFormatName(dst), quality, cacheKey);
    }
    if (FAILED(hr))
    {
        Logger::Warn("BC 圧縮に失敗しました（無圧縮のまま続行）: hr=0x{:08X} format={}",
                     static_cast<unsigned>(hr), DxgiFormatName(dst));
        return;
    }

    if (!cachePath.empty())
    {
        const HRESULT sh = SaveToDDSFile(compressed.GetImages(), compressed.GetImageCount(),
                                         compressed.GetMetadata(), DDS_FLAGS_NONE,
                                         PathResolver::Utf8ToWide(cachePath).c_str());
        if (FAILED(sh))
            Logger::Warn("BC 圧縮キャッシュの保存に失敗しました: {}", cachePath);
    }

    scratch = std::move(compressed);
}

} // namespace

void TextureLoader::SetCompressionMode(int mode)
{
    g_compressionMode.store((mode < 0) ? 0 : (mode > 2 ? 2 : mode));
}

TextureLoader::CompressionMode TextureLoader::GetCompressionMode()
{
    return static_cast<CompressionMode>(g_compressionMode.load());
}

bool TextureLoader::IsCompressionEnabled()
{
    return g_compressionMode.load() != 0;
}

DXGI_FORMAT TextureLoader::SelectCompressedFormat(TextureUsage usage, DXGI_FORMAT srcFormat, bool srgb)
{
    if (usage == TextureUsage::Unknown)
        return DXGI_FORMAT_UNKNOWN;

    // HDR(.hdr / float 系) は色数を潰さない BC6H へ。用途より元データの型を優先する。
    if (DirectX::FormatDataType(srcFormat) == DirectX::FORMAT_TYPE_FLOAT)
        return DXGI_FORMAT_BC6H_UF16;

    switch (usage)
    {
    case TextureUsage::BaseColor:
        // アルベドは BC7。sRGB で読む経路なら _SRGB 版（BC1 でも良いが画質重視でまず BC7）。
        return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    case TextureUsage::Normal:
        // 法線は 2ch の BC5（RG）。z は PBR.hlsli の PerturbNormal が再構成する。
        return DXGI_FORMAT_BC5_UNORM;
    case TextureUsage::NonColor:
        return DXGI_FORMAT_BC7_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool TextureLoader::IsCompressibleSize(size_t width, size_t height, size_t arraySize, size_t depth)
{
    // D3D12 はブロック圧縮テクスチャの最上位 mip に 4 の倍数を要求する。
    // キューブ/配列/3D はこのローダの圧縮対象外（IBL は別経路で焼き済み .dds を読む）。
    if (arraySize != 1 || depth != 1) return false;
    if (width < 4 || height < 4) return false;
    if ((width % 4) != 0 || (height % 4) != 0) return false;
    return true;
}

TextureProbeInfo TextureLoader::Probe(const std::wstring& filePath)
{
    TextureProbeInfo info;
    DirectX::TexMetadata meta{};
    const HRESULT hr = LoadMetadataByExt(filePath, meta);
    if (FAILED(hr))
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "metadata load failed (hr=0x%08X)", static_cast<unsigned>(hr));
        info.error = buf;
        return info;
    }
    info.width     = static_cast<uint32_t>(meta.width);
    info.height    = static_cast<uint32_t>(meta.height);
    info.mipLevels = static_cast<uint32_t>(meta.mipLevels);
    info.arraySize = static_cast<uint32_t>(meta.arraySize);
    info.format    = DxgiFormatName(meta.format);
    info.isCubemap = meta.IsCubemap();
    info.ok = true;
    return info;
}

bool TextureLoader::ConvertToPng(const std::wstring& srcPath, const std::wstring& dstPngPath,
                                 uint32_t maxSize, std::string& outError,
                                 uint32_t& outWidth, uint32_t& outHeight)
{
    using namespace DirectX;
    ScratchImage scratch;
    HRESULT hr = LoadScratchByExt(srcPath, scratch);
    if (FAILED(hr)) { outError = "load failed"; return false; }

    // BC 圧縮はまずデコード
    if (IsCompressed(scratch.GetMetadata().format))
    {
        ScratchImage decompressed;
        hr = Decompress(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
                        DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
        if (FAILED(hr)) { outError = "decompress failed"; return false; }
        scratch = std::move(decompressed);
    }
    // PNG 保存できる 8bit RGBA へ(HDR は 0..1 クランプ)
    if (scratch.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        ScratchImage converted;
        hr = Convert(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
                     DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { outError = "format convert failed"; return false; }
        scratch = std::move(converted);
    }
    // 長辺 maxSize 超は縮小(AI に見せる用途なのでフル解像度は不要)
    {
        const TexMetadata& meta = scratch.GetMetadata();
        const size_t longSide = (std::max)(meta.width, meta.height);
        if (maxSize > 0 && longSide > maxSize)
        {
            const double scale = static_cast<double>(maxSize) / static_cast<double>(longSide);
            size_t nw = static_cast<size_t>(static_cast<double>(meta.width) * scale);
            size_t nh = static_cast<size_t>(static_cast<double>(meta.height) * scale);
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            ScratchImage resized;
            hr = Resize(scratch.GetImages(), scratch.GetImageCount(), meta,
                        nw, nh, TEX_FILTER_DEFAULT, resized);
            if (FAILED(hr)) { outError = "resize failed"; return false; }
            scratch = std::move(resized);
        }
    }
    const Image* img = scratch.GetImage(0, 0, 0);   // キューブ/配列は先頭面のみ
    if (!img) { outError = "no image"; return false; }
    hr = SaveToWICFile(*img, WIC_FLAGS_NONE, GetWICCodec(WIC_CODEC_PNG), dstPngPath.c_str());
    if (FAILED(hr)) { outError = "png save failed"; return false; }
    outWidth  = static_cast<uint32_t>(img->width);
    outHeight = static_cast<uint32_t>(img->height);
    return true;
}

std::unique_ptr<Texture> TextureLoader::LoadFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filePath,
    bool srgb,
    TextureUsage usage)
{
    DirectX::ScratchImage scratchImage;

    // 拡張子で読み込み方法を判定(拡張子なしパスでも out_of_range を投げない)
    const size_t dotPos = filePath.find_last_of(L'.');
    const std::wstring ext = (dotPos != std::wstring::npos) ? filePath.substr(dotPos) : L"";

    HRESULT hr = S_OK;
    if (ext == L".dds" || ext == L".DDS")
    {
        hr = DirectX::LoadFromDDSFile(
            filePath.c_str(),
            DirectX::DDS_FLAGS_NONE,
            nullptr,
            scratchImage);
    }
    else
    {
        hr = DirectX::LoadFromWICFile(
            filePath.c_str(),
            DirectX::WIC_FLAGS_NONE,
            nullptr,
            scratchImage);
    }

    // wstring → UTF-8（ログ / 圧縮キャッシュのキーで使う）
    std::string pathStr;
    {
        int sz = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sz > 1)
        {
            pathStr.assign(static_cast<size_t>(sz), '\0');
            WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, pathStr.data(), sz, nullptr, nullptr);
            pathStr.pop_back();
        }
    }

    if (FAILED(hr))
    {
        Logger::Error("テクスチャの読み込みに失敗しました: {}", pathStr);
        return nullptr;
    }

    // mip が無ければ生成（メタデータは生成後に取り直す）
    EnsureMipChain(scratchImage, srgb);

    // BC 圧縮（キャッシュ経由）。キャッシュキーはパス + 更新時刻 + サイズ。
    if (usage != TextureUsage::Unknown)
    {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(filePath, ec);
        const auto mt = std::filesystem::last_write_time(filePath, ec);
        const std::string stamp = pathStr + "|" + (ec ? std::string("?") :
            std::to_string(static_cast<unsigned long long>(sz)) + ":" +
            std::to_string(mt.time_since_epoch().count()));
        CompressInPlace(scratchImage, usage, srgb, HashString(stamp), pathStr);
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    // D3D12_RESOURCE_DESC 構築
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment          = 0;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = static_cast<UINT16>(meta.arraySize);
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // 全 subresource（arraySize×mip）をアップロード
    // (旧: mip0 のみ = DDS のミップは VRAM を確保するだけの未初期化ゴミだった)
    auto subs = BuildSubresources(scratchImage);
    if (subs.empty())
    {
        Logger::Error("ScratchImage から画像データを取得できません");
        return nullptr;
    }

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Texture loaded: {}x{}, mips={}, format={}",
                 static_cast<u32>(meta.width),
                 static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels),
                 static_cast<u32>(format));

    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadCubeFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filePath,
    bool srgb)
{
    auto toUtf8 = [](const std::wstring& w) -> std::string {
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sz <= 1) return std::string{};
        std::string s(static_cast<size_t>(sz), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
        s.pop_back();
        return s;
    };

    const size_t dotPos = filePath.find_last_of(L'.');
    const std::wstring ext = (dotPos != std::wstring::npos) ? filePath.substr(dotPos) : L"";
    if (!(ext == L".dds" || ext == L".DDS"))
    {
        Logger::Error("LoadCubeFromFile: キューブマップは .dds のみ対応です: {}", toUtf8(filePath));
        return nullptr;
    }

    DirectX::ScratchImage scratchImage;
    HRESULT hr = DirectX::LoadFromDDSFile(filePath.c_str(),
        DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    if (FAILED(hr))
    {
        Logger::Error("キューブマップ DDS の読み込みに失敗しました: {}", toUtf8(filePath));
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    if (!meta.IsCubemap() || meta.arraySize != 6)
    {
        Logger::Error("LoadCubeFromFile: 6面キューブマップではありません: {}", toUtf8(filePath));
        return nullptr;
    }

    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment          = 0;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = 6;
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // 6 面 × 全 mip ぶんの subresource を ScratchImage から手動で組む。
    // D3D12 のサブリソース順序は item(face) major, mip minor:
    //   subresource = face * mipLevels + mip
    const size_t mipLevels = meta.mipLevels;
    std::vector<D3D12_SUBRESOURCE_DATA> subs(6 * mipLevels);
    for (size_t face = 0; face < 6; ++face)
    {
        for (size_t mip = 0; mip < mipLevels; ++mip)
        {
            const DirectX::Image* img = scratchImage.GetImage(mip, face, 0);
            if (!img)
            {
                Logger::Error("LoadCubeFromFile: 画像がありません（face={}, mip={}）: {}",
                              static_cast<u32>(face), static_cast<u32>(mip), toUtf8(filePath));
                return nullptr;
            }
            auto& sd      = subs[face * mipLevels + mip];
            sd.pData      = img->pixels;
            sd.RowPitch   = static_cast<LONG_PTR>(img->rowPitch);
            sd.SlicePitch = static_cast<LONG_PTR>(img->slicePitch);
        }
    }

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Cube texture loaded: {}x{}, mips={}, format={}",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels), static_cast<u32>(format));

    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadFromMemory(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t dataSize,
    const char* formatHint,
    bool srgb,
    TextureUsage usage,
    const std::string& cacheKey)
{
    DirectX::ScratchImage scratchImage;

    HRESULT hr = S_OK;
    std::string hint = formatHint ? formatHint : "";

    // 拡張子ヒントより中身を優先する（配布 pak に圧縮済み .dds を "foo.png" の名前で
    // 入れても読めるようにするため。DDS のマジックは先頭 4 バイトの "DDS "）。
    const bool ddsMagic = (dataSize >= 4 && data[0] == 'D' && data[1] == 'D' &&
                           data[2] == 'S' && data[3] == ' ');
    if (hint == "dds" || ddsMagic)
    {
        hr = DirectX::LoadFromDDSMemory(data, dataSize,
            DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    }
    else
    {
        // jpg, png 等は WIC で読める
        hr = DirectX::LoadFromWICMemory(data, dataSize,
            DirectX::WIC_FLAGS_NONE, nullptr, scratchImage);
    }

    if (FAILED(hr))
    {
        Logger::Error("埋め込みテクスチャの読み込みに失敗しました（format={}）", hint);
        return nullptr;
    }

    // mip が無ければ生成（メタデータは生成後に取り直す）
    EnsureMipChain(scratchImage, srgb);

    // BC 圧縮（キャッシュ経由）。元バイト列のハッシュをキーにするので、
    // ディスクモード(ルーズファイル)でも pak モードでも同じ判定になる。
    if (usage != TextureUsage::Unknown)
        CompressInPlace(scratchImage, usage, srgb, HashBytes(data, dataSize), cacheKey);

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = static_cast<UINT16>(meta.arraySize);
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto subs = BuildSubresources(scratchImage);
    if (subs.empty()) return nullptr;

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Embedded texture loaded: {}x{}, mips={} (format={})",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels), hint);

    return texture;
}

std::unique_ptr<Texture> TextureLoader::CreateArrayFromRGBA(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::vector<const uint8_t*>& slices,
    uint32_t width, uint32_t height,
    bool srgb,
    TextureUsage usage,
    const std::string& cacheKey,
    uint64_t contentHash)
{
    using namespace DirectX;
    if (slices.empty() || width == 0 || height == 0) return nullptr;

    // 1. RGBA8 の配列 ScratchImage を組む（mip0 のみ）
    ScratchImage scratch;
    if (FAILED(scratch.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height,
                                    slices.size(), 1)))
    {
        Logger::Error("テクスチャ配列の確保に失敗しました ({}x{} x{})", width, height,
                      static_cast<u32>(slices.size()));
        return nullptr;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (size_t i = 0; i < slices.size(); ++i)
    {
        const Image* img = scratch.GetImage(0, i, 0);
        if (!img || !slices[i]) return nullptr;
        for (uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(img->pixels + static_cast<size_t>(y) * img->rowPitch,
                        slices[i] + static_cast<size_t>(y) * rowBytes, rowBytes);
        }
    }

    // 2. ミップチェーン生成（配列でも一括で作れる）。sRGB はガンマ考慮フィルタで縮小する。
    {
        ScratchImage mipChain;
        const TEX_FILTER_FLAGS filter = srgb ? TEX_FILTER_SRGB : TEX_FILTER_DEFAULT;
        if (SUCCEEDED(GenerateMipMaps(scratch.GetImages(), scratch.GetImageCount(),
                                      scratch.GetMetadata(), filter, 0, mipChain)))
            scratch = std::move(mipChain);
    }

    // 3. BC 圧縮（.texcache 経由）。配列でも 1 枚の .dds にまとまる。
    if (contentHash != 0)
        CompressInPlace(scratch, usage, srgb, contentHash, cacheKey, /*allowArray=*/true);

    const TexMetadata meta = scratch.GetMetadata();
    const DXGI_FORMAT format = (srgb && !IsSRGB(meta.format)) ? MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width            = static_cast<UINT64>(meta.width);
    resourceDesc.Height           = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize = static_cast<UINT16>(meta.arraySize);
    resourceDesc.MipLevels        = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format           = format;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto subs = BuildSubresources(scratch);
    if (subs.empty()) return nullptr;

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Texture array created: {}x{} x{} mips={} format={} ({})",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.arraySize), static_cast<u32>(meta.mipLevels),
                 static_cast<u32>(format), cacheKey);
    return texture;
}

std::unique_ptr<Texture> TextureLoader::LoadCubeFromMemory(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t dataSize,
    bool srgb)
{
    DirectX::ScratchImage scratchImage;
    HRESULT hr = DirectX::LoadFromDDSMemory(data, dataSize,
        DirectX::DDS_FLAGS_NONE, nullptr, scratchImage);
    if (FAILED(hr))
    {
        Logger::Error("LoadCubeFromMemory: DDS バッファのデコードに失敗しました");
        return nullptr;
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    if (!meta.IsCubemap() || meta.arraySize != 6)
    {
        Logger::Error("LoadCubeFromMemory: 6面キューブマップではありません（arraySize={}）",
                      static_cast<u32>(meta.arraySize));
        return nullptr;
    }

    DXGI_FORMAT format = srgb ? DirectX::MakeSRGB(meta.format) : meta.format;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment          = 0;
    resourceDesc.Width              = static_cast<UINT64>(meta.width);
    resourceDesc.Height             = static_cast<UINT>(meta.height);
    resourceDesc.DepthOrArraySize   = 6;
    resourceDesc.MipLevels          = static_cast<UINT16>(meta.mipLevels);
    resourceDesc.Format             = format;
    resourceDesc.SampleDesc.Count   = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    // 6 面 × 全 mip ぶんの subresource を ScratchImage から手動で組む。
    // D3D12 のサブリソース順序は item(face) major, mip minor:
    //   subresource = face * mipLevels + mip
    const size_t mipLevels = meta.mipLevels;
    std::vector<D3D12_SUBRESOURCE_DATA> subs(6 * mipLevels);
    for (size_t face = 0; face < 6; ++face)
    {
        for (size_t mip = 0; mip < mipLevels; ++mip)
        {
            const DirectX::Image* img = scratchImage.GetImage(mip, face, 0);
            if (!img)
            {
                Logger::Error("LoadCubeFromMemory: 画像がありません（face={}, mip={}）",
                              static_cast<u32>(face), static_cast<u32>(mip));
                return nullptr;
            }
            auto& sd      = subs[face * mipLevels + mip];
            sd.pData      = img->pixels;
            sd.RowPitch   = static_cast<LONG_PTR>(img->rowPitch);
            sd.SlicePitch = static_cast<LONG_PTR>(img->slicePitch);
        }
    }

    auto texture = std::make_unique<Texture>();
    texture->Initialize(device, cmdList, resourceDesc, subs.data(), static_cast<u32>(subs.size()));

    Logger::Info("Cube texture (memory) loaded: {}x{}, mips={}, format={}",
                 static_cast<u32>(meta.width), static_cast<u32>(meta.height),
                 static_cast<u32>(meta.mipLevels), static_cast<u32>(format));

    return texture;
}

} // namespace dx12e
