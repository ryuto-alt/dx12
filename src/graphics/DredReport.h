#pragma once

// W05-05 F0-3a: DRED（Device Removed Extended Data）とデバイス消失のレポート。
//   ・デバイス作成の前に AutoBreadcrumbs / PageFault / BreadcrumbContext を FORCED_ON にする。
//   ・消失時は ID3D12DeviceRemovedExtendedData2 からパンくず（コマンドリスト名・最後に完了した op・コンテキスト文字列）と
//     ページフォルトの VA・既存/解放済みの割り当て名を取り、%LOCALAPPDATA%\UnoEngine\crash\device_removed_<UTC>.json に書く。
//   ・既定はエディタで ON、GameRuntime で OFF。--dred で ON、--no-dred で OFF。
// 消失時の処理は、これ以上 D3D を呼べない前提で書く（全体を try/catch。待たない。書くのはローカルのファイル 1 つだけ）。

#include <Windows.h>

#include <directx/d3d12.h>

#include <filesystem>
#include <string>

namespace dx12e::dred
{

// コマンドラインが明示した DRED の指定。
enum class CmdLine { None, ForceOn, ForceOff };
CmdLine ParseCommandLine(const wchar_t* commandLine);

// GameRuntime（exe 名に GameRuntime を含む）か。
bool IsGameRuntimeExe();

// 純関数: 要求（GraphicsDeviceOptions::dred）・コマンドライン・GameRuntime か、から実際に ON にするかを決める。
//   --no-dred は常に OFF。要求が false なら OFF（コードからの明示）。--dred で ON。無指定は GameRuntime なら OFF、それ以外は ON。
bool Decide(bool requested, CmdLine cmd, bool gameRuntime);

// 現在のプロセスのコマンドライン・exe から Decide する。
bool ResolveEnabled(bool requested);

// デバイス作成の前に呼ぶ。FORCED_ON を設定できたら true（取れなければ false）。
bool EnableBeforeDeviceCreation();

// DRED から取った生のデータ（単体テストが手で組み立てられるよう公開）。ポインタは呼び出し側が生かしておく。
struct ReportData
{
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    HRESULT                             breadcrumbsHr = S_OK;
    D3D12_DRED_PAGE_FAULT_OUTPUT1       pageFault{};
    bool                                hasPageFault = false;
    D3D12_DRED_DEVICE_STATE             deviceState = D3D12_DRED_DEVICE_STATE_UNKNOWN;
    bool                                hasState = false;
};
std::string BuildReportJsonFromData(const ReportData& data, HRESULT removedReason, bool dredEnabled);

// レポートの JSON 文字列。デバイスが DRED を持たない／無効でも、removedReason だけは入る。例外を投げない。
std::string BuildReportJson(ID3D12Device* device, HRESULT removedReason, bool dredEnabled);

// 既定の出力先: %LOCALAPPDATA%\UnoEngine\crash（DX12E_DATA_DIR があればその下の crash）。
std::filesystem::path DefaultReportDir();

// JSON を device_removed_<UTC>.json として書く。書けたらそのパス（UTF-8）、失敗なら空。例外を投げない。
std::string WriteReport(ID3D12Device* device, HRESULT removedReason, bool dredEnabled,
                        const std::filesystem::path& dir = {});

// 名前の無い D3D12 オブジェクトにだけ名前を付ける（ページフォルトの割り当て名が空にならないように）。
void SetNameIfUnnamed(ID3D12Object* object, const wchar_t* name);
// "Texture 256x256 mip=9 fmt=28" のような名前を作って SetNameIfUnnamed する。
void NameTexture(ID3D12Object* object, const wchar_t* kind, UINT64 width, UINT height, UINT mips, DXGI_FORMAT format);
void NameBuffer(ID3D12Object* object, const wchar_t* kind, UINT64 bytes);

} // namespace dx12e::dred
