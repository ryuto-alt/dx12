#pragma once

// オートセーブ退避スロット（assets/scenes/.autosave/）の中身を扱う純粋な部分。
//
// なぜ Application から切り出したか: ここは「消す/消さない」を間違えると
// ユーザーの未保存作業がそのまま消える場所なので、GPU もウィンドウも要らない形にして
// テストで固定したい（tests/autosave_test.cpp）。
//
// スロットの中身:
//   scene.json … 退避したシーン本体
//   scene.nav  … ナビメッシュのサイドカー（焼いてあるときだけ）
//   meta.json  … { originPath, engineVersion, savedAtUnix }

#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dx12e::autosave
{

// スロット内のファイル名（呼び出し側が末尾 '/' 付きのディレクトリを渡す）。
inline std::string ScenePath(const std::string& dir) { return dir + "scene.json"; }
inline std::string MetaPath (const std::string& dir) { return dir + "meta.json";  }
inline std::string NavPath  (const std::string& dir) { return dir + "scene.nav";  }

// meta.json の originPath。読めない/無い/壊れているときは空文字。
inline std::string OriginPath(const std::string& dir)
{
    std::error_code ec;
    const std::string metaP = MetaPath(dir);
    if (!std::filesystem::exists(metaP, ec)) return {};
    try
    {
        std::ifstream mf(metaP, std::ios::binary);
        if (!mf) return {};
        nlohmann::json meta;
        mf >> meta;
        return meta.value("originPath", std::string());
    }
    catch (...) { return {}; }   // 壊れた meta は「無い」と同じ扱い（黙って諦める）
}

// 2 つのパスが同じファイルを指すか（区切り文字・大文字小文字・相対の揺れを吸収）。
inline bool SamePath(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::weakly_canonical(fs::path(a), ec) == fs::weakly_canonical(fs::path(b), ec);
}

// 退避が scenePath のものなら消して true。別シーンの退避／退避なしなら false（触らない）。
//
// ★別シーンなら絶対に消さないこと。そこにしか無い未保存作業を消すことになる。
inline bool DiscardIfFor(const std::string& dir, const std::string& scenePath)
{
    if (scenePath.empty()) return false;
    if (!SamePath(OriginPath(dir), scenePath)) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(ScenePath(dir), ec);
    fs::remove(MetaPath(dir),  ec);
    fs::remove(NavPath(dir),   ec);
    return true;
}

// 2 つのファイルの中身が完全に同じか。サイズ違いは読まずに false。
// 片方でも開けなければ false（＝「同じとは言い切れない」側に倒す）。
inline bool SameBytes(const std::string& a, const std::string& b)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto sa = fs::file_size(a, ec); if (ec) return false;
    const auto sb = fs::file_size(b, ec); if (ec) return false;
    if (sa != sb) return false;

    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    constexpr size_t kChunk = 64 * 1024;
    std::vector<char> ba(kChunk), bb(kChunk);
    for (;;)
    {
        fa.read(ba.data(), static_cast<std::streamsize>(kChunk));
        fb.read(bb.data(), static_cast<std::streamsize>(kChunk));
        const auto na = fa.gcount(), nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) return true;
        if (std::memcmp(ba.data(), bb.data(), static_cast<size_t>(na)) != 0) return false;
    }
}

} // namespace dx12e::autosave
