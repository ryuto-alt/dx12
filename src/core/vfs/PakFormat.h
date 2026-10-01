#pragma once
//
// game.pak のオンディスク構造 + 共有ユーティリティ（Normalize / FnvHash）。
// Writer（パック時）と Runtime（読み込み時）の両方がこの 1 ファイルを共有する。
// Normalize / FnvHash の不一致 = 静かな「アセットが見つからない」になるので、
// 唯一の正準実装としてここに置く（ヘッダオンリーで inline）。
//
#include <cstdint>
#include <string>
#include <string_view>
#include <cctype>

namespace dx12e::vfs
{

// 'G','M','P','K' をリトルエンディアンの uint32 にしたもの（0x4B504D47）。
inline constexpr uint32_t kPakMagic       = 0x4B504D47u;
inline constexpr uint32_t kPakVersion     = 1u;
inline constexpr uint32_t kEndianSentinel = 0x01020304u;

// PakEntry::flags / PakHeader::flags のビット。
inline constexpr uint16_t kEntryCompressed = 0x1; // bit0: XPRESS_HUFF 圧縮済み
inline constexpr uint16_t kEntryEncrypted  = 0x2; // bit1: AES-256-GCM 暗号化済み

inline constexpr uint32_t kHeaderAnyXpress       = 0x1; // bit0
inline constexpr uint32_t kHeaderEntriesEncrypted = 0x2; // bit1
inline constexpr uint32_t kHeaderStringsStripped = 0x4; // bit2

inline constexpr uint32_t kNoNameOff = 0xFFFFFFFFu; // 文字列テーブルが strip された場合

#pragma pack(push, 1)
struct PakHeader
{
    uint32_t magic;          // 'GMPK'
    uint32_t version;        // = 1
    uint32_t endianSentinel; // = 0x01020304
    uint32_t flags;          // bit0 any_xpress, bit1 entries_encrypted, bit2 strings_stripped
    uint64_t tocOffset;      // 先頭 PakEntry の絶対オフセット
    uint32_t entryCount;     // N
    uint32_t strtabSize;     // 文字列テーブルのバイト数（strip 時 0）
};
#pragma pack(pop)
static_assert(sizeof(PakHeader) == 32, "PakHeader must be exactly 32 bytes");

#pragma pack(push, 1)
struct PakEntry
{
    uint64_t pathHash;     // 0  : 正規化パスの FNV-1a 64
    uint64_t dataOffset;   // 8  : data セクション内の ciphertext 絶対オフセット
    uint32_t storedSize;   // 16 : ディスク上のバイト数 = ciphertext 長 = 圧縮後長
    uint32_t originalSize; // 20 : 平文（圧縮前）の長さ
    uint16_t flags;        // 24 : bit0 compressed, bit1 encrypted
    uint16_t _pad0;        // 26
    uint8_t  nonce[12];    // 28 : GCM ノンス（エントリ毎ランダム）
    uint8_t  tag[16];      // 40 : GCM 認証タグ
    uint32_t nameOff;      // 56 : 文字列テーブルへのバイトオフセット（strip 時 0xFFFFFFFF）
    uint32_t _reserved;    // 60 : 0
};                         // 64
#pragma pack(pop)
static_assert(sizeof(PakEntry) == 64, "PakEntry must be exactly 64 bytes");

// 正規化（唯一の正準関数）:
//   小文字化、'\\' -> '/'、"//" "/./" "x/../" を畳む、先頭 '/' を除去、先頭 "assets/" のみ除去。
//   "scripts/" はそのまま残す（スクリプトは assets/ の外に居るため）。
inline std::string Normalize(std::string_view path)
{
    std::string s(path);
    for (char& c : s)
    {
        if (c == '\\')
            c = '/';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // "a//b" / "a/./b" / "a/b/../c" を畳む（glTF の "../textures/x.png" や fbx のテクスチャ参照が
    // モデルのフォルダ基準で "models/arch/../tex/x.png" のまま来ても、pak のキー "models/tex/x.png" に当たるように）。
    // 先頭を越える ".." は畳めないのでそのまま残す（pak に無いキー＝ミスになる。パックの外へ出る参照を通さない）。
    // 書き込み側のキーは lexically_relative の結果で ".." も "." も含まないため、既存の pak のキーは変わらない。
    {
        std::string out;
        out.reserve(s.size());
        std::size_t pos = 0;
        std::size_t depth = 0;                  // 畳める（"..", 以外の）セグメントの数
        while (pos <= s.size())
        {
            std::size_t end = s.find('/', pos);
            if (end == std::string::npos) end = s.size();
            const std::string_view seg(s.data() + pos, end - pos);
            if (seg.empty() || seg == ".")
            {
                // 空セグメント / カレント: 捨てる
            }
            else if (seg == ".." && depth > 0)
            {
                // 直前のセグメントを消す
                const std::size_t cut = out.find_last_of('/');
                out.erase(cut == std::string::npos ? 0 : cut);
                --depth;
            }
            else
            {
                if (!out.empty()) out += '/';
                out.append(seg);
                if (seg != "..") ++depth;
            }
            pos = end + 1;
        }
        s = std::move(out);
    }
    // 先頭 "assets/" のみ除去（"scripts/" は残す）
    static constexpr char kAssetsPfx[] = "assets/";
    constexpr std::size_t kAssetsPfxLen = sizeof(kAssetsPfx) - 1;
    if (s.size() >= kAssetsPfxLen && s.compare(0, kAssetsPfxLen, kAssetsPfx) == 0)
        s.erase(0, kAssetsPfxLen);
    return s;
}

// FNV-1a 64: basis 0xcbf29ce484222325, prime 0x100000001b3。
inline uint64_t FnvHash(std::string_view normalized)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : normalized)
    {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

} // namespace dx12e::vfs
