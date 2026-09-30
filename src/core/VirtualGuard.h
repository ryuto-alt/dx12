#pragma once

// ===========================================================================
// 仮想入力モード中に「人の画面へ窓を出す / フォーカスを奪う」操作を止めるための薄いガード
// ---------------------------------------------------------------------------
// AI が UI のボタンを押した結果として、次のような OS 操作が走ると人の作業を奪う:
//   ・ネイティブのファイル選択ダイアログ（モーダル。フォーカスを取り UI スレッドも止まる）
//   ・ShellExecute（エクスプローラー / 既定ブラウザ / VS Code が前面に出る）
//   ・CREATE_NEW_CONSOLE（別コンソール窓）/ 別プロセスのエディタ窓
// 仮想入力モード（vinput::Enabled()）の間はこれらを実行せず、ログに残して失敗扱いにする。
// Logger に依存しない（OutputDebugString のみ）ので、どのライブラリからでも使える。
// ===========================================================================

#include <atomic>
#include <Windows.h>
#include <shellapi.h>

#include "input/VirtualInput.h"

namespace dx12e::guard
{

// UI 自動テストの実行中は、仮想入力モードでなくても OS への副作用（エクスプローラー / ブラウザ / ダイアログ）を止める。
// テストが人の画面に窓を出してフォーカスを奪わないための保険（UiTestHarness が立てる）。
inline std::atomic<bool>& TestRunActive()
{
    static std::atomic<bool> s_active{false};
    return s_active;
}

// 仮想入力モードなら true（呼び出し側は何もせず戻る）。what は人が読む説明。
inline bool Blocked(const char* what)
{
    if (!vinput::Enabled() && !TestRunActive().load()) return false;
    OutputDebugStringA("[vinput] blocked (would touch the desktop): ");
    OutputDebugStringA(what ? what : "");
    OutputDebugStringA("\n");
    return true;
}

// ShellExecuteA の置き換え。仮想入力モード中は実行せず SE_ERR_ACCESSDENIED(5) を返す（<=32 は失敗の規約）。
inline HINSTANCE ShellExecuteGuarded(HWND hwnd, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    if (Blocked(file)) return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_ACCESSDENIED));
    return ::ShellExecuteA(hwnd, verb, file, params, dir, show);
}

} // namespace dx12e::guard
