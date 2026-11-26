#pragma once

#include <cstdint>
#include <string>

namespace dx12e
{

// 実際に読み込まれた D3D12 のランタイム（D3D12Core.dll）の情報。
struct AgilityInfo
{
    std::uint32_t loadedSdkVersion = 0;     // 読み込まれた Core が export する D3D12SDKVersion（未読み込みは 0）
    bool          fromRedistDir    = false; // <exeDir>\D3D12\ から読んだか（情報）
    bool          sha256Match      = false; // 読み込んだ Core の SHA256 が lock と一致するか（情報）
    std::wstring  corePath;                 // 読み込まれた D3D12Core.dll のパス
    std::wstring  fileVersion;              // ファイルバージョン文字列（情報。合否には使わない）
};

// デバイス作成後に呼ぶ。GetModuleHandleW(L"D3D12Core.dll") → GetProcAddress("D3D12SDKVersion") で
// 読み込んだ Core の版を読む。合否は loadedSdkVersion >= RequiredAgilitySdk() だけで判定する。
AgilityInfo QueryAgilityRuntime();

// agility.lock.json の SDK 番号（UnoAgilityLock.h 由来）。
std::uint32_t RequiredAgilitySdk();
// agility.lock.json の版文字列（"1.620.2"）。
const char* RequiredAgilityVersion();

// <exeDir>\D3D12\D3D12Core.dll があるか。exeDir は実行ファイルのあるフォルダ。
std::wstring AgilityRedistDir();
bool         AgilityRedistPresent();

// ファイルの SHA256（小文字 16 進）。失敗は空文字。
std::string Sha256OfFile(const std::wstring& path);

// 読み込み済みモジュールのフルパス（未読み込みは空）。DX12_D3D_DEBUG=1 で d3d12SDKLayers.dll の出所をログへ出すのに使う。
std::wstring LoadedModulePath(const wchar_t* moduleName);

// Debug Layer を要求したのに D3D12GetDebugInterface が失敗したことの記録。
// テスト exe（AgilityTestGuard）が終了時に見て、失敗扱い（終了コード 97）にする。
void NoteDebugLayerFailure();
bool DebugLayerFailed();

} // namespace dx12e
