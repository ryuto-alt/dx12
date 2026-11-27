#pragma once

// 起動画面の書体（W1-BD10 / BA-10）。splash_subset.ttf（Uno Splash Mono Light、約 100KB。Plex 全体は読まない）を
// 【メモリから】DirectWrite へ渡す。ファイルは 1 回だけ読み込んで自前のバッファに置き、IDWriteInMemoryFontFileLoader の
// 参照（ownerObject）にそのバッファを持たせる。
//
// 寿命: IDWriteInMemoryFontFileLoader は IDWriteFactory5 に登録した時点から、登録を外すまで factory が参照する。
// フォントコレクション / TextFormat / TextLayout は loader の中のバッファを使い続けるので、
//   (1) SplashFontSet がバッファ（ownerObject）・loader・コレクション・factory を同時に持ち、
//   (2) 破棄の順は「コレクション → フォールバック → 登録解除 → loader → factory」。
// SplashFontSet を壊す前に、それを使う TextFormat / TextLayout を先に捨てること（GaugeRenderer が守る）。
//
// 足りない字: IDWriteFontFallback を作って TextFormat1 に付ける。Uno Splash Mono に無い字は
// Yu Gothic UI → Meiryo UI → Segoe UI → Segoe UI Symbol → システム既定の順に当てる。

#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <string>

namespace dx12e::splash
{

class SplashFontSet
{
public:
    SplashFontSet() = default;
    ~SplashFontSet() { Reset(); }
    SplashFontSet(const SplashFontSet&) = delete;
    SplashFontSet& operator=(const SplashFontSet&) = delete;

    // file を読み込んで family（"Uno Splash Mono"）を持つコレクションを作る。失敗したら false（Error() に理由）。
    // 失敗しても fallback だけは作る（システム書体で描くため）。
    bool Load(IDWriteFactory* dw, const std::wstring& file, const wchar_t* family);
    void Reset();

    bool Loaded() const { return collection_ != nullptr; }
    IDWriteFontCollection1* Collection() const { return collection_.Get(); }
    IDWriteFontFallback* Fallback() const { return fallback_.Get(); }
    double LoadMs() const { return loadMs_; }
    size_t Bytes() const;
    const std::string& Error() const { return error_; }

private:
    Microsoft::WRL::ComPtr<IDWriteFactory5> factory_;
    Microsoft::WRL::ComPtr<IDWriteInMemoryFontFileLoader> loader_;
    Microsoft::WRL::ComPtr<IUnknown> owner_;              // バッファの持ち主（loader より長く生きる必要はないが、同じ寿命で持つ）
    Microsoft::WRL::ComPtr<IDWriteFontCollection1> collection_;
    Microsoft::WRL::ComPtr<IDWriteFontFallback> fallback_;
    bool registered_ = false;
    double loadMs_ = 0.0;
    std::string error_;
};

// システム書体だけのフォールバック（Segoe UI / Yu Gothic UI の順）。SplashFontSet とは別にテストから使える。
bool BuildSplashFallback(IDWriteFactory5* f5, Microsoft::WRL::ComPtr<IDWriteFontFallback>& out);

} // namespace dx12e::splash
