#pragma once
//
// VFS 公開ファサード（namespace dx12e::vfs）。
//   ゲームモード（pak マウント済み）: pak から復号 + 展開してメモリへ。
//   ディスクモード（エディタ / --validate）: AssetsDir からルーズファイルを読む（バイト等価）。
//
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::vfs
{

// pak をマウント（GameRuntime のブート）。ファイル欠落 / 不正なら false（呼び出し側はディスクモード可）。
bool MountPak(const std::string& pakPath);
void Unmount();

// pak がマウントされていれば true（= 出荷ゲーム）。エディタ / --validate では false（ディスクモード）。
bool InGameMode();

// ASSETS 相対パス（例 "textures/foo.png", "scenes/title.json"）でアセットを読む。
// ゲームモード: pak から復号 + 展開。ディスクモード: AssetsDir のルーズファイル。
// ミス / 失敗時は空ベクタ。
std::vector<uint8_t> ReadAsset(const std::string& relPath);

// 絶対パス（ローダが AssetsDir()+rel で組むもの）を受け、AssetsDir プレフィックスを剥がして
// 相対キーを復元してから ReadAsset する。テクスチャ系の wstring オーバーロードも提供。
std::vector<uint8_t> ReadAssetAbs(const std::string& absPath);
std::vector<uint8_t> ReadAssetAbs(const std::wstring& absPath);

bool Exists(const std::string& relPath);

// 絶対パス版 Exists。ReadAssetAbs と同じプレフィックス剥がしで相対キーへ正規化してから
// 存在確認する（ゲームモード: pak の TOC 照会のみで復号しない。ディスクモード: ファイル存在）。
bool ExistsAbs(const std::string& absPath);
bool ExistsAbs(const std::wstring& absPath);

// pak の "__manifest__" エントリから読むブート設定（ゲームモード）。ディスクモードでは false。
struct BootConfig
{
    std::string title;
    std::string startScene;
    int         windowWidth  = 1280;
    int         windowHeight = 720;
};
bool ReadBootConfig(BootConfig& out);

} // namespace dx12e::vfs
