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
#include <cassert>     // HashChunkInto のチャンク境界チェック
#include <chrono>      // BC 圧縮の所要時間ログ
#include <cstdio>      // Probe のエラーメッセージ整形
#include <cstring>     // ハッシュの memcpy
#include <cwctype>     // 拡張子の小文字化
#include <filesystem>  // .texcache の作成/存在確認
#include <fstream>     // HashFileContents（中身を読んでハッシュする）
#include <mutex>       // 同上（プリウォームのワーカースレッドと共有するメモ）
#include <string>
#include <unordered_map>

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
// 長辺が maxDim を超えていたら縮小する。maxDim==0 は何もしない。
// BC 圧縮済み / 配列 / キューブ / ボリュームは DirectXTex の Resize が扱えないので素通し
// （＝サムネイルでも等倍で載る。無言で壊すより等倍で正しい方がまし）。
void DownscaleIfLarger(DirectX::ScratchImage& scratch, uint32_t maxDim)
{
    if (maxDim == 0) return;
    const DirectX::TexMetadata& meta = scratch.GetMetadata();
    if (DirectX::IsCompressed(meta.format)) return;
    if (meta.arraySize > 1 || meta.depth > 1 || meta.IsCubemap()) return;

    size_t nw = 0, nh = 0;
    if (!TextureLoader::ComputeDownscale(meta.width, meta.height, maxDim, nw, nh)) return;

    // mip0 だけを縮小する（元にミップがあっても、この後 EnsureMipChain が作り直す）
    const DirectX::Image* src = scratch.GetImage(0, 0, 0);
    if (!src) return;

    DirectX::ScratchImage resized;
    if (FAILED(DirectX::Resize(*src, nw, nh, DirectX::TEX_FILTER_DEFAULT, resized)))
        return;   // 縮小に失敗しても等倍で続行する（読み込み自体は成功させる）
    scratch = std::move(resized);
}

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
constexpr uint64_t kHashSeed  = 1469598103934665603ull;
constexpr uint64_t kHashPrime = 1099511628211ull;

// ハッシュの本体。チャンクに分けて呼べるように「状態 h を進める」形にしてある。
//
// ★ここを 1 つに保つことが重要。ファイル経路(HashFileContents)とメモリ経路(HashBytes)で
//   別々の式を書いてしまうと、同じ画像なのに .texcache のキーが 2 種類でき、
//   同じテクスチャが二度 BC 圧縮される（実際にそうなっていた: 先読みが作ったキャッシュに
//   本読み込みが当たらず、49 秒のロードが 30 秒までしか縮まなかった）。
//   tests/texture_compress_test.cpp の Test_FileHashMatchesMemoryHash が両者の一致を見張る。
//
// tailAllowed=false のチャンク（= 最後以外）は必ず 8 の倍数であること。
// 8 バイト境界をまたいで状態を持ち越さない前提で、全体を 1 本で流したのと同じ値になる。
void HashChunkInto(uint64_t& h, const uint8_t* data, size_t len, bool isLastChunk)
{
    size_t i = 0;
    for (; i + 8 <= len; i += 8)
    {
        uint64_t block = 0;
        std::memcpy(&block, data + i, 8);
        h = (h ^ block) * kHashPrime;
        h ^= h >> 29;
    }
    // 端数は最終チャンクにしか現れない（呼び出し側が 8 の倍数で刻む）。
    assert(isLastChunk || i == len);
    (void)isLastChunk;
    for (; i < len; ++i)
        h = (h ^ data[i]) * kHashPrime;
}

uint64_t HashBytes(const uint8_t* data, size_t len)
{
    uint64_t h = kHashSeed;
    HashChunkInto(h, data, len, /*isLastChunk=*/true);
    return h;
}

uint64_t HashString(const std::string& s)
{
    return HashBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ファイルの【中身】のハッシュ。
// ★ここは以前 "パス|サイズ|更新時刻" の文字列ハッシュだった。ファイルを開かずに済む代わりに
//   git checkout に極端に弱い: git は書き出したファイルの mtime を「その時刻」にするので、
//   ブランチを行き来してテクスチャが書き戻されるだけで、中身が 1 バイトも変わっていなくても
//   キーが変わって全キャッシュがミスする。1 枚あたり数秒の BC7 圧縮がやり直しになり、
//   「ブランチを切り替えるとシーンを開くのに数十秒かかる」の直接の原因だった。
//   中身を読んで FNV-1a を回すコストは 1MB あたり 1ms 程度で、再圧縮に比べれば無視できる。
//   LoadFromMemory 側は元から HashBytes(中身) を使っていたので、これで両経路の規則が揃う。
//
// 同一ファイルは (srgb, usage) の組み合わせ違いで何度も読まれるので、
// (パス, サイズ, 更新時刻) をキーにプロセス内でメモ化して再読み込みを避ける。
// プリウォーム用のワーカースレッドから同時に呼ばれるため mutex で保護する。
uint64_t HashFileContents(const std::wstring& filePath)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    const auto size  = fs::file_size(filePath, ec);
    const auto mtime = fs::last_write_time(filePath, ec);
    if (ec) return 0;   // 読めないものはキャッシュしない（0 = キー無効）

    struct Stamp { uintmax_t size; int64_t mtime; uint64_t hash; };
    static std::mutex                                s_memoMutex;
    static std::unordered_map<std::wstring, Stamp>   s_memo;
    const int64_t mt = static_cast<int64_t>(mtime.time_since_epoch().count());
    {
        std::lock_guard<std::mutex> lock(s_memoMutex);
        auto it = s_memo.find(filePath);
        if (it != s_memo.end() && it->second.size == size && it->second.mtime == mt)
            return it->second.hash;
    }

    std::ifstream f(filePath, std::ios::binary);
    if (!f) return 0;

    // ★チャンク幅は 8 の倍数であること（HashChunkInto の前提）。
    //   これで「全バイトを 1 本で HashBytes した値」と必ず同じになる。
    uint64_t h = kHashSeed;
    std::vector<uint8_t> buf(1u << 16);
    static_assert((1u << 16) % 8 == 0, "chunk size must be a multiple of 8");
    uintmax_t readTotal = 0;
    while (f)
    {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got <= 0) break;
        readTotal += static_cast<uintmax_t>(got);
        const bool isLast = (static_cast<size_t>(got) < buf.size());
        HashChunkInto(h, buf.data(), static_cast<size_t>(got), isLast);
    }

    // ★最後まで読み切れなかったら「キー無効(0)」を返す。
    //   途中までのバイト列でハッシュを作ると、同じファイルなのに読めた量によって
    //   キーが変わり、キャッシュが当たらず毎回 BC 圧縮をやり直すことになる
    //   （先読みと本読み込みで別々のキーができてしまい、実際にそうなっていた）。
    //   0 を返せば「今回はキャッシュを使わない」だけで、絵は正しく出る。
    if (readTotal != size)
    {
        Logger::Warn("テクスチャのハッシュ用読み込みが途中で終わりました（キャッシュ無しで続行）: "
                     "{} / {} バイト", readTotal, static_cast<uintmax_t>(size));
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(s_memoMutex);
        s_memo[filePath] = Stamp{size, mt, h};
    }
    return h;
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

// 圧縮キャッシュのファイルパスを組む（空文字 = キャッシュ不使用）。
// ★キーの材料は「デコードしなくても分かるもの」だけで構成すること。
//   （元バイトのハッシュ / 用途 / 出力形式 / 配列数 / 品質）
//   ここにデコード後にしか分からない値を混ぜると、下の事前ヒット判定が成立しなくなり
//   「キャッシュがあるのに PNG を毎回デコードしてから捨てる」に逆戻りする。
// ★ 品質をキーに含める（含めないと texture_compression を 1↔2 で切り替えても
//   古い品質のキャッシュを読み続けて設定が効かない）。
std::string CompressedCachePath(DXGI_FORMAT dst, size_t arraySize, TextureUsage usage,
                                uint64_t contentHash, const std::string& cacheKey)
{
    if (cacheKey.empty())
        return {};
    const std::string dir = CacheDir();
    if (dir.empty())
        return {};

    const std::string key = cacheKey + "|" + std::to_string(contentHash)
                          + "|u" + std::to_string(static_cast<int>(usage))
                          + "|f" + std::to_string(static_cast<int>(dst))
                          + "|a" + std::to_string(static_cast<unsigned>(arraySize))
                          + "|q" + std::to_string(g_compressionMode.load()) + "|v2";
    char name[40];
    snprintf(name, sizeof(name), "t%016llx.dds",
             static_cast<unsigned long long>(HashString(key)));
    return dir + name;
}

// 圧縮キャッシュを引く。ヒットしたら outScratch に BC 済み画像が入って true。
// srcMeta は「元画像のメタデータ」で、ヘッダだけ読んだもの（GetMetadataFromWIC*）でも
// デコード済みのものでも良い ── キーが依存するのは format/arraySize だけで、
// どちらの経路でも同じ値になる。
bool TryLoadCachedCompressed(const DirectX::TexMetadata& srcMeta, TextureUsage usage, bool srgb,
                             uint64_t contentHash, const std::string& cacheKey,
                             bool allowArray, DirectX::ScratchImage& outScratch)
{
    using namespace DirectX;
    if (g_compressionMode.load() == 0 || usage == TextureUsage::Unknown)
        return false;
    if (IsCompressed(srcMeta.format))
        return false;   // 既に BC（DDS 入力）＝圧縮しないのでキャッシュも無い
    const bool sizeOk = allowArray
        ? (srcMeta.depth == 1 && srcMeta.width >= 4 && srcMeta.height >= 4
           && (srcMeta.width % 4) == 0 && (srcMeta.height % 4) == 0)
        : TextureLoader::IsCompressibleSize(srcMeta.width, srcMeta.height,
                                            srcMeta.arraySize, srcMeta.depth);
    if (!sizeOk)
        return false;

    const DXGI_FORMAT dst = TextureLoader::SelectCompressedFormat(usage, srcMeta.format, srgb);
    if (dst == DXGI_FORMAT_UNKNOWN)
        return false;

    const std::string cachePath =
        CompressedCachePath(dst, srcMeta.arraySize, usage, contentHash, cacheKey);
    if (cachePath.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec))
        return false;

    ScratchImage cached;
    if (FAILED(LoadFromDDSFile(PathResolver::Utf8ToWide(cachePath).c_str(),
                               DDS_FLAGS_NONE, nullptr, cached)))
        return false;   // 壊れたキャッシュは無視して作り直す

    const TexMetadata& cm = cached.GetMetadata();
    if (cm.format != dst || cm.width != srcMeta.width ||
        cm.height != srcMeta.height || cm.arraySize != srcMeta.arraySize)
        return false;   // 古い/食い違うキャッシュ

    outScratch = std::move(cached);
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
    // 呼び出し側がデコード前に TryLoadCachedCompressed で当てていれば、ここは基本ミスする。
    // それでも残してあるのは、事前判定を通らない経路（配列テクスチャ等）のため。
    {
        ScratchImage cached;
        if (TryLoadCachedCompressed(meta, usage, srgb, contentHash, cacheKey, allowArray, cached))
        {
            scratch = std::move(cached);
            return;
        }
    }
    const std::string cachePath =
        CompressedCachePath(dst, meta.arraySize, usage, contentHash, cacheKey);

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
        // usage とキャッシュ先も出す。同じ画像が二度圧縮されたときに「キーのどこが
        // 違ったのか」をログだけで追えるようにするため（先読みの空振り調査で必要になった）。
        Logger::Info("BC 圧縮 {:.1f}s: {}x{} {} usage={} q={} arr={} hash={:016x} -> {} ({})",
                     elapsedMs / 1000.0,
                     static_cast<unsigned>(meta.width), static_cast<unsigned>(meta.height),
                     DxgiFormatName(dst), static_cast<int>(usage), quality,
                     static_cast<unsigned>(meta.arraySize), contentHash,
                     std::filesystem::path(cachePath).filename().string(), cacheKey);
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

uint64_t TextureLoader::ContentHashForCacheKey(const std::wstring& filePath)
{
    return HashFileContents(filePath);
}

TextureLoader::PrewarmResult TextureLoader::PrewarmCompressedCache(
    const std::wstring& filePath, bool srgb, TextureUsage usage, uint32_t maxDimension)
{
    // LoadFromFile の「デコード → 縮小 → ミップ生成 → BC 圧縮 → .texcache へ .dds を書く」
    // までをそのままやって、結果を捨てる関数。D3D12 には一切触らないので
    // どのスレッドからでも呼べる（ここが GPU リソースを作らないことがワーカー化の前提）。
    // 実際の読み込みはこの後 LoadFromFile が走ったときにキャッシュヒットで済む。

    const size_t dotPos = filePath.find_last_of(L'.');
    const std::wstring ext = (dotPos != std::wstring::npos) ? filePath.substr(dotPos) : L"";
    if (ext == L".dds" || ext == L".DDS") return PrewarmResult::Skipped;  // 既に BC
    if (usage == TextureUsage::Unknown)   return PrewarmResult::Skipped;  // 圧縮対象外

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
    if (pathStr.empty()) return PrewarmResult::Failed;

    const uint64_t contentHash = TextureLoader::ContentHashForCacheKey(filePath);
    if (contentHash == 0) return PrewarmResult::Failed;

    // 既にキャッシュがあるなら何もしない（起動のたびに全部を作り直さないための最重要分岐）。
    {
        DirectX::TexMetadata srcMeta{};
        DirectX::ScratchImage probe;
        if (SUCCEEDED(DirectX::GetMetadataFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, srcMeta))
            && TryLoadCachedCompressed(srcMeta, usage, srgb, contentHash, pathStr, false, probe))
        {
            return PrewarmResult::AlreadyCached;
        }
    }

    DirectX::ScratchImage scratch;
    if (FAILED(DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, scratch)))
        return PrewarmResult::Failed;

    DownscaleIfLarger(scratch, maxDimension);
    EnsureMipChain(scratch, srgb);
    CompressInPlace(scratch, usage, srgb, contentHash, pathStr);   // ここで .texcache へ書かれる
    return PrewarmResult::Compressed;
}

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

// 読み込み時に指定した色空間を、実際のリソース形式へ確定させる。
//
// ★ここが素直に見えて素直でない箇所。
//   以前は `srgb ? MakeSRGB(fmt) : fmt` だった。つまり srgb=false は
//   「sRGB を付けない」だけで「sRGB を外す」ことができなかった。
//   ソース画像のメタデータが最初から _SRGB だと（最近の書き出しツールは
//   法線や ORM にも sRGB チャンクを埋めてくる）、リニアで読んだつもりの
//   テクスチャが GPU 側で sRGB デコードされる。
//   法線なら 0.5 が 0.21 になり、ORM なら粗さと金属度がまるごとずれる。
//   ライティングが静かに壊れるだけでエラーは出ない。
//
//   BC 圧縮が効く経路は SelectCompressedFormat が形式を明示するので無事だった。
//   踏むのは圧縮が省かれる経路――圧縮 OFF、4 の倍数でないサイズ、
//   ConvertForCompression の失敗――で、そこだけ黙って壊れていた。
DXGI_FORMAT TextureLoader::SelectViewFormat(DXGI_FORMAT fmt, bool srgb)
{
    return srgb ? DirectX::MakeSRGB(fmt) : DirectX::MakeLinear(fmt);
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

bool TextureLoader::ComputeDownscale(size_t width, size_t height, uint32_t maxDim,
                                     size_t& outWidth, size_t& outHeight)
{
    if (maxDim == 0 || width == 0 || height == 0) return false;

    const size_t longSide = (std::max)(width, height);
    if (longSide <= maxDim) return false;

    const double scale = static_cast<double>(maxDim) / static_cast<double>(longSide);
    size_t nw = static_cast<size_t>(static_cast<double>(width) * scale);
    size_t nh = static_cast<size_t>(static_cast<double>(height) * scale);
    // 極端なアスペクト比（8192x1 等）で短辺が 0 に落ちると Resize が失敗する
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    outWidth  = nw;
    outHeight = nh;
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
    TextureUsage usage,
    uint32_t maxDimension)
{
    DirectX::ScratchImage scratchImage;

    // 拡張子で読み込み方法を判定(拡張子なしパスでも out_of_range を投げない)
    const size_t dotPos = filePath.find_last_of(L'.');
    const std::wstring ext = (dotPos != std::wstring::npos) ? filePath.substr(dotPos) : L"";

    const bool isDds = (ext == L".dds" || ext == L".DDS");

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

    // キャッシュキー = ファイルの中身のハッシュ。パスや更新時刻は混ぜない
    //（混ぜると git checkout のたびに全ミスする。ContentHashForCacheKey のコメント参照）。
    uint64_t contentHash = 0;
    if (!isDds && usage != TextureUsage::Unknown)
        contentHash = TextureLoader::ContentHashForCacheKey(filePath);

    // ★ デコードより先に BC 圧縮キャッシュを引く（LoadFromMemory 側と同じ理由）。
    //   ヘッダだけ読めばキーが組めるので、ヒットすればデコードもミップ生成も不要。
    bool fromCache = false;
    if (!isDds && usage != TextureUsage::Unknown)
    {
        DirectX::TexMetadata srcMeta{};
        fromCache = SUCCEEDED(DirectX::GetMetadataFromWICFile(
                        filePath.c_str(), DirectX::WIC_FLAGS_NONE, srcMeta))
                 && TryLoadCachedCompressed(srcMeta, usage, srgb, contentHash, pathStr,
                                            false, scratchImage);
    }

    if (!fromCache)
    {
        HRESULT hr = isDds
            ? DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, scratchImage)
            : DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, scratchImage);

        if (FAILED(hr))
        {
            Logger::Error("テクスチャの読み込みに失敗しました: {}", pathStr);
            return nullptr;
        }

        // ★縮小はミップ生成より前。後だとフルサイズのミップ鎖を一度作ってから捨てることになる。
        DownscaleIfLarger(scratchImage, maxDimension);

        // mip が無ければ生成（メタデータは生成後に取り直す）
        EnsureMipChain(scratchImage, srgb);

        // BC 圧縮（キャッシュ経由）。キャッシュキーはファイルの中身のハッシュ。
        if (usage != TextureUsage::Unknown)
            CompressInPlace(scratchImage, usage, srgb, contentHash, pathStr);
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = TextureLoader::SelectViewFormat(meta.format, srgb);

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

    DXGI_FORMAT format = TextureLoader::SelectViewFormat(meta.format, srgb);

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
    const std::string& cacheKey,
    uint32_t maxDimension)
{
    DirectX::ScratchImage scratchImage;

    HRESULT hr = S_OK;
    std::string hint = formatHint ? formatHint : "";

    // 拡張子ヒントより中身を優先する（配布 pak に圧縮済み .dds を "foo.png" の名前で
    // 入れても読めるようにするため。DDS のマジックは先頭 4 バイトの "DDS "）。
    const bool ddsMagic = (dataSize >= 4 && data[0] == 'D' && data[1] == 'D' &&
                           data[2] == 'S' && data[3] == ' ');
    const bool isDds = (hint == "dds" || ddsMagic);

    // ★ BC 圧縮キャッシュは「デコードする前」に引く。
    //   ヒットするなら PNG のデコード(2048² で RGBA 16MB)とミップ生成が丸ごと不要になる。
    //   以前はデコード＋ミップ生成を済ませてから CompressInPlace の中で判定していたため、
    //   キャッシュが効いていても 1 枚あたり 143〜169ms 払って結果を捨てていた
    //   （Nocturne の 70 枚で実測 3.2 秒）。ヘッダだけ読めばキーは組める。
    bool fromCache = false;
    const uint64_t contentHash = (!isDds && usage != TextureUsage::Unknown)
                                     ? HashBytes(data, dataSize) : 0;
    if (!isDds && usage != TextureUsage::Unknown)
    {
        DirectX::TexMetadata srcMeta{};
        fromCache = SUCCEEDED(DirectX::GetMetadataFromWICMemory(
                        data, dataSize, DirectX::WIC_FLAGS_NONE, srcMeta))
                 && TryLoadCachedCompressed(srcMeta, usage, srgb, contentHash, cacheKey,
                                            false, scratchImage);
    }

    if (!fromCache)
    {
        if (isDds)
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

        // ★縮小はミップ生成より前（LoadFromFile と同じ）
        DownscaleIfLarger(scratchImage, maxDimension);

        // mip が無ければ生成（メタデータは生成後に取り直す）
        EnsureMipChain(scratchImage, srgb);

        // BC 圧縮（キャッシュ経由）。元バイト列のハッシュをキーにするので、
        // ディスクモード(ルーズファイル)でも pak モードでも同じ判定になる。
        if (usage != TextureUsage::Unknown)
            CompressInPlace(scratchImage, usage, srgb, contentHash, cacheKey);
    }

    DirectX::TexMetadata meta = scratchImage.GetMetadata();
    DXGI_FORMAT format = TextureLoader::SelectViewFormat(meta.format, srgb);

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

    DXGI_FORMAT format = TextureLoader::SelectViewFormat(meta.format, srgb);

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
