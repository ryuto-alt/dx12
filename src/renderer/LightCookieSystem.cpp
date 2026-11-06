#include "renderer/LightCookieSystem.h"

#include "core/Logger.h"
#include "core/vfs/Vfs.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Texture.h"
#include "renderer/IesProfile.h"

#include <DirectXPackedVector.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dx12e
{

LightCookieSystem::LightCookieSystem()
{
    m_cookies.resize(kMaxCookies);
    m_ies.resize(kMaxIes);
    m_cookiePixels.assign(static_cast<size_t>(kMaxCookies) * kCookieSize * kCookieSize * 4, 255);
    m_iesHalf.assign(static_cast<size_t>(kMaxIes) * kIesSamples, DirectX::PackedVector::XMConvertFloatToHalf(1.0f));
}

LightCookieSystem::~LightCookieSystem() = default;

void LightCookieSystem::BeginRequests()
{
    for (auto& c : m_cookies) c.wanted = false;
    for (auto& i : m_ies)     i.wanted = false;
}

void LightCookieSystem::Request(const std::string& cookiePath, const std::string& iesPath)
{
    auto take = [this](auto& table, const std::string& path, auto loader)
    {
        if (path.empty()) return;
        for (auto& e : table)
            if (e.path == path) { e.wanted = true; return; }   // 既にスロットを持つ（読めなかった印も含む）
        // 空きスロット = 未使用 or 今フレームの要求に無い古いもの（読み込み失敗の印は早めに手放して再挑戦させない）
        size_t slot = table.size();
        for (size_t k = 0; k < table.size(); ++k)
            if (table[k].path.empty()) { slot = k; break; }
        if (slot == table.size())
            for (size_t k = 0; k < table.size(); ++k)
                if (!table[k].wanted) { slot = k; break; }
        if (slot == table.size())
        {
            static bool warned = false;
            if (!warned) { Logger::Warn("LightCookieSystem: クッキー / IES の同時使用数が上限（{}）を超えました", table.size()); warned = true; }
            return;
        }
        table[slot].path   = path;
        table[slot].wanted = true;
        table[slot].failed = !loader(slot);
        m_dirty = true;
    };

    take(m_cookies, cookiePath, [this](size_t slot) {
        u8* dst = m_cookiePixels.data() + slot * kCookieSize * kCookieSize * 4;
        if (!LoadCookiePixels(m_cookies[slot].path, dst))
        {
            std::memset(dst, 255, static_cast<size_t>(kCookieSize) * kCookieSize * 4);   // 読めない = 白（効果なし）
            return false;
        }
        return true;
    });
    take(m_ies, iesPath, [this](size_t slot) {
        u16* dst = m_iesHalf.data() + slot * kIesSamples;
        if (!LoadIesRow(m_ies[slot].path, dst))
        {
            const u16 one = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            for (u32 i = 0; i < kIesSamples; ++i) dst[i] = one;
            return false;
        }
        return true;
    });
}

int LightCookieSystem::CookieSlot(const std::string& path) const
{
    if (path.empty()) return -1;
    for (size_t k = 0; k < m_cookies.size(); ++k)
        if (m_cookies[k].path == path) return m_cookies[k].failed ? -1 : static_cast<int>(k);
    return -1;
}

int LightCookieSystem::IesSlot(const std::string& path) const
{
    if (path.empty()) return -1;
    for (size_t k = 0; k < m_ies.size(); ++k)
        if (m_ies[k].path == path) return m_ies[k].failed ? -1 : static_cast<int>(k);
    return -1;
}

std::vector<u8> LightCookieSystem::LoadBytes(const std::string& relPath) const
{
    std::vector<u8> bytes = vfs::ReadAsset(relPath);
    if (!bytes.empty() || m_engineAssetsDir.empty()) return bytes;
    // プロジェクトに無ければエンジン同梱（開発時のみ）。
    std::ifstream f(m_engineAssetsDir + relPath, std::ios::binary);
    if (!f) return bytes;
    return std::vector<u8>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool LightCookieSystem::LoadCookiePixels(const std::string& path, u8* dst)
{
    const std::vector<u8> bytes = LoadBytes(path);
    if (bytes.empty())
    {
        Logger::Warn("ライトのクッキーを読めませんでした（ファイルが無い）: {}", path);
        return false;
    }
    DirectX::ScratchImage img;
    // 色空間は解釈しない（バイトのまま。SRV を _SRGB にして GPU がリニアへ戻す）。
    HRESULT hr = DirectX::LoadFromWICMemory(bytes.data(), bytes.size(),
                                            DirectX::WIC_FLAGS_IGNORE_SRGB | DirectX::WIC_FLAGS_FORCE_RGB, nullptr, img);
    if (FAILED(hr))
    {
        Logger::Warn("ライトのクッキーを読めませんでした（デコード失敗）: {}", path);
        return false;
    }
    DirectX::ScratchImage conv;
    if (img.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(img.GetImages(), img.GetImageCount(), img.GetMetadata(),
                              DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, conv);
        if (FAILED(hr)) { Logger::Warn("ライトのクッキーの形式変換に失敗: {}", path); return false; }
        img = std::move(conv);
    }
    const DirectX::Image* src = img.GetImage(0, 0, 0);
    if (!src) return false;
    DirectX::ScratchImage rs;
    const DirectX::Image* use = src;
    if (src->width != kCookieSize || src->height != kCookieSize)
    {
        hr = DirectX::Resize(*src, kCookieSize, kCookieSize, DirectX::TEX_FILTER_LINEAR | DirectX::TEX_FILTER_SEPARATE_ALPHA, rs);
        if (FAILED(hr)) { Logger::Warn("ライトのクッキーのリサイズに失敗: {}", path); return false; }
        use = rs.GetImage(0, 0, 0);
    }
    for (u32 y = 0; y < kCookieSize; ++y)
    {
        const u8* row = use->pixels + static_cast<size_t>(y) * use->rowPitch;
        u8* out = dst + static_cast<size_t>(y) * kCookieSize * 4;
        std::memcpy(out, row, static_cast<size_t>(kCookieSize) * 4);
        for (u32 x = 0; x < kCookieSize; ++x) out[x * 4 + 3] = 255;   // アルファは使わない
    }
    return true;
}

bool LightCookieSystem::LoadIesRow(const std::string& path, u16* dst)
{
    const std::vector<u8> bytes = LoadBytes(path);
    if (bytes.empty())
    {
        Logger::Warn("IES を読めませんでした（ファイルが無い）: {}", path);
        return false;
    }
    const IesProfile prof = ParseIes(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!prof.ok)
    {
        Logger::Warn("IES を解析できませんでした: {} ({})", path, prof.error);
        return false;
    }
    std::vector<float> baked(kIesSamples);
    prof.Bake(baked.data(), baked.size());
    for (u32 i = 0; i < kIesSamples; ++i)
        dst[i] = DirectX::PackedVector::XMConvertFloatToHalf(std::clamp(baked[i], 0.0f, 1.0f));
    return true;
}

void LightCookieSystem::Rebuild(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd)
{
    auto make = [&](DXGI_FORMAT fmt, u32 w, u32 h, u32 arr, const void* data, u32 rowPitch, u32 slicePitch)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width            = w;
        d.Height           = h;
        d.DepthOrArraySize = static_cast<UINT16>(arr);
        d.MipLevels        = 1;
        d.Format           = fmt;
        d.SampleDesc       = {1, 0};
        d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        std::vector<D3D12_SUBRESOURCE_DATA> subs(arr);
        for (u32 s = 0; s < arr; ++s)
        {
            subs[s].pData      = static_cast<const u8*>(data) + static_cast<size_t>(s) * slicePitch;
            subs[s].RowPitch   = rowPitch;
            subs[s].SlicePitch = slicePitch;
        }
        auto t = std::make_unique<Texture>();
        t->Initialize(device, cmd, d, subs.data(), arr);
        t->FinishUpload();   // 実行完了までは DeferredRelease が面倒を見る
        return t;
    };
    // 旧テクスチャは unique_ptr の解放で DeferredRelease に載る（呼び出し側が WaitIdle 済みなので即時でも安全）。
    m_cookieTex = make(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kCookieSize, kCookieSize, kMaxCookies,
                       m_cookiePixels.data(), kCookieSize * 4, kCookieSize * kCookieSize * 4);
    m_iesTex    = make(DXGI_FORMAT_R16_FLOAT, kIesSamples, kMaxIes, 1,
                       m_iesHalf.data(), kIesSamples * 2, kIesSamples * kMaxIes * 2);
    m_dirty = false;
}

void LightCookieSystem::WriteSrvs(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cookieDst, D3D12_CPU_DESCRIPTOR_HANDLE iesDst)
{
    if (!IsReady()) return;
    m_cookieTex->CreateArraySRV(device, cookieDst);
    m_iesTex->CreateSRV(device, iesDst);
}

} // namespace dx12e
