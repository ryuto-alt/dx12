#pragma once
//
// VgsrcFormat.h ― 中間形式 VGSRC（UE cook / Blender 等 → vgeocook への入力）v1.0。
//
//   ★仕様の正本は docs/VGEO_SPEC.md §12。これはその C++ 実装（P1 の cooker が読み、P6 の C# が同じ形で書く）。
//   ★ヘッダオンリー・標準ライブラリのみ。VgeoFormat.h のエラー型 / ByteSource / ByteSink / MaterialRecord を共有する。
//
//   単純な生配列のファイル。u64 のカウントを使い、書き手は「ヘッダを空で書く → 配列を順に流す → 先頭へ戻ってヘッダを確定」で
//   メモリに載らない規模（1 億三角形）でもストリーム書きできる。
//
//   [   0] VgsrcHeader 64 B
//   [16整列] positions  f32[3] * vertexCount
//   [16整列] normals    f32[3] * vertexCount   （flags.hasNormals のとき）
//   [16整列] uv0        f32[2] * vertexCount   （flags.hasUV0 のとき）
//   [16整列] indices    u32    * indexCount    （三角形リスト。indexCount % 3 == 0）
//   [16整列] SectionRecord * sectionCount      （16 B）
//   [16整列] MaterialRecord * materialCount    （96 B。VgeoFormat.h と同一。パス文字列は下の文字列プールを参照）
//   [16整列] 文字列プール: u32 byteLength + UTF-8 バイト列（先頭 1 byte は '\0'）
//
#include "renderer/vg/VgeoFormat.h"

namespace dx12e::vg
{

inline constexpr u32 kVgsrcMagic        = 0x52534756u;   // 'V''G''S''R' を LE で読んだ u32
inline constexpr u16 kVgsrcVersionMajor = 1;
inline constexpr u16 kVgsrcVersionMinor = 0;
inline constexpr u32 kVgsrcHeaderSize   = 64;

inline constexpr u32 kVgsrcHasNormals   = 1u << 0;
inline constexpr u32 kVgsrcHasUV0       = 1u << 1;
inline constexpr u32 kVgsrcHasTangents  = 1u << 2;   // 読み飛ばす（v1 は tangent を持たない）
inline constexpr u32 kVgsrcHasColors    = 1u << 3;   // 読み飛ばす
inline constexpr u32 kVgsrcKnownFlags   = 0xFu;

inline constexpr u32 kCoordEngine = 0;   // Y up・左手系・メートル（そのまま）
inline constexpr u32 kCoordUE     = 1;   // Z up・左手系・(X 前, Y 右, Z 上)。(x,y,z)_UE → (y,z,x)_engine × unitScaleToMeters

inline constexpr u32 kVgsrcSectionForceNonVg = 1u << 0;   // VG にしない（カスタムシェーダ等）。それ以外は材質フラグから cooker が決める

#pragma pack(push, 1)
struct VgsrcHeader                        // 64 B
{
    u32 magic;                            // 0
    u16 versionMajor;                     // 4
    u16 versionMinor;                     // 6
    u32 flags;                            // 8
    u32 materialCount;                    // 12
    u32 sectionCount;                     // 16
    u32 reserved0;                        // 20
    u64 vertexCount;                      // 24
    u64 indexCount;                       // 32
    f32 unitScaleToMeters;                // 40
    u32 coordSystem;                      // 44
    u64 sourceHash;                       // 48  FNV-1a 64（バイト 64..EOF）。0 = 未計算
    u64 reserved1;                        // 56
};
struct VgsrcSection                       // 16 B
{
    u32 materialIndex;                    // 0
    u32 firstIndex;                       // 4   indices[] の要素番号（バイトではない）
    u32 indexCount;                       // 8   3 の倍数
    u32 flags;                            // 12
};
#pragma pack(pop)
static_assert(sizeof(VgsrcHeader) == 64, "VgsrcHeader size");
static_assert(offsetof(VgsrcHeader, vertexCount) == 24 && offsetof(VgsrcHeader, indexCount) == 32 &&
              offsetof(VgsrcHeader, unitScaleToMeters) == 40 && offsetof(VgsrcHeader, sourceHash) == 48, "VgsrcHeader offsets");
static_assert(sizeof(VgsrcSection) == 16, "VgsrcSection size");

struct VgsrcData
{
    u32 flags = kVgsrcHasNormals | kVgsrcHasUV0;
    f32 unitScaleToMeters = 1.0f;
    u32 coordSystem = kCoordEngine;
    u64 sourceHash = 0;                      // Write 時に 0 でなければそのまま、0 のとき computeHash なら計算して入れる
    std::vector<f32> positions;              // 3 * vertexCount
    std::vector<f32> normals;                // 3 * vertexCount（flags.hasNormals）
    std::vector<f32> uv0;                    // 2 * vertexCount（flags.hasUV0）
    std::vector<u32> indices;
    std::vector<VgsrcSection> sections;
    std::vector<MaterialRecord> materials;
    std::vector<char> strings;               // 先頭 '\0'
};

// 各配列のファイル内位置（ヘッダのカウントとフラグだけから決まる。書き手も読み手もこの関数を使う）
struct VgsrcLayout
{
    u64 positions = 0, normals = 0, uv0 = 0, indices = 0, sections = 0, materials = 0, strings = 0;   // 各配列の先頭
    u64 stringBytes = 0;                                                                                 // 文字列プールのバイト数（長さ prefix 除く）
    u64 end = 0;                                                                                         // 文字列プール末尾（= 最小ファイルサイズ。stringBytes 込み）
};
// stringBytes を渡す（ヘッダには無いので、読み手は先に materials まで読んで prefix を読む 2 段階になる。ComputeLayoutUpToStrings 参照）
inline VgsrcLayout ComputeVgsrcLayoutUpToStrings(const VgsrcHeader& h)
{
    VgsrcLayout l;
    u64 c = kVgsrcHeaderSize;
    l.positions = AlignUp(c, 16); c = l.positions + h.vertexCount * 12;
    if (h.flags & kVgsrcHasNormals) { l.normals = AlignUp(c, 16); c = l.normals + h.vertexCount * 12; }
    if (h.flags & kVgsrcHasUV0)     { l.uv0 = AlignUp(c, 16);     c = l.uv0 + h.vertexCount * 8; }
    l.indices = AlignUp(c, 16);   c = l.indices + h.indexCount * 4;
    l.sections = AlignUp(c, 16);  c = l.sections + static_cast<u64>(h.sectionCount) * sizeof(VgsrcSection);
    l.materials = AlignUp(c, 16); c = l.materials + static_cast<u64>(h.materialCount) * sizeof(MaterialRecord);
    l.strings = AlignUp(c, 16);   // ここに u32 byteLength、続いて本体
    l.end = l.strings;
    return l;
}

// ストリーム書き出し（配列をコピーせずに流す。1 億三角形でも余分なメモリを使わない）。出力バイト列は従来と同一。
// ハッシュはヘッダに要るので、先にハッシュだけの走査を 1 回行う（d.sourceHash が 0 でなく、または computeHash == false ならスキップ）。
inline VgeoError WriteVgsrc(const VgsrcData& d, ByteSink& sink, bool computeHash = true)
{
    using detail::MakeErr;
    const u64 vc = d.positions.size() / 3;
    VgsrcHeader h{};
    h.magic = kVgsrcMagic; h.versionMajor = kVgsrcVersionMajor; h.versionMinor = kVgsrcVersionMinor;
    h.flags = d.flags; h.materialCount = static_cast<u32>(d.materials.size()); h.sectionCount = static_cast<u32>(d.sections.size());
    h.vertexCount = vc; h.indexCount = d.indices.size();
    h.unitScaleToMeters = d.unitScaleToMeters; h.coordSystem = d.coordSystem;
    const VgsrcLayout l = ComputeVgsrcLayoutUpToStrings(h);

    // 本体（ヘッダ以降）を put(ptr, n) の列として流す。ハッシュ走査と書き出しで同じ列を使う。
    auto body = [&](auto&& put)
    {
        u64 cursor = kVgsrcHeaderSize;
        static const u8 kZeros[16] = {};
        auto padTo = [&](u64 abs) { while (cursor < abs) { const u64 n = std::min<u64>(abs - cursor, sizeof kZeros); put(kZeros, n); cursor += n; } };
        auto app = [&](const void* p, u64 n) { if (n) put(p, n); cursor += n; };
        padTo(l.positions);              app(d.positions.data(), d.positions.size() * 4);
        if (h.flags & kVgsrcHasNormals) { padTo(l.normals); app(d.normals.data(), d.normals.size() * 4); }
        if (h.flags & kVgsrcHasUV0)     { padTo(l.uv0);     app(d.uv0.data(), d.uv0.size() * 4); }
        padTo(l.indices);                app(d.indices.data(), d.indices.size() * 4);
        padTo(l.sections);               app(d.sections.data(), d.sections.size() * sizeof(VgsrcSection));
        padTo(l.materials);              app(d.materials.data(), d.materials.size() * sizeof(MaterialRecord));
        padTo(l.strings);
        const u32 sl = static_cast<u32>(d.strings.size());
        app(&sl, 4); app(d.strings.data(), d.strings.size());
    };
    if (d.sourceHash) h.sourceHash = d.sourceHash;
    else if (computeHash)
    {
        u64 hash = 0xcbf29ce484222325ull;
        body([&](const void* p, u64 n) { hash = Fnv1a64(p, n, hash); });
        h.sourceHash = hash;
    }
    if (!sink.Write(&h, sizeof h)) return MakeErr(Errc::IoError, "sink write failed (vgsrc)");
    bool ok = true;
    body([&](const void* p, u64 n) { if (ok) ok = sink.Write(p, n); });
    if (!ok) return MakeErr(Errc::IoError, "sink write failed (vgsrc)");
    return VgeoError{};
}

// 検証つきの全読み込み（テスト / 小さいファイル向け。巨大ファイルは ComputeVgsrcLayoutUpToStrings で位置を出して自前で読む）
inline VgeoError ReadVgsrc(const ByteSource& src, VgsrcData& out, bool verifyHash = false)
{
    using detail::MakeErr; using detail::Fmt;
    out = VgsrcData{};
    const u64 fs = src.Size();
    if (fs < kVgsrcHeaderSize) return MakeErr(Errc::Truncated, "file is smaller than the 64-byte VGSRC header");
    VgsrcHeader h;
    if (!src.Read(0, &h, sizeof h)) return MakeErr(Errc::IoError, "failed to read the VGSRC header");
    if (h.magic != kVgsrcMagic) return MakeErr(Errc::BadMagic, "not a VGSRC file");
    if (h.versionMajor != kVgsrcVersionMajor) return MakeErr(Errc::UnsupportedMajor, Fmt("VGSRC major %u is not supported", h.versionMajor));
    if (h.flags & ~kVgsrcKnownFlags) return MakeErr(Errc::UnsupportedFeature, "unknown VGSRC flags");
    if (h.coordSystem > kCoordUE) return MakeErr(Errc::UnsupportedFeature, Fmt("unknown coordSystem %u", h.coordSystem));
    if (!Finite(h.unitScaleToMeters) || !(h.unitScaleToMeters > 0.0f)) return MakeErr(Errc::BadFloat, "unitScaleToMeters must be finite and > 0");
    if (h.vertexCount > 0xFFFFFFFFull) return MakeErr(Errc::LimitExceeded, "vertexCount exceeds 2^32-1 (u32 indices)");
    if (h.indexCount % 3 != 0) return MakeErr(Errc::CountMismatch, "indexCount is not a multiple of 3");
    if (h.materialCount > kMaxMaterials) return MakeErr(Errc::LimitExceeded, "too many materials");
    // 巨大カウントで加算がオーバーフローしないよう先に上限を見る（ファイルサイズ以内でなければ即拒否）
    if (h.vertexCount > fs / 12 || h.indexCount > fs / 4 || h.sectionCount > fs / 16 || h.materialCount > fs / 96)
        return MakeErr(Errc::Truncated, "header counts exceed the file size");
    const VgsrcLayout l = ComputeVgsrcLayoutUpToStrings(h);
    if (l.strings + 4 > fs) return MakeErr(Errc::Truncated, "file ends before the string pool");
    u32 sl = 0;
    if (!src.Read(l.strings, &sl, 4)) return MakeErr(Errc::IoError, "failed to read the string pool length");
    if (l.strings + 4 + sl != fs && l.strings + 4 + sl > fs) return MakeErr(Errc::Truncated, "string pool extends beyond the file");
    if (l.strings + 4 + sl != fs) return MakeErr(Errc::SectionRange, "trailing bytes after the string pool");
    if (verifyHash && h.sourceHash != 0)
    {
        // 64 MiB ずつ読んで連鎖ハッシュ（巨大ファイルでも本体ぶんのメモリを確保しない）
        std::vector<u8> chunk(static_cast<size_t>(std::min<u64>(fs - kVgsrcHeaderSize, 64ull << 20)));
        u64 hash = 0xcbf29ce484222325ull;
        for (u64 pos = kVgsrcHeaderSize; pos < fs;)
        {
            const u64 n = std::min<u64>(fs - pos, chunk.size());
            if (!src.Read(pos, chunk.data(), n)) return MakeErr(Errc::IoError, "failed to read the body for hashing");
            hash = Fnv1a64(chunk.data(), n, hash);
            pos += n;
        }
        if (hash != h.sourceHash) return MakeErr(Errc::SectionCrcMismatch, "sourceHash mismatch");
    }
    out.flags = h.flags; out.unitScaleToMeters = h.unitScaleToMeters; out.coordSystem = h.coordSystem; out.sourceHash = h.sourceHash;
    auto rd = [&](u64 off, auto& v, u64 count, u64 elemBytes) -> bool
    {
        v.resize(static_cast<size_t>(count));
        return count == 0 || src.Read(off, v.data(), count * elemBytes);
    };
    if (!rd(l.positions, out.positions, h.vertexCount * 3, 4)) return MakeErr(Errc::IoError, "positions");
    if ((h.flags & kVgsrcHasNormals) && !rd(l.normals, out.normals, h.vertexCount * 3, 4)) return MakeErr(Errc::IoError, "normals");
    if ((h.flags & kVgsrcHasUV0) && !rd(l.uv0, out.uv0, h.vertexCount * 2, 4)) return MakeErr(Errc::IoError, "uv0");
    if (!rd(l.indices, out.indices, h.indexCount, 4)) return MakeErr(Errc::IoError, "indices");
    if (!rd(l.sections, out.sections, h.sectionCount, sizeof(VgsrcSection))) return MakeErr(Errc::IoError, "sections");
    if (!rd(l.materials, out.materials, h.materialCount, sizeof(MaterialRecord))) return MakeErr(Errc::IoError, "materials");
    if (!rd(l.strings + 4, out.strings, sl, 1)) return MakeErr(Errc::IoError, "strings");

    // 内容の検査
    for (f32 f : out.positions) if (!Finite(f)) return MakeErr(Errc::BadFloat, "a position is not finite");
    for (f32 f : out.normals)   if (!Finite(f)) return MakeErr(Errc::BadFloat, "a normal is not finite");
    for (f32 f : out.uv0)       if (!Finite(f)) return MakeErr(Errc::BadFloat, "a uv is not finite");
    for (u32 i : out.indices)   if (i >= h.vertexCount) return MakeErr(Errc::ClusterInvalid, Fmt("index %u >= vertexCount", i));
    // セクションは indices[] を先頭から隙間なく順に覆う
    u64 cursor = 0;
    for (size_t i = 0; i < out.sections.size(); ++i)
    {
        const VgsrcSection& s = out.sections[i];
        if (s.firstIndex != cursor || s.indexCount % 3 != 0 || s.indexCount == 0) return MakeErr(Errc::SectionShape, Fmt("section %zu does not tile the index array (first %u count %u expected first %llu)", i, s.firstIndex, s.indexCount, static_cast<unsigned long long>(cursor)), static_cast<i64>(i));
        if (s.materialIndex >= h.materialCount) return MakeErr(Errc::MaterialInvalid, Fmt("section %zu: materialIndex out of range", i), static_cast<i64>(i));
        cursor += s.indexCount;
    }
    if (cursor != h.indexCount) return MakeErr(Errc::SectionShape, "sections do not cover the whole index array");
    if (!out.strings.empty() && (out.strings.front() != '\0' || out.strings.back() != '\0')) return MakeErr(Errc::StringInvalid, "string pool must start and end with NUL");
    for (size_t i = 0; i < out.materials.size(); ++i)
    {
        const MaterialRecord& r = out.materials[i];
        const u32 offs[] = {r.nameOff, r.albedoPathOff, r.normalPathOff, r.metalRoughPathOff, r.emissivePathOff};
        for (u32 o : offs)
            if (o != kNone && (o >= out.strings.size() || (o > 0 && out.strings[o - 1] != '\0')))
                return MakeErr(Errc::MaterialInvalid, Fmt("material %zu: string offset %u is invalid", i, o), static_cast<i64>(i));
        if (r.sectionKind > 1) return MakeErr(Errc::MaterialInvalid, Fmt("material %zu: sectionKind", i), static_cast<i64>(i));
    }
    return VgeoError{};
}

// UE(coordSystem = kCoordUE) → エンジン空間へ。(x,y,z)_UE → (y,z,x)_engine（巡回置換: 手系・巻き順は変わらない）× unitScaleToMeters。
inline void ConvertPositionToEngine(u32 coordSystem, f32 unitScale, const f32 in[3], f32 out[3])
{
    if (coordSystem == kCoordUE) { out[0] = in[1] * unitScale; out[1] = in[2] * unitScale; out[2] = in[0] * unitScale; }
    else { out[0] = in[0] * unitScale; out[1] = in[1] * unitScale; out[2] = in[2] * unitScale; }
}
inline void ConvertNormalToEngine(u32 coordSystem, const f32 in[3], f32 out[3])
{
    if (coordSystem == kCoordUE) { out[0] = in[1]; out[1] = in[2]; out[2] = in[0]; }
    else { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; }
}

} // namespace dx12e::vg
