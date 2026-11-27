#include "core/SplashFonts.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace dx12e::splash
{
using Microsoft::WRL::ComPtr;

namespace
{
// ファイルの中身を持つだけの COM オブジェクト（InMemoryFontFileReference の ownerObject）。
class BufferOwner final : public IUnknown
{
public:
    std::vector<unsigned char> data;
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
private:
    ~BufferOwner() = default;
    volatile LONG ref_ = 1;
};

bool ReadAll(const std::wstring& file, std::vector<unsigned char>& out)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (16ll << 20);   // サブセットは約 100KB。16MB 超は異常
    if (ok)
    {
        out.resize(static_cast<size_t>(sz.QuadPart));
        size_t done = 0;
        while (ok && done < out.size())
        {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
            ok = ReadFile(h, out.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(h);
    return ok;
}
} // namespace

bool BuildSplashFallback(IDWriteFactory5* f5, ComPtr<IDWriteFontFallback>& out)
{
    out.Reset();
    if (!f5) return false;
    ComPtr<IDWriteFontFallbackBuilder> b;
    if (FAILED(f5->CreateFontFallbackBuilder(&b))) return false;
    // 全範囲を、日本語に強い UI 書体から順に当てる。無い書体はその行だけ飛ばされる。
    static const DWRITE_UNICODE_RANGE all[] = { { 0x0000, 0x10FFFF } };
    static const wchar_t* families[] = { L"Yu Gothic UI", L"Meiryo UI", L"Segoe UI", L"Segoe UI Symbol" };
    if (FAILED(b->AddMapping(all, 1, families, 4, nullptr, nullptr, nullptr, 1.0f))) return false;
    ComPtr<IDWriteFontFallback> sys;
    if (SUCCEEDED(f5->GetSystemFontFallback(&sys)) && sys) b->AddMappings(sys.Get());   // 最後にシステム既定（絵文字など）
    return SUCCEEDED(b->CreateFontFallback(&out));
}

size_t SplashFontSet::Bytes() const
{
    if (!owner_) return 0;
    return static_cast<BufferOwner*>(owner_.Get())->data.size();
}

void SplashFontSet::Reset()
{
    collection_.Reset();
    fallback_.Reset();
    if (factory_ && loader_ && registered_) factory_->UnregisterFontFileLoader(loader_.Get());
    registered_ = false;
    loader_.Reset();
    owner_.Reset();
    factory_.Reset();
    loadMs_ = 0.0;
    error_.clear();
}

bool SplashFontSet::Load(IDWriteFactory* dw, const std::wstring& file, const wchar_t* family)
{
    Reset();
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](bool ok) {
        loadMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return ok;
    };
    if (!dw || FAILED(dw->QueryInterface(IID_PPV_ARGS(&factory_)))) { error_ = "IDWriteFactory5"; return finish(false); }
    BuildSplashFallback(factory_.Get(), fallback_);   // 失敗してもよい（TextFormat は既定のフォールバックに縮退）
    if (file.empty()) { error_ = "no font file"; return finish(false); }

    auto* buf = new BufferOwner();
    owner_.Attach(buf);
    if (!ReadAll(file, buf->data)) { error_ = "read failed"; owner_.Reset(); return finish(false); }

    if (FAILED(factory_->CreateInMemoryFontFileLoader(&loader_))) { error_ = "CreateInMemoryFontFileLoader"; return finish(false); }
    if (FAILED(factory_->RegisterFontFileLoader(loader_.Get()))) { error_ = "RegisterFontFileLoader"; return finish(false); }
    registered_ = true;

    ComPtr<IDWriteFontFile> ff;
    if (FAILED(loader_->CreateInMemoryFontFileReference(factory_.Get(), buf->data.data(), static_cast<UINT32>(buf->data.size()),
                                                        owner_.Get(), &ff)))
    { error_ = "CreateInMemoryFontFileReference"; return finish(false); }
    ComPtr<IDWriteFontSetBuilder1> sb;
    if (FAILED(factory_->CreateFontSetBuilder(&sb))) { error_ = "CreateFontSetBuilder"; return finish(false); }
    if (FAILED(sb->AddFontFile(ff.Get()))) { error_ = "AddFontFile"; return finish(false); }
    ComPtr<IDWriteFontSet> set;
    if (FAILED(sb->CreateFontSet(&set))) { error_ = "CreateFontSet"; return finish(false); }
    ComPtr<IDWriteFontCollection1> col;
    if (FAILED(factory_->CreateFontCollectionFromFontSet(set.Get(), &col))) { error_ = "CreateFontCollectionFromFontSet"; return finish(false); }
    UINT32 idx = 0; BOOL ex = FALSE;
    if (FAILED(col->FindFamilyName(family, &idx, &ex)) || !ex) { error_ = "family not found"; return finish(false); }
    collection_ = col;
    return finish(true);
}

} // namespace dx12e::splash
