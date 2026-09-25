#pragma once

// ユーザーごとの書き込み先（セーブ / 選んだ言語 など、ゲームが実行時に書くもの）。
//
// ★なぜ exe の隣や assets ではないか:
//   配布ゲームは Program Files 配下へ入れられると exe の隣へ書けない（settings.json が
//   実際にそれで無言で消えていた）。assets はゲームビルドで game.pak に丸ごと詰められるので、
//   開発機のセーブが配布物へ混ざる。OS のユーザー領域へ分ける。
//
//   配布ゲーム（pak マウント済み）: %LOCALAPPDATA%/<ゲーム名>/
//   エディタ / ヘッドレス          : <プロジェクト>/.dx12/   （本番のセーブを汚さない）
//
// ゲーム名は game.pak の manifest の title（ビルド設定の「ゲーム名」）。
// パスは UTF-8 で持ち、Win32 へ渡すときは必ずワイド文字列にする
// （std::filesystem::path(std::string) は ACP 解釈なので日本語のゲーム名が化ける）。

#include <filesystem>
#include <string>

namespace dx12e::save
{

// ユーザー領域のルート（末尾に区切り無し）。作成はしない（書く側が作る）。
std::filesystem::path UserDataDir();

// セーブの置き場 = UserDataDir()/saves
std::filesystem::path SavesDir();

// ゲーム名をフォルダ名に使える形へ（<>:"/\|?* と制御文字を落とし、末尾の空白とドットを削る）。
// 空になったら "DX12Game"。純関数（tests/save_file_test.cpp で固定）。
std::string SanitizeFolderName(const std::string& title);

// UTF-8 文字列 → path（ワイド経由。ACP を通さない）
std::filesystem::path PathFromUtf8(const std::string& utf8);
// path → UTF-8 文字列（'/' 区切り）
std::string PathToUtf8(const std::filesystem::path& p);

} // namespace dx12e::save
