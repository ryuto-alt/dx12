#pragma once
//
// VgeoFormat.h  ―  仮想ジオメトリ(Nanite 風)のオンディスク形式 `.vgeo` v1.0 の唯一の実装。
//
//   ★仕様の正本は docs/VGEO_SPEC.md（このヘッダはその実装）。食い違ったら仕様書が正。
//   ★ヘッダオンリー・標準ライブラリのみ（GPU / D3D / DirectXMath 非依存）。tests/ の依存ゼロ構成でも動く。
//
// 含むもの:
//   1. 定数 / オンディスク構造体（`static_assert` でサイズ・オフセットを固定）
//   2. 共通部品: CRC32 / FNV-1a / ビット詰め / oct16 法線 / 球の包含 / 位置格子
//   3. クラスタ・ページの符号化(PageBuilder) と復号(DecodeCluster)
//   4. Writer（VgeoContent → バイト列 / ファイル。派生フィールドは Writer が再計算する）
//   5. Reader（ByteSource → VgeoMeta / ページ / VgeoContent）と構造化エラー
//   6. Validator（全オフセット・カウントの整合、DAG 不変条件、BVH の集約値、CRC …）
//
// 規約: リトルエンディアン固定・構造体は pack(1)・全セクションの先頭は 4096 B 整列・ページは 131072 B 固定。
//       ファイルを読むときは必ず memcpy でホスト構造体へコピーする（未整列アクセスを作らない）。
//
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dx12e::vg
{

static_assert(std::endian::native == std::endian::little, ".vgeo is little-endian only");

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// ════════════════════════════════════════════════════════════════════════════
// 1. 定数
// ════════════════════════════════════════════════════════════════════════════
inline constexpr u32 kVgeoMagic        = 0x4F454756u;  // 'V''G''E''O' を LE で読んだ u32
inline constexpr u16 kVersionMajor     = 1;
inline constexpr u16 kVersionMinor     = 0;
inline constexpr u32 kHeaderSize       = 512;
inline constexpr u32 kPageSize         = 131072;       // 128 KiB。ストリーミングの単位
inline constexpr u32 kSectionAlign     = 4096;         // 無バッファ IO のセクタ境界
inline constexpr u32 kSectionCount     = 9;
inline constexpr u32 kMaxClusterVerts  = 128;          // 形式の上限（ヘッダの maxClusterVerts はこれ以下）
inline constexpr u32 kMaxClusterTris   = 128;
inline constexpr u32 kClusterHeaderSize= 128;
inline constexpr u32 kPageHeaderSize   = 64;
inline constexpr u32 kPageMagic        = 0x47504756u;  // 'V''G''P''G'
inline constexpr u32 kMaxClustersPerPage = 256;        // clusterIndexInPage / firstCluster が 8bit
inline constexpr u32 kMaxGroupClusters = 15;           // groupPacked.clusterCount が 4bit
inline constexpr u32 kMaxPages         = 65535;        // pageIndex が 16bit（0xFFFF は予約）
inline constexpr u32 kMaxMaterials     = 65536;        // クラスタの materialIndex が 16bit
inline constexpr u32 kMaxLevels        = 255;          // クラスタの level が 8bit（0..254 を使う）
inline constexpr u32 kGridBits         = 24;
inline constexpr u32 kGridMax          = (1u << kGridBits) - 1;   // 16777215
inline constexpr u32 kNone             = 0xFFFFFFFFu;  // 「無し」（オフセット / ページ / グループ）
inline constexpr u32 kInfBits          = 0x7F800000u;  // +INF（ルートの parentLodError）
inline constexpr u32 kProxyVertexSize  = 96;           // renderer/Mesh.h の Vertex と同一レイアウト
inline constexpr u32 kProxyHeaderSize  = 32;
inline constexpr u32 kProxyEntrySize   = 64;

// SectionEntry の添字（= ヘッダ内の並び = ファイル内の並び）
enum Section : u32
{
    kSecMaterials = 0,
    kSecStrings   = 1,
    kSecNodes     = 2,
    kSecPageTable = 3,
    kSecPageDeps  = 4,
    kSecPages     = 5,
    kSecProxy     = 6,
    kSecNonVg     = 7,
    kSecDebugJson = 8,
};

// VgeoHeader::flags。下位 16bit = 「必須機能」（未知のビットが立っていたら v1 ローダは拒否）、
// 上位 16bit = 「任意のヒント」（無視してよい）。
inline constexpr u32 kFlagCompressedPages = 1u << 0;   // ページが XPRESS_HUFF 圧縮（v1 の writer は出さない）
inline constexpr u32 kFlagHasProxy        = 1u << 1;
inline constexpr u32 kFlagHasNonVg        = 1u << 2;
inline constexpr u32 kKnownRequiredFlags  = kFlagCompressedPages | kFlagHasProxy | kFlagHasNonVg;

// PageTableEntry::levelFlags のうち flags(上位16bit) のビット。PageHeader::flags と同値。
inline constexpr u32 kPageFlagPinned = 1u << 0;

// ClusterHeader::flags
inline constexpr u32 kClusterFlagLod0 = 1u << 0;
inline constexpr u32 kClusterFlagRoot = 1u << 1;

// MaterialRecord::flags
inline constexpr u32 kMatHasNormalMap    = 1u << 0;
inline constexpr u32 kMatHasMetalRough   = 1u << 1;
inline constexpr u32 kMatAlphaTest       = 1u << 2;   // NONVG
inline constexpr u32 kMatHasEmissiveTex  = 1u << 3;
inline constexpr u32 kMatDoubleSided     = 1u << 4;
inline constexpr u32 kMatBlend           = 1u << 5;   // NONVG

// ProxySectionEntry::flags
inline constexpr u32 kProxyFlagExact = 1u << 0;       // 簡略化していない完全詳細（error == 0）

// ════════════════════════════════════════════════════════════════════════════
// 2. オンディスク構造体
// ════════════════════════════════════════════════════════════════════════════
#pragma pack(push, 1)

struct SectionEntry                      // 32 B
{
    u64 offset;                          // 0   ファイル先頭から。空セクションは 0
    u64 size;                            // 8   バイト数（パディングを含まない実サイズ）
    u32 count;                           // 16  要素数（意味はセクションごと）
    u32 stride;                          // 20  要素サイズ。可変長は 0
    u32 crc32;                           // 24  0 = 未計算
    u32 reserved;                        // 28
};

struct VgeoHeader                        // 512 B
{
    u32 magic;                           // 0
    u16 versionMajor;                    // 4
    u16 versionMinor;                    // 6
    u32 headerSize;                      // 8
    u32 flags;                           // 12
    u32 maxClusterVerts;                 // 16
    u32 maxClusterTris;                  // 20
    u32 pageSize;                        // 24
    u32 pageCount;                       // 28
    u32 clusterCount;                    // 32
    u32 groupCount;                      // 36
    u32 nodeCount;                       // 40
    u32 levelCount;                      // 44
    u64 sourceTriangleCount;             // 48
    u64 sourceVertexCount;               // 56
    f32 aabbMin[3];                      // 64
    f32 aabbMax[3];                      // 76
    f32 boundingSphere[4];               // 88
    f32 posOrigin[3];                    // 104
    f32 posStep;                         // 116
    u32 materialCount;                   // 120
    u32 proxySectionCount;               // 124
    u32 nonVgSectionCount;               // 128
    f32 proxyError;                      // 132
    u32 rootNode;                        // 136
    u32 rootClusterCount;                // 140
    u32 pinnedPageCount;                 // 144
    u32 clusterMaterialMode;             // 148
    u64 sourceHash;                      // 152
    u32 cookParamsHash;                  // 160
    u32 reserved0;                       // 164
    char cooker[32];                     // 168
    SectionEntry sections[kSectionCount];// 200
    u8  reserved1[20];                   // 488
    u32 headerCrc32;                     // 508
};

struct MaterialRecord                    // 96 B
{
    u32 nameOff;                         // 0   STRINGS 内。kNone = 名前無し
    u32 flags;                           // 4
    u32 albedoPathOff;                   // 8   kNone = 無し
    u32 normalPathOff;                   // 12
    u32 metalRoughPathOff;               // 16
    u32 emissivePathOff;                 // 20
    f32 metallic;                        // 24
    f32 roughness;                       // 28
    f32 emissiveColor[3];                // 32
    f32 emissiveIntensity;               // 44
    f32 alphaCutoff;                     // 48
    f32 baseColorAlpha;                  // 52
    f32 baseColorFactor[4];              // 56
    f32 uvScaleOffset[4];                // 72
    u32 sectionKind;                     // 88  0 = VG / 1 = NONVG
    u32 reserved;                        // 92
};

struct HierChild                         // 48 B（float4 境界に 3 本）
{
    f32 cullSphere[4];                   // 0   部分木の全ジオメトリを包む球
    f32 lodSphere[4];                    // 16  部分木のメンバーの parentLodSphere(⊇ lodSphere) を包む球
    f32 minOwnError;                     // 32  メンバーの lodError の最小
    f32 maxParentError;                  // 36  メンバーの parentLodError の最大（+INF あり）
    u32 ref;                             // 40  kNone = 空 / bit31=1 葉(下位31bit=グループ番号) / bit31=0 子ノード番号
    u32 groupPacked;                     // 44  葉のみ: page[0:16) | firstCluster[16:24) | clusterCount[24:28)
};

struct HierNode                          // 192 B
{
    HierChild child[4];
};

struct PageTableEntry                    // 32 B
{
    u64 fileOffset;                      // 0
    u32 storedSize;                      // 8   ディスク上のバイト数（無圧縮 = 131072）
    u32 pageCrc32;                       // 12  展開後 131072 B 全体の CRC32（★設計書の rawSize を置換）
    u32 clusterCount;                    // 16
    u32 groupCount;                      // 20
    u32 levelFlags;                      // 24  levelMin[0:8) | levelMax[8:16) | flags[16:32)
    u32 priority;                        // 28  ルートページからの依存深さ（0 = 最優先）
};

struct PageHeader                        // 64 B
{
    u32 magic;                           // 0
    u32 pageIndex;                       // 4
    u32 clusterCount;                    // 8
    u32 groupCount;                      // 12
    u32 clusterTableOffset;              // 16  = 64
    u32 payloadOffset;                   // 20  = 64 + 128 * clusterCount
    u32 usedBytes;                       // 24  以降 131072 までゼロ詰め
    u32 levelMin;                        // 28
    u32 flags;                           // 32  bit0 = pinned
    u32 levelMax;                        // 36  （設計書では reserved。PageTableEntry と対称にするため追加）
    u8  reserved[24];                    // 40
};

struct ClusterHeader                     // 128 B。前半 64 B = カリングが読む値、後半 64 B = ラスタ/シェーディング
{
    f32 lodSphere[4];                    // 0   このクラスタを「生んだ」グループの LOD 球（LOD0 は cullSphere と同値）
    f32 parentLodSphere[4];              // 16  「消費した」グループの LOD 球（ルートは lodSphere と同値）
    f32 cullSphere[4];                   // 32  クラスタのジオメトリを包む球
    f32 lodError;                        // 48  LOD0 = 0
    f32 parentLodError;                  // 52  ルート = +INF
    u32 coneS8;                          // 56  axis.xyz s8 (byte0..2) + cutoff s8 (byte3)。/127 で復元
    f32 maxEdgeLength;                   // 60
    u32 vertexOffset;                    // 64  ページ先頭から
    u32 triangleOffset;                  // 68
    u32 packedCounts;                    // 72  vertexCount[0:8) | triangleCount[8:16) | materialIndex[16:32)
    u32 posBits;                         // 76  bx[0:5) | by[5:10) | bz[10:15)
    i32 posMin[3];                       // 80  格子単位（0..16777215）
    u32 flags;                           // 92  bit0 LOD0 / bit1 root / [8:16) level
    f32 uvBase[2];                       // 96
    f32 uvScale[2];                      // 104
    u32 childPage;                       // 112 自分を「生んだ」グループのページ（LOD0 = kNone）
    u32 childGroup;                      // 116 同グループの packed（page[0:16)|first[16:24)|count[24:28)）。LOD0 = kNone（★追加）
    u32 reserved[2];                     // 120
};

struct ProxyHeader                       // 32 B
{
    u32 sectionCount;
    u32 totalVertices;
    u32 totalIndices;
    u8  reserved[20];
};

struct ProxySectionEntry                 // 64 B
{
    u32 materialIndex;                   // 0
    u32 vertexCount;                     // 4
    u32 indexCount;                      // 8
    u32 vertexOffset;                    // 12  PROXY/NONVG セクション先頭から（16 整列）
    u32 indexOffset;                     // 16  同上（u32 インデックス）
    u32 flags;                           // 20
    f32 error;                           // 24  ソース形状からの最大偏差（アセット空間 m）
    f32 aabbMin[3];                      // 28
    f32 aabbMax[3];                      // 40
    u32 reserved[3];                     // 52
};

struct ProxyVertex                       // 96 B ＝ renderer/Mesh.h の Vertex と同一
{
    f32 position[3];                     // 0
    f32 normal[3];                       // 12
    f32 color[4];                        // 24
    f32 texCoord[2];                     // 40
    f32 tangent[4];                      // 48
    u32 boneIndices[4];                  // 64
    f32 boneWeights[4];                  // 80
};

#pragma pack(pop)

#define VG_SIZE(T, n) static_assert(sizeof(T) == (n), #T " size")
#define VG_OFF(T, m, n) static_assert(offsetof(T, m) == (n), #T "::" #m " offset")

VG_SIZE(SectionEntry, 32);
VG_OFF(SectionEntry, offset, 0);  VG_OFF(SectionEntry, size, 8);   VG_OFF(SectionEntry, count, 16);
VG_OFF(SectionEntry, stride, 20); VG_OFF(SectionEntry, crc32, 24); VG_OFF(SectionEntry, reserved, 28);

VG_SIZE(VgeoHeader, 512);
VG_OFF(VgeoHeader, magic, 0);            VG_OFF(VgeoHeader, versionMajor, 4);   VG_OFF(VgeoHeader, versionMinor, 6);
VG_OFF(VgeoHeader, headerSize, 8);       VG_OFF(VgeoHeader, flags, 12);         VG_OFF(VgeoHeader, maxClusterVerts, 16);
VG_OFF(VgeoHeader, maxClusterTris, 20);  VG_OFF(VgeoHeader, pageSize, 24);      VG_OFF(VgeoHeader, pageCount, 28);
VG_OFF(VgeoHeader, clusterCount, 32);    VG_OFF(VgeoHeader, groupCount, 36);    VG_OFF(VgeoHeader, nodeCount, 40);
VG_OFF(VgeoHeader, levelCount, 44);      VG_OFF(VgeoHeader, sourceTriangleCount, 48);
VG_OFF(VgeoHeader, sourceVertexCount, 56); VG_OFF(VgeoHeader, aabbMin, 64);     VG_OFF(VgeoHeader, aabbMax, 76);
VG_OFF(VgeoHeader, boundingSphere, 88);  VG_OFF(VgeoHeader, posOrigin, 104);    VG_OFF(VgeoHeader, posStep, 116);
VG_OFF(VgeoHeader, materialCount, 120);  VG_OFF(VgeoHeader, proxySectionCount, 124);
VG_OFF(VgeoHeader, nonVgSectionCount, 128); VG_OFF(VgeoHeader, proxyError, 132); VG_OFF(VgeoHeader, rootNode, 136);
VG_OFF(VgeoHeader, rootClusterCount, 140); VG_OFF(VgeoHeader, pinnedPageCount, 144);
VG_OFF(VgeoHeader, clusterMaterialMode, 148); VG_OFF(VgeoHeader, sourceHash, 152); VG_OFF(VgeoHeader, cookParamsHash, 160);
VG_OFF(VgeoHeader, reserved0, 164);      VG_OFF(VgeoHeader, cooker, 168);       VG_OFF(VgeoHeader, sections, 200);
VG_OFF(VgeoHeader, reserved1, 488);      VG_OFF(VgeoHeader, headerCrc32, 508);

VG_SIZE(MaterialRecord, 96);
VG_OFF(MaterialRecord, nameOff, 0);       VG_OFF(MaterialRecord, flags, 4);          VG_OFF(MaterialRecord, albedoPathOff, 8);
VG_OFF(MaterialRecord, normalPathOff, 12); VG_OFF(MaterialRecord, metalRoughPathOff, 16);
VG_OFF(MaterialRecord, emissivePathOff, 20); VG_OFF(MaterialRecord, metallic, 24);   VG_OFF(MaterialRecord, roughness, 28);
VG_OFF(MaterialRecord, emissiveColor, 32); VG_OFF(MaterialRecord, emissiveIntensity, 44);
VG_OFF(MaterialRecord, alphaCutoff, 48);  VG_OFF(MaterialRecord, baseColorAlpha, 52);
VG_OFF(MaterialRecord, baseColorFactor, 56); VG_OFF(MaterialRecord, uvScaleOffset, 72);
VG_OFF(MaterialRecord, sectionKind, 88);  VG_OFF(MaterialRecord, reserved, 92);

VG_SIZE(HierChild, 48);
VG_OFF(HierChild, cullSphere, 0); VG_OFF(HierChild, lodSphere, 16); VG_OFF(HierChild, minOwnError, 32);
VG_OFF(HierChild, maxParentError, 36); VG_OFF(HierChild, ref, 40); VG_OFF(HierChild, groupPacked, 44);
VG_SIZE(HierNode, 192);

VG_SIZE(PageTableEntry, 32);
VG_OFF(PageTableEntry, fileOffset, 0); VG_OFF(PageTableEntry, storedSize, 8); VG_OFF(PageTableEntry, pageCrc32, 12);
VG_OFF(PageTableEntry, clusterCount, 16); VG_OFF(PageTableEntry, groupCount, 20);
VG_OFF(PageTableEntry, levelFlags, 24); VG_OFF(PageTableEntry, priority, 28);

VG_SIZE(PageHeader, 64);
VG_OFF(PageHeader, magic, 0); VG_OFF(PageHeader, pageIndex, 4); VG_OFF(PageHeader, clusterCount, 8);
VG_OFF(PageHeader, groupCount, 12); VG_OFF(PageHeader, clusterTableOffset, 16); VG_OFF(PageHeader, payloadOffset, 20);
VG_OFF(PageHeader, usedBytes, 24); VG_OFF(PageHeader, levelMin, 28); VG_OFF(PageHeader, flags, 32);
VG_OFF(PageHeader, levelMax, 36); VG_OFF(PageHeader, reserved, 40);

VG_SIZE(ClusterHeader, 128);
VG_OFF(ClusterHeader, lodSphere, 0);   VG_OFF(ClusterHeader, parentLodSphere, 16); VG_OFF(ClusterHeader, cullSphere, 32);
VG_OFF(ClusterHeader, lodError, 48);   VG_OFF(ClusterHeader, parentLodError, 52);  VG_OFF(ClusterHeader, coneS8, 56);
VG_OFF(ClusterHeader, maxEdgeLength, 60); VG_OFF(ClusterHeader, vertexOffset, 64); VG_OFF(ClusterHeader, triangleOffset, 68);
VG_OFF(ClusterHeader, packedCounts, 72); VG_OFF(ClusterHeader, posBits, 76);        VG_OFF(ClusterHeader, posMin, 80);
VG_OFF(ClusterHeader, flags, 92);      VG_OFF(ClusterHeader, uvBase, 96);         VG_OFF(ClusterHeader, uvScale, 104);
VG_OFF(ClusterHeader, childPage, 112); VG_OFF(ClusterHeader, childGroup, 116);    VG_OFF(ClusterHeader, reserved, 120);

VG_SIZE(ProxyHeader, 32);
VG_SIZE(ProxySectionEntry, 64);
VG_OFF(ProxySectionEntry, materialIndex, 0); VG_OFF(ProxySectionEntry, vertexCount, 4); VG_OFF(ProxySectionEntry, indexCount, 8);
VG_OFF(ProxySectionEntry, vertexOffset, 12); VG_OFF(ProxySectionEntry, indexOffset, 16); VG_OFF(ProxySectionEntry, flags, 20);
VG_OFF(ProxySectionEntry, error, 24); VG_OFF(ProxySectionEntry, aabbMin, 28); VG_OFF(ProxySectionEntry, aabbMax, 40);
VG_OFF(ProxySectionEntry, reserved, 52);
VG_SIZE(ProxyVertex, 96);
VG_OFF(ProxyVertex, position, 0); VG_OFF(ProxyVertex, normal, 12); VG_OFF(ProxyVertex, color, 24);
VG_OFF(ProxyVertex, texCoord, 40); VG_OFF(ProxyVertex, tangent, 48); VG_OFF(ProxyVertex, boneIndices, 64);
VG_OFF(ProxyVertex, boneWeights, 80);

#undef VG_SIZE
#undef VG_OFF

static_assert(kPageHeaderSize + kMaxClustersPerPage * kClusterHeaderSize < kPageSize, "cluster table must fit in a page");
static_assert(kPageSize % kSectionAlign == 0, "pages keep section alignment");
static_assert(kMaxClusterVerts <= 255 && kMaxClusterTris <= 255, "counts are stored in 8 bits");

// ════════════════════════════════════════════════════════════════════════════
// 3. 構造化エラー
// ════════════════════════════════════════════════════════════════════════════
enum class Errc : u32
{
    Ok = 0,
    IoError,              // ファイルを開けない / 読めない
    Truncated,            // ヘッダやセクションを読む前にファイルが終わっている
    BadMagic,
    UnsupportedMajor,     // 未知の versionMajor
    BadHeaderSize,
    HeaderCrcMismatch,
    SectionCrcMismatch,
    PageCrcMismatch,
    UnsupportedFeature,   // 未知の必須フラグ / 圧縮ページ（展開関数なし）/ clusterMaterialMode != 0
    LimitExceeded,        // maxClusterVerts>128 等、形式上限の超過
    BadPageSize,
    SectionRange,         // セクションがファイル外 / 順序・重なり違反
    SectionAlign,         // 4096 整列違反
    SectionShape,         // count * stride != size など
    CountMismatch,        // ヘッダのカウントと中身が食い違う
    BadFloat,             // NaN / Inf / 負の半径など
    NodeInvalid,          // BVH のツリー構造 / ref 範囲外
    GroupInvalid,         // グループがページを分割する / 番号が規則に反する
    PageInvalid,          // ページヘッダ不整合
    ClusterInvalid,       // クラスタヘッダ / ブロック配置の不整合
    LodInvariant,         // DAG の誤差単調性 / 球の包含 / グループ内一致
    DepsInvalid,          // ページ依存表 / priority / pinned の不整合
    MaterialInvalid,
    StringInvalid,
    ProxyInvalid,
    ReservedNotZero,      // strictReserved 指定時のみ: 予約フィールドが 0 でない
};

inline const char* ErrcName(Errc e)
{
    switch (e)
    {
    case Errc::Ok: return "Ok";
    case Errc::IoError: return "IoError";
    case Errc::Truncated: return "Truncated";
    case Errc::BadMagic: return "BadMagic";
    case Errc::UnsupportedMajor: return "UnsupportedMajor";
    case Errc::BadHeaderSize: return "BadHeaderSize";
    case Errc::HeaderCrcMismatch: return "HeaderCrcMismatch";
    case Errc::SectionCrcMismatch: return "SectionCrcMismatch";
    case Errc::PageCrcMismatch: return "PageCrcMismatch";
    case Errc::UnsupportedFeature: return "UnsupportedFeature";
    case Errc::LimitExceeded: return "LimitExceeded";
    case Errc::BadPageSize: return "BadPageSize";
    case Errc::SectionRange: return "SectionRange";
    case Errc::SectionAlign: return "SectionAlign";
    case Errc::SectionShape: return "SectionShape";
    case Errc::CountMismatch: return "CountMismatch";
    case Errc::BadFloat: return "BadFloat";
    case Errc::NodeInvalid: return "NodeInvalid";
    case Errc::GroupInvalid: return "GroupInvalid";
    case Errc::PageInvalid: return "PageInvalid";
    case Errc::ClusterInvalid: return "ClusterInvalid";
    case Errc::LodInvariant: return "LodInvariant";
    case Errc::DepsInvalid: return "DepsInvalid";
    case Errc::MaterialInvalid: return "MaterialInvalid";
    case Errc::StringInvalid: return "StringInvalid";
    case Errc::ProxyInvalid: return "ProxyInvalid";
    case Errc::ReservedNotZero: return "ReservedNotZero";
    }
    return "?";
}

struct VgeoError
{
    Errc        code    = Errc::Ok;
    std::string message;               // 人が読む説明（どのフィールドが何だったか）
    u64         offset  = 0;           // 分かる場合のファイルオフセット（不明は 0）
    i64         index   = -1;          // 分かる場合の要素番号（ページ / クラスタ / ノード / セクション）
    bool        ok() const { return code == Errc::Ok; }
    std::string ToString() const
    {
        std::string s = ErrcName(code);
        if (!message.empty()) { s += ": "; s += message; }
        if (index >= 0) { s += " [index="; s += std::to_string(index); s += "]"; }
        if (offset) { s += " [offset="; s += std::to_string(offset); s += "]"; }
        return s;
    }
};

struct ValidationReport
{
    std::vector<VgeoError> issues;
    bool ok() const { return issues.empty(); }
    Errc first() const { return issues.empty() ? Errc::Ok : issues.front().code; }
    bool Has(Errc c) const { for (const auto& i : issues) if (i.code == c) return true; return false; }
};

// ════════════════════════════════════════════════════════════════════════════
// 4. 共通部品（CRC / ハッシュ / ビット / 法線 / 球 / 位置格子）
// ════════════════════════════════════════════════════════════════════════════
namespace detail
{
struct CrcTables
{
    u32 t[8][256];
    constexpr CrcTables() : t{}
    {
        for (u32 i = 0; i < 256; ++i)
        {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            t[0][i] = c;
        }
        for (u32 i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    }
};
inline constexpr CrcTables kCrc{};
} // namespace detail

// CRC-32（IEEE 802.3。反射 / 初期値 0xFFFFFFFF / 最終 XOR 0xFFFFFFFF）。Crc32("123456789") == 0xCBF43926。
inline u32 Crc32Update(u32 state, const void* data, u64 n)
{
    const u8* p = static_cast<const u8*>(data);
    const auto& t = detail::kCrc.t;
    while (n >= 8)
    {
        u32 a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= state;
        state = t[7][a & 0xFFu] ^ t[6][(a >> 8) & 0xFFu] ^ t[5][(a >> 16) & 0xFFu] ^ t[4][a >> 24]
              ^ t[3][b & 0xFFu] ^ t[2][(b >> 8) & 0xFFu] ^ t[1][(b >> 16) & 0xFFu] ^ t[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) state = (state >> 8) ^ t[0][(state ^ *p++) & 0xFFu];
    return state;
}
inline u32 Crc32(const void* data, u64 n) { return ~Crc32Update(0xFFFFFFFFu, data, n); }

// FNV-1a。sourceHash（64bit）/ cookParamsHash（32bit）に使う。
inline u64 Fnv1a64(const void* data, u64 n, u64 h = 0xcbf29ce484222325ull)
{
    const u8* p = static_cast<const u8*>(data);
    for (u64 i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
inline u32 Fnv1a32(const void* data, u64 n, u32 h = 0x811C9DC5u)
{
    const u8* p = static_cast<const u8*>(data);
    for (u64 i = 0; i < n; ++i) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

inline u32 F2U(f32 f) { u32 u; std::memcpy(&u, &f, 4); return u; }
inline f32 U2F(u32 u) { f32 f; std::memcpy(&f, &u, 4); return f; }
inline f32 PosInf() { return U2F(kInfBits); }
inline bool IsPosInf(f32 f) { return F2U(f) == kInfBits; }
inline bool Finite(f32 f) { return std::isfinite(f); }
inline u32 Align16(u32 v) { return (v + 15u) & ~15u; }
inline u64 AlignUp(u64 v, u64 a) { return (v + a - 1) / a * a; }

// ビット詰め（LSB ファースト）。PutBits の書き込み先は事前にゼロ初期化されていること。
inline void PutBits(u8* dst, u64 bitPos, u32 value, u32 nbits)
{
    if (nbits == 0) return;
    u64 v = static_cast<u64>(value) << (bitPos & 7u);
    u8* p = dst + (bitPos >> 3);
    for (; v != 0; v >>= 8) *p++ |= static_cast<u8>(v & 0xFFu);
}
inline u32 GetBits(const u8* src, u64 bitPos, u32 nbits)
{
    if (nbits == 0) return 0;
    const u8* p = src + (bitPos >> 3);
    const u32 need = (static_cast<u32>(bitPos & 7u) + nbits + 7u) / 8u;   // ≤ 4
    u64 v = 0;
    for (u32 i = 0; i < need; ++i) v |= static_cast<u64>(p[i]) << (8u * i);
    v >>= (bitPos & 7u);
    return static_cast<u32>(v & ((nbits >= 32) ? 0xFFFFFFFFull : ((1ull << nbits) - 1ull)));
}

// 八面体法線 oct16×2。下位 16bit = x(snorm16)、上位 16bit = y(snorm16)。丸めは half away from zero。
inline u32 PackSnorm16(f64 v)
{
    v = std::min(1.0, std::max(-1.0, v));
    return static_cast<u32>(static_cast<u16>(static_cast<std::int16_t>(std::lround(v * 32767.0))));
}
inline f64 UnpackSnorm16(u32 bits16)
{
    const f64 v = static_cast<f64>(static_cast<std::int16_t>(static_cast<u16>(bits16))) / 32767.0;
    return std::max(-1.0, v);
}
inline u32 EncodeOct16(f64 x, f64 y, f64 z)
{
    const f64 l1 = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (!(l1 > 0.0) || !std::isfinite(l1)) { x = 0; y = 0; z = 1; }
    else { x /= l1; y /= l1; z /= l1; }
    f64 ox = x, oy = y;
    if (z < 0.0)
    {
        ox = (1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0);
        oy = (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0);
    }
    return PackSnorm16(ox) | (PackSnorm16(oy) << 16);
}
inline void DecodeOct16(u32 packed, f32 out[3])
{
    f64 x = UnpackSnorm16(packed & 0xFFFFu), y = UnpackSnorm16(packed >> 16);
    f64 z = 1.0 - std::fabs(x) - std::fabs(y);
    if (z < 0.0)
    {
        const f64 nx = (1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0);
        const f64 ny = (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0);
        x = nx; y = ny;
    }
    const f64 l = std::sqrt(x * x + y * y + z * z);
    out[0] = static_cast<f32>(x / l); out[1] = static_cast<f32>(y / l); out[2] = static_cast<f32>(z / l);
}

// UV: uv = uvBase + (u16 / 65535) * uvScale
inline u32 EncodeUnorm16(f64 t)
{
    t = std::min(1.0, std::max(0.0, t));
    return static_cast<u32>(std::lround(t * 65535.0));
}

// 法線コーン: coneS8 = axis.x,y,z (s8, byte0..2) + cutoff (s8, byte3)。/127 で復元。cutoff=127 は「使えない(カリングしない)」。
inline u32 PackConeS8(i8 ax, i8 ay, i8 az, i8 cutoff)
{
    return static_cast<u32>(static_cast<u8>(ax)) | (static_cast<u32>(static_cast<u8>(ay)) << 8)
         | (static_cast<u32>(static_cast<u8>(az)) << 16) | (static_cast<u32>(static_cast<u8>(cutoff)) << 24);
}
inline i8 ConeByte(u32 coneS8, u32 i) { return static_cast<i8>(static_cast<u8>((coneS8 >> (8u * i)) & 0xFFu)); }

// 球 [x,y,z,r]
inline bool SphereContains(const f32 outer[4], const f32 inner[4], f64 absTol = 0.0, f64 relTol = 1e-4)
{
    const f64 dx = static_cast<f64>(outer[0]) - inner[0], dy = static_cast<f64>(outer[1]) - inner[1], dz = static_cast<f64>(outer[2]) - inner[2];
    const f64 d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const f64 tol = relTol * std::max(static_cast<f64>(outer[3]), static_cast<f64>(inner[3])) + absTol;
    return d + inner[3] <= static_cast<f64>(outer[3]) + tol;
}
// n 個の球(x,y,z,r の連続配列)を内包する球。決定的（各球の外接 AABB の中心 + 最大距離）。
inline void EnclosingSphere(const f32* spheres, u32 n, f32 out[4])
{
    if (n == 0) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    f64 lo[3], hi[3];
    for (int a = 0; a < 3; ++a) { lo[a] = 1e300; hi[a] = -1e300; }
    for (u32 i = 0; i < n; ++i)
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = std::min(lo[a], static_cast<f64>(spheres[i * 4 + a]) - spheres[i * 4 + 3]);
            hi[a] = std::max(hi[a], static_cast<f64>(spheres[i * 4 + a]) + spheres[i * 4 + 3]);
        }
    f64 c[3] = {(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5};
    f64 r = 0.0;
    for (u32 i = 0; i < n; ++i)
    {
        const f64 dx = c[0] - spheres[i * 4], dy = c[1] - spheres[i * 4 + 1], dz = c[2] - spheres[i * 4 + 2];
        r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz) + spheres[i * 4 + 3]);
    }
    out[0] = static_cast<f32>(c[0]); out[1] = static_cast<f32>(c[1]); out[2] = static_cast<f32>(c[2]);
    // float への丸めで包含が崩れないよう 1ulp 外側へ。中心の丸め誤差ぶんも半径に足す。
    f64 cerr = 0.0;
    for (int a = 0; a < 3; ++a) cerr = std::max(cerr, std::fabs(static_cast<f64>(out[a]) - c[a]));
    out[3] = std::nextafter(static_cast<f32>(r + cerr * 1.7320508), std::numeric_limits<f32>::infinity());
}

// ── 位置格子（アセット全体で共通の 24bit 格子。クラック防止の要）──────────────────
// posStep = max(extent) / 16777215（f32 へ丸めた結果 posStep*16777215 < extent になったら 1ulp 上げる）。extent == 0 なら 1.0。
inline f32 ComputePosStep(f64 maxExtent)
{
    if (!(maxExtent > 0.0)) return 1.0f;
    f32 s = static_cast<f32>(maxExtent / static_cast<f64>(kGridMax));
    if (static_cast<f64>(s) * static_cast<f64>(kGridMax) < maxExtent) s = std::nextafter(s, std::numeric_limits<f32>::infinity());
    return s;
}
// q = clamp(floor((p - origin) / step + 0.5), 0, 16777215)  ―  全部 double、格納済みの f32 origin/step を使う。
inline u32 QuantizeCoord(f32 p, f32 origin, f32 step)
{
    const f64 t = std::floor((static_cast<f64>(p) - static_cast<f64>(origin)) / static_cast<f64>(step) + 0.5);
    if (!(t > 0.0)) return 0;
    if (t >= static_cast<f64>(kGridMax)) return kGridMax;
    return static_cast<u32>(t);
}
inline f32 DequantizeCoord(u32 q, f32 origin, f32 step) { return origin + static_cast<f32>(q) * step; }

// packed 2 種
inline u32 MakeGroupPacked(u32 page, u32 firstCluster, u32 clusterCount)
{
    return (page & 0xFFFFu) | ((firstCluster & 0xFFu) << 16) | ((clusterCount & 0xFu) << 24);
}
inline u32 GroupPage(u32 gp)    { return gp & 0xFFFFu; }
inline u32 GroupFirst(u32 gp)   { return (gp >> 16) & 0xFFu; }
inline u32 GroupCount(u32 gp)   { return (gp >> 24) & 0xFu; }
inline u32 MakeClusterRef(u32 page, u32 clusterInPage) { return (page & 0xFFFFu) | ((clusterInPage & 0xFFu) << 16); }
inline u32 ClusterRefPage(u32 r)    { return r & 0xFFFFu; }
inline u32 ClusterRefIndex(u32 r)   { return (r >> 16) & 0xFFu; }
inline bool NodeRefIsLeaf(u32 ref)  { return ref != kNone && (ref & 0x80000000u) != 0; }
inline u32  MakeLeafRef(u32 groupId){ return 0x80000000u | groupId; }

inline u32 LevelMinOf(u32 levelFlags) { return levelFlags & 0xFFu; }
inline u32 LevelMaxOf(u32 levelFlags) { return (levelFlags >> 8) & 0xFFu; }
inline u32 PageFlagsOf(u32 levelFlags){ return levelFlags >> 16; }
inline u32 ClusterLevel(const ClusterHeader& h) { return (h.flags >> 8) & 0xFFu; }
inline u32 ClusterVertexCount(const ClusterHeader& h)   { return h.packedCounts & 0xFFu; }
inline u32 ClusterTriangleCount(const ClusterHeader& h) { return (h.packedCounts >> 8) & 0xFFu; }
inline u32 ClusterMaterial(const ClusterHeader& h)      { return h.packedCounts >> 16; }
inline u32 ClusterBitsX(const ClusterHeader& h) { return h.posBits & 31u; }
inline u32 ClusterBitsY(const ClusterHeader& h) { return (h.posBits >> 5) & 31u; }
inline u32 ClusterBitsZ(const ClusterHeader& h) { return (h.posBits >> 10) & 31u; }

// ── ブロックサイズ（仕様 §6.3）──────────────────────────────────────────────
inline u32 PosBlockBytes(u32 vc, u32 bpp)    { return Align16(static_cast<u32>((static_cast<u64>(vc) * bpp + 7) / 8)); }
inline u32 VertexBlockBytes(u32 vc, u32 bpp) { return PosBlockBytes(vc, bpp) + 2u * Align16(4u * vc); }
inline u32 TriBlockBytes(u32 tc)             { return Align16(3u * tc); }

// ════════════════════════════════════════════════════════════════════════════
// 5. クラスタ / ページの符号化と復号
// ════════════════════════════════════════════════════════════════════════════
// エンコーダへの入力 1 クラスタ。位置は「アセット共通格子の整数座標」で渡す（QuantizeCoord で作る）。
struct ClusterSource
{
    f32 lodSphere[4]       = {0, 0, 0, 0};
    f32 parentLodSphere[4] = {0, 0, 0, 0};
    f32 cullSphere[4]      = {0, 0, 0, 0};
    f32 lodError           = 0.0f;
    f32 parentLodError     = std::numeric_limits<f32>::infinity();
    u32 coneS8             = 0x7F000000u;   // 軸 0、cutoff 127 = カリングしない
    f32 maxEdgeLength      = 0.0f;
    u32 level              = 0;
    bool root              = true;
    u32 childPage          = kNone;
    u32 childGroup         = kNone;
    u32 materialIndex      = 0;
    std::vector<u32> q;                      // 3 * vertexCount（x,y,z の順）。各 0..16777215
    std::vector<u32> normalOct;              // vertexCount（EncodeOct16）
    std::vector<f32> uv;                     // 2 * vertexCount
    std::vector<u8>  tri;                    // 3 * triangleCount（クラスタ内頂点番号 0..vertexCount-1）
    u32 vertexCount() const   { return static_cast<u32>(q.size() / 3); }
    u32 triangleCount() const { return static_cast<u32>(tri.size() / 3); }
};

struct ClusterQuant
{
    u32 posMin[3] = {0, 0, 0};
    u32 bits[3]   = {0, 0, 0};
    f32 uvBase[2] = {0, 0};
    f32 uvScale[2]= {0, 0};
    u32 bpp() const { return bits[0] + bits[1] + bits[2]; }
};

inline ClusterQuant ComputeClusterQuant(const ClusterSource& c)
{
    ClusterQuant o;
    const u32 vc = c.vertexCount();
    u32 lo[3] = {kGridMax, kGridMax, kGridMax}, hi[3] = {0, 0, 0};
    for (u32 i = 0; i < vc; ++i)
        for (u32 a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], c.q[i * 3 + a]); hi[a] = std::max(hi[a], c.q[i * 3 + a]); }
    for (u32 a = 0; a < 3; ++a)
    {
        o.posMin[a] = (vc ? lo[a] : 0);
        o.bits[a]   = static_cast<u32>(std::bit_width(vc ? hi[a] - lo[a] : 0u));
    }
    f32 ulo[2] = {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    f32 uhi[2] = {-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max()};
    for (u32 i = 0; i < vc; ++i)
        for (u32 a = 0; a < 2; ++a) { ulo[a] = std::min(ulo[a], c.uv[i * 2 + a]); uhi[a] = std::max(uhi[a], c.uv[i * 2 + a]); }
    for (u32 a = 0; a < 2; ++a)
    {
        o.uvBase[a]  = vc ? ulo[a] : 0.0f;
        o.uvScale[a] = vc ? uhi[a] - ulo[a] : 0.0f;
    }
    return o;
}

// ページ 1 枚を組み立てる。グループ（1..15 クラスタ）は分割せず同じページへ入れる。
class PageBuilder
{
public:
    enum class AddResult { Added, NoFit, Invalid };

    explicit PageBuilder(u32 pageIndex = 0) { Reset(pageIndex); }
    void Reset(u32 pageIndex)
    {
        m_pageIndex = pageIndex; m_items.clear(); m_arena.clear(); m_groups = 0;
    }
    u32  PageIndex() const    { return m_pageIndex; }
    u32  ClusterCount() const { return static_cast<u32>(m_items.size()); }
    u32  GroupCount() const   { return m_groups; }
    bool Empty() const        { return m_items.empty(); }

    // c[0..n) を 1 グループとして追加する。入らなければ NoFit（何も追加しない）。
    // 入力が仕様外（頂点 0 / 128 超 / 三角形の番号範囲外 / 15 クラスタ超 …）なら Invalid。
    AddResult TryAddGroup(const ClusterSource* c, u32 n, u32* outFirstCluster = nullptr)
    {
        if (n == 0 || n > kMaxGroupClusters) return AddResult::Invalid;
        struct Prep { ClusterQuant qz; u32 vbytes; u32 total; };
        Prep prep[kMaxGroupClusters];
        u64 addBytes = 0;
        for (u32 i = 0; i < n; ++i)
        {
            const ClusterSource& s = c[i];
            const u32 vc = s.vertexCount(), tc = s.triangleCount();
            if (vc == 0 || vc > kMaxClusterVerts || tc == 0 || tc > kMaxClusterTris) return AddResult::Invalid;
            if (s.q.size() != static_cast<size_t>(vc) * 3 || s.normalOct.size() != vc || s.uv.size() != static_cast<size_t>(vc) * 2 || s.tri.size() != static_cast<size_t>(tc) * 3)
                return AddResult::Invalid;
            if (s.materialIndex >= kMaxMaterials || s.level >= kMaxLevels) return AddResult::Invalid;
            for (u8 t : s.tri) if (t >= vc) return AddResult::Invalid;
            for (u32 q : s.q) if (q > kGridMax) return AddResult::Invalid;
            prep[i].qz = ComputeClusterQuant(s);
            prep[i].vbytes = VertexBlockBytes(vc, prep[i].qz.bpp());
            prep[i].total = prep[i].vbytes + TriBlockBytes(tc);
            addBytes += prep[i].total;
        }
        const u64 newCount = m_items.size() + n;
        if (newCount > kMaxClustersPerPage) return AddResult::NoFit;
        if (kPageHeaderSize + newCount * kClusterHeaderSize + m_arena.size() + addBytes > kPageSize) return AddResult::NoFit;

        if (outFirstCluster) *outFirstCluster = static_cast<u32>(m_items.size());
        for (u32 i = 0; i < n; ++i) Encode(c[i], prep[i].qz, prep[i].vbytes, prep[i].total);
        ++m_groups;
        return AddResult::Added;
    }

    // 完成した 131072 B を pages の末尾へ追記する。pinned は PageHeader::flags bit0 になる。
    void FinishAppend(std::vector<u8>& pages, bool pinned) const
    {
        const size_t base = pages.size();
        pages.resize(base + kPageSize, 0);
        u8* page = pages.data() + base;
        const u32 n = static_cast<u32>(m_items.size());
        const u32 payloadOffset = kPageHeaderSize + kClusterHeaderSize * n;
        PageHeader ph{};
        ph.magic = kPageMagic; ph.pageIndex = m_pageIndex; ph.clusterCount = n; ph.groupCount = m_groups;
        ph.clusterTableOffset = kPageHeaderSize; ph.payloadOffset = payloadOffset;
        ph.usedBytes = payloadOffset + static_cast<u32>(m_arena.size());
        u32 lvMin = 255, lvMax = 0;
        u32 off = payloadOffset;
        for (u32 i = 0; i < n; ++i)
        {
            ClusterHeader h = m_items[i].h;
            h.vertexOffset = off;
            h.triangleOffset = off + m_items[i].vbytes;
            std::memcpy(page + kPageHeaderSize + static_cast<size_t>(i) * kClusterHeaderSize, &h, sizeof h);
            std::memcpy(page + off, m_arena.data() + m_items[i].arenaOff, m_items[i].total);
            off += m_items[i].total;
            lvMin = std::min(lvMin, ClusterLevel(h)); lvMax = std::max(lvMax, ClusterLevel(h));
        }
        if (n == 0) lvMin = 0;
        ph.levelMin = lvMin; ph.levelMax = lvMax;
        ph.flags = pinned ? kPageFlagPinned : 0;
        std::memcpy(page, &ph, sizeof ph);
    }

private:
    struct Item { ClusterHeader h; u32 arenaOff; u32 vbytes; u32 total; };

    void Encode(const ClusterSource& s, const ClusterQuant& qz, u32 vbytes, u32 total)
    {
        Item it{};
        it.arenaOff = static_cast<u32>(m_arena.size());
        it.vbytes = vbytes; it.total = total;
        m_arena.resize(m_arena.size() + total, 0);
        u8* blk = m_arena.data() + it.arenaOff;
        const u32 vc = s.vertexCount(), tc = s.triangleCount(), bpp = qz.bpp();
        // 位置ストリーム
        u64 bit = 0;
        for (u32 i = 0; i < vc; ++i)
            for (u32 a = 0; a < 3; ++a) { PutBits(blk, bit, s.q[i * 3 + a] - qz.posMin[a], qz.bits[a]); bit += qz.bits[a]; }
        // 法線 / UV ストリーム
        const u32 posBytes = PosBlockBytes(vc, bpp);
        for (u32 i = 0; i < vc; ++i) std::memcpy(blk + posBytes + 4u * i, &s.normalOct[i], 4);
        const u32 uvOff = posBytes + Align16(4u * vc);
        for (u32 i = 0; i < vc; ++i)
        {
            const f64 u = qz.uvScale[0] > 0.0f ? (static_cast<f64>(s.uv[i * 2]) - qz.uvBase[0]) / qz.uvScale[0] : 0.0;
            const f64 v = qz.uvScale[1] > 0.0f ? (static_cast<f64>(s.uv[i * 2 + 1]) - qz.uvBase[1]) / qz.uvScale[1] : 0.0;
            const u32 packed = EncodeUnorm16(u) | (EncodeUnorm16(v) << 16);
            std::memcpy(blk + uvOff + 4u * i, &packed, 4);
        }
        // 三角形ブロック
        std::memcpy(blk + vbytes, s.tri.data(), s.tri.size());

        ClusterHeader& h = it.h;
        std::memset(&h, 0, sizeof h);
        std::memcpy(h.lodSphere, s.lodSphere, 16);
        std::memcpy(h.parentLodSphere, s.parentLodSphere, 16);
        std::memcpy(h.cullSphere, s.cullSphere, 16);
        h.lodError = s.lodError; h.parentLodError = s.parentLodError;
        h.coneS8 = s.coneS8; h.maxEdgeLength = s.maxEdgeLength;
        h.packedCounts = vc | (tc << 8) | (s.materialIndex << 16);
        h.posBits = qz.bits[0] | (qz.bits[1] << 5) | (qz.bits[2] << 10);
        for (int a = 0; a < 3; ++a) h.posMin[a] = static_cast<i32>(qz.posMin[a]);
        h.flags = (s.level == 0 ? kClusterFlagLod0 : 0u) | (s.root ? kClusterFlagRoot : 0u) | (s.level << 8);
        std::memcpy(h.uvBase, qz.uvBase, 8);
        std::memcpy(h.uvScale, qz.uvScale, 8);
        h.childPage = s.childPage; h.childGroup = s.childGroup;
        m_items.push_back(it);
    }

    u32 m_pageIndex = 0;
    u32 m_groups = 0;
    std::vector<Item> m_items;
    std::vector<u8>   m_arena;
};

inline ClusterHeader ReadClusterHeader(const u8* page, u32 clusterIndex)
{
    ClusterHeader h;
    std::memcpy(&h, page + kPageHeaderSize + static_cast<size_t>(clusterIndex) * kClusterHeaderSize, sizeof h);
    return h;
}
inline PageHeader ReadPageHeader(const u8* page)
{
    PageHeader h;
    std::memcpy(&h, page, sizeof h);
    return h;
}

// 復号結果（CPU 参照 / テスト / ツール用）。GPU 側の HLSL デコーダはこれと一致させる。
struct DecodedCluster
{
    ClusterHeader h{};
    u32 vertexCount = 0, triangleCount = 0;
    std::vector<u32> q;         // 3 * vc
    std::vector<f32> pos;       // 3 * vc（origin + q * step）
    std::vector<f32> normal;    // 3 * vc
    std::vector<f32> uv;        // 2 * vc
    std::vector<u8>  tri;       // 3 * tc
};

// 範囲検査つき。壊れたヘッダなら false。
inline bool DecodeCluster(const u8* page, u32 clusterIndex, const f32 origin[3], f32 step, DecodedCluster& out)
{
    const PageHeader ph = ReadPageHeader(page);
    if (clusterIndex >= ph.clusterCount || ph.clusterCount > kMaxClustersPerPage) return false;
    out.h = ReadClusterHeader(page, clusterIndex);
    const ClusterHeader& h = out.h;
    const u32 vc = ClusterVertexCount(h), tc = ClusterTriangleCount(h);
    const u32 bx = ClusterBitsX(h), by = ClusterBitsY(h), bz = ClusterBitsZ(h);
    if (vc == 0 || tc == 0 || bx > kGridBits || by > kGridBits || bz > kGridBits) return false;
    const u32 bpp = bx + by + bz;
    const u32 vbytes = VertexBlockBytes(vc, bpp);
    if (static_cast<u64>(h.vertexOffset) + vbytes > kPageSize) return false;
    if (static_cast<u64>(h.triangleOffset) + TriBlockBytes(tc) > kPageSize) return false;
    out.vertexCount = vc; out.triangleCount = tc;
    out.q.assign(static_cast<size_t>(vc) * 3, 0);
    out.pos.assign(static_cast<size_t>(vc) * 3, 0.0f);
    out.normal.assign(static_cast<size_t>(vc) * 3, 0.0f);
    out.uv.assign(static_cast<size_t>(vc) * 2, 0.0f);
    const u8* vb = page + h.vertexOffset;
    u64 bit = 0;
    const u32 bits[3] = {bx, by, bz};
    for (u32 i = 0; i < vc; ++i)
        for (u32 a = 0; a < 3; ++a)
        {
            const u32 q = static_cast<u32>(h.posMin[a]) + GetBits(vb, bit, bits[a]);
            bit += bits[a];
            out.q[i * 3 + a] = q;
            out.pos[i * 3 + a] = DequantizeCoord(q, origin[a], step);
        }
    const u32 posBytes = PosBlockBytes(vc, bpp);
    for (u32 i = 0; i < vc; ++i)
    {
        u32 n, t;
        std::memcpy(&n, vb + posBytes + 4u * i, 4);
        std::memcpy(&t, vb + posBytes + Align16(4u * vc) + 4u * i, 4);
        DecodeOct16(n, &out.normal[static_cast<size_t>(i) * 3]);
        out.uv[i * 2]     = static_cast<f32>(h.uvBase[0] + static_cast<f64>(t & 0xFFFFu) / 65535.0 * h.uvScale[0]);
        out.uv[i * 2 + 1] = static_cast<f32>(h.uvBase[1] + static_cast<f64>(t >> 16) / 65535.0 * h.uvScale[1]);
    }
    out.tri.assign(page + h.triangleOffset, page + h.triangleOffset + static_cast<size_t>(tc) * 3);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// 6. 内容モデル / 文字列プール / Writer
// ════════════════════════════════════════════════════════════════════════════
namespace detail
{
inline std::string Fmt(const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return std::string(buf);
}
inline VgeoError MakeErr(Errc c, std::string msg, i64 index = -1, u64 off = 0)
{
    VgeoError e;
    e.code = c; e.message = std::move(msg); e.index = index; e.offset = off;
    return e;
}
template <class T>
inline void AppendPod(std::vector<u8>& out, const T* p, size_t n)
{
    const size_t old = out.size();
    out.resize(old + sizeof(T) * n);
    if (n) std::memcpy(out.data() + old, p, sizeof(T) * n);
}
inline void PadTo(std::vector<u8>& out, size_t align)
{
    out.resize((out.size() + align - 1) / align * align, 0);
}
} // namespace detail

// PROXY / NONVG セクションの 1 要素（エンジンの Mesh::Initialize にそのまま渡せる 96 B 頂点 + u32 インデックス）
struct ProxySectionData
{
    u32 materialIndex = 0;
    u32 flags         = 0;          // kProxyFlagExact
    f32 error         = 0.0f;
    f32 aabbMin[3]    = {0, 0, 0};
    f32 aabbMax[3]    = {0, 0, 0};
    std::vector<ProxyVertex> vertices;
    std::vector<u32>         indices;
};

// ファイル 1 個ぶんの中身。Writer の入力であり、LoadContent の出力でもある。
//   Writer が「入力として読む」メタ: header の maxClusterVerts / maxClusterTris / flags の上位16bit /
//     sourceTriangleCount / sourceVertexCount / aabb* / boundingSphere / posOrigin / posStep /
//     sourceHash / cookParamsHash / cooker / pinnedPageCount
//   それ以外のヘッダ値（カウント・セクション表・CRC・proxyError・rootClusterCount …）は Writer が中身から再計算する。
struct VgeoContent
{
    VgeoHeader                  header{};
    std::vector<MaterialRecord> materials;
    std::vector<char>           strings;      // 空でなければ先頭 1 byte は '\0'（offset 0 = 空文字列）
    std::vector<HierNode>       nodes;
    std::vector<u8>             pages;        // pageCount * kPageSize（PageBuilder::FinishAppend の連結）
    std::vector<ProxySectionData> proxy;
    std::vector<ProxySectionData> nonvg;
    std::string                 debugJson;
};

// 重複を畳む文字列プール。offset 0 は常に空文字列。
class StringPool
{
public:
    StringPool() { m_data.push_back('\0'); m_map.emplace(std::string(), 0u); }
    u32 Add(std::string_view s)
    {
        auto it = m_map.find(std::string(s));
        if (it != m_map.end()) return it->second;
        const u32 off = static_cast<u32>(m_data.size());
        m_data.insert(m_data.end(), s.begin(), s.end());
        m_data.push_back('\0');
        m_map.emplace(std::string(s), off);
        return off;
    }
    const std::vector<char>& Data() const { return m_data; }
private:
    std::vector<char> m_data;
    std::unordered_map<std::string, u32> m_map;
};

inline MaterialRecord MakeDefaultMaterial()
{
    MaterialRecord m{};
    m.nameOff = m.albedoPathOff = m.normalPathOff = m.metalRoughPathOff = m.emissivePathOff = kNone;
    m.metallic = 0.0f; m.roughness = 0.5f;
    m.baseColorAlpha = 1.0f;
    m.baseColorFactor[0] = m.baseColorFactor[1] = m.baseColorFactor[2] = m.baseColorFactor[3] = 1.0f;
    m.uvScaleOffset[0] = 1.0f; m.uvScaleOffset[1] = 1.0f;
    m.alphaCutoff = 0.5f;
    return m;
}

// ── 出力先 ────────────────────────────────────────────────────────────────────
class ByteSink
{
public:
    virtual ~ByteSink() = default;
    virtual bool Write(const void* data, u64 n) = 0;
};
class VectorSink final : public ByteSink
{
public:
    explicit VectorSink(std::vector<u8>& v) : m_v(v) {}
    bool Write(const void* d, u64 n) override
    {
        const u8* p = static_cast<const u8*>(d);
        m_v.insert(m_v.end(), p, p + n);
        return true;
    }
private:
    std::vector<u8>& m_v;
};
class FileSink final : public ByteSink
{
public:
    FileSink() = default;
    ~FileSink() { Close(); }
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    bool Open(const std::string& path)
    {
#ifdef _WIN32
        if (fopen_s(&m_f, path.c_str(), "wb") != 0) m_f = nullptr;
#else
        m_f = std::fopen(path.c_str(), "wb");
#endif
        return m_f != nullptr;
    }
    bool Close()
    {
        if (!m_f) return true;
        const bool ok = std::fclose(m_f) == 0;
        m_f = nullptr;
        return ok;
    }
    bool Write(const void* d, u64 n) override
    {
        const u8* p = static_cast<const u8*>(d);
        while (n)
        {
            const size_t chunk = static_cast<size_t>(std::min<u64>(n, 1u << 26));
            if (std::fwrite(p, 1, chunk, m_f) != chunk) return false;
            p += chunk; n -= chunk;
        }
        return true;
    }
private:
    std::FILE* m_f = nullptr;
};

// ── PROXY / NONVG セクションの直列化（仕様 §10）─────────────────────────────────
// [ProxyHeader 32][ProxySectionEntry * n][各セクションの 頂点(16 整列) 頂点数*96 → インデックス(16 整列) 個数*4 …] 全体 16 整列
inline std::vector<u8> SerializeProxyBlob(const std::vector<ProxySectionData>& secs)
{
    std::vector<u8> out;
    if (secs.empty()) return out;
    ProxyHeader ph{};
    ph.sectionCount = static_cast<u32>(secs.size());
    for (const auto& s : secs) { ph.totalVertices += static_cast<u32>(s.vertices.size()); ph.totalIndices += static_cast<u32>(s.indices.size()); }
    out.resize(kProxyHeaderSize + kProxyEntrySize * secs.size(), 0);
    u64 cursor = out.size();
    std::vector<ProxySectionEntry> entries(secs.size());
    for (size_t i = 0; i < secs.size(); ++i)
    {
        const auto& s = secs[i];
        ProxySectionEntry e{};
        e.materialIndex = s.materialIndex;
        e.vertexCount = static_cast<u32>(s.vertices.size());
        e.indexCount = static_cast<u32>(s.indices.size());
        cursor = AlignUp(cursor, 16); e.vertexOffset = static_cast<u32>(cursor); cursor += static_cast<u64>(e.vertexCount) * kProxyVertexSize;
        cursor = AlignUp(cursor, 16); e.indexOffset = static_cast<u32>(cursor);  cursor += static_cast<u64>(e.indexCount) * 4;
        e.flags = s.flags; e.error = s.error;
        std::memcpy(e.aabbMin, s.aabbMin, 12); std::memcpy(e.aabbMax, s.aabbMax, 12);
        entries[i] = e;
    }
    cursor = AlignUp(cursor, 16);
    out.resize(static_cast<size_t>(cursor), 0);
    std::memcpy(out.data(), &ph, sizeof ph);
    std::memcpy(out.data() + kProxyHeaderSize, entries.data(), entries.size() * sizeof(ProxySectionEntry));
    for (size_t i = 0; i < secs.size(); ++i)
    {
        if (!secs[i].vertices.empty()) std::memcpy(out.data() + entries[i].vertexOffset, secs[i].vertices.data(), secs[i].vertices.size() * kProxyVertexSize);
        if (!secs[i].indices.empty())  std::memcpy(out.data() + entries[i].indexOffset, secs[i].indices.data(), secs[i].indices.size() * 4);
    }
    return out;
}

// ── ページ表 / 依存表 / ヘッダ派生値の再計算（Writer と Validator の共有）──────────────
// 全クラスタヘッダ(ページ順・ページ内順で連結)から、ページ依存の CSR と priority を導く。
//   deps[Q] = { P : ページ P のクラスタ d の childPage == Q かつ P != Q }（昇順・重複なし）
//   依存は必ず「小さいページ添字」を向く（粗いレベルのページが先）。違反したら false。
//   priority = 依存の無いページ(ルート側)を 0 とした依存の深さ。
struct DerivedDeps
{
    std::vector<u32> offsets;      // pageCount + 1
    std::vector<u32> deps;
    std::vector<u32> priority;     // pageCount
};
inline bool DeriveDepsFromHeaders(u32 pageCount, const std::vector<u32>& clusterBase, const std::vector<ClusterHeader>& all,
                                  DerivedDeps& out, VgeoError* err)
{
    std::vector<std::vector<u32>> depSets(pageCount);
    for (u32 p = 0; p < pageCount; ++p)
        for (u32 i = clusterBase[p]; i < clusterBase[p + 1]; ++i)
        {
            const u32 q = all[i].childPage;
            if (q != kNone && q < pageCount && q != p) depSets[q].push_back(p);
        }
    out.offsets.assign(pageCount + 1, 0);
    out.deps.clear();
    out.priority.assign(pageCount, 0);
    for (u32 p = 0; p < pageCount; ++p)
    {
        auto& d = depSets[p];
        std::sort(d.begin(), d.end());
        d.erase(std::unique(d.begin(), d.end()), d.end());
        out.offsets[p + 1] = out.offsets[p] + static_cast<u32>(d.size());
        out.deps.insert(out.deps.end(), d.begin(), d.end());
        u32 dmax = 0;
        for (u32 dep : d)
        {
            if (dep >= p)
            {
                if (err) *err = detail::MakeErr(Errc::DepsInvalid, detail::Fmt("page %u depends on page %u (a dependency must have a lower page index)", p, dep), p);
                return false;
            }
            dmax = std::max(dmax, out.priority[dep] + 1);
        }
        out.priority[p] = dmax;
    }
    return true;
}

struct DerivedTables
{
    std::vector<PageTableEntry> pageTable;   // fileOffset は「PAGES セクション先頭からの相対」（Writer が pagesBase を足す）
    DerivedDeps deps;
    u32 clusterCount = 0;
    u32 rootClusterCount = 0;
    u32 levelCount = 0;
};

// pages[] の各ページからページ表(fileOffset 以外) と依存 CSR を導く。ページヘッダが壊れていれば false。
inline bool DeriveTables(const std::vector<u8>& pages, u32 pinnedPageCount, DerivedTables& out, VgeoError* err)
{
    const u32 pageCount = static_cast<u32>(pages.size() / kPageSize);
    out = DerivedTables{};
    out.pageTable.resize(pageCount);
    std::vector<u32> base(pageCount + 1, 0);
    std::vector<ClusterHeader> all;
    u32 maxLevel = 0; bool anyCluster = false;
    for (u32 p = 0; p < pageCount; ++p)
    {
        const u8* page = pages.data() + static_cast<size_t>(p) * kPageSize;
        const PageHeader ph = ReadPageHeader(page);
        if (ph.magic != kPageMagic || ph.clusterCount > kMaxClustersPerPage || ph.pageIndex != p)
        {
            if (err) *err = detail::MakeErr(Errc::PageInvalid, detail::Fmt("page %u header (magic/pageIndex/clusterCount) is invalid", p), p);
            return false;
        }
        PageTableEntry& e = out.pageTable[p];
        e.fileOffset = static_cast<u64>(p) * kPageSize;
        e.storedSize = kPageSize;
        e.pageCrc32  = Crc32(page, kPageSize);
        e.clusterCount = ph.clusterCount;
        e.groupCount = ph.groupCount;
        e.levelFlags = (ph.levelMin & 0xFFu) | ((ph.levelMax & 0xFFu) << 8) | ((p < pinnedPageCount ? kPageFlagPinned : 0u) << 16);
        out.clusterCount += ph.clusterCount;
        base[p + 1] = base[p] + ph.clusterCount;
        for (u32 i = 0; i < ph.clusterCount; ++i)
        {
            const ClusterHeader h = ReadClusterHeader(page, i);
            if (h.flags & kClusterFlagRoot) ++out.rootClusterCount;
            maxLevel = std::max(maxLevel, ClusterLevel(h)); anyCluster = true;
            all.push_back(h);
        }
    }
    out.levelCount = anyCluster ? maxLevel + 1 : 0;
    if (!DeriveDepsFromHeaders(pageCount, base, all, out.deps, err)) return false;
    for (u32 p = 0; p < pageCount; ++p) out.pageTable[p].priority = out.deps.priority[p];
    return true;
}

// ── Writer ───────────────────────────────────────────────────────────────────
// content → sink。同じ content なら必ず同じバイト列（パディングは全部ゼロ、時刻や乱数は入れない）。
inline VgeoError WriteVgeo(const VgeoContent& c, ByteSink& sink)
{
    using detail::MakeErr; using detail::Fmt;
    if (c.pages.size() % kPageSize != 0) return MakeErr(Errc::PageInvalid, "pages.size() is not a multiple of 131072");
    const u32 pageCount = static_cast<u32>(c.pages.size() / kPageSize);
    if (pageCount > kMaxPages) return MakeErr(Errc::LimitExceeded, Fmt("pageCount %u > %u", pageCount, kMaxPages));
    if (c.materials.size() > kMaxMaterials) return MakeErr(Errc::LimitExceeded, "too many materials");
    if (c.header.pinnedPageCount > pageCount) return MakeErr(Errc::CountMismatch, "pinnedPageCount > pageCount");
    if (c.header.maxClusterVerts > kMaxClusterVerts || c.header.maxClusterTris > kMaxClusterTris)
        return MakeErr(Errc::LimitExceeded, "maxClusterVerts/Tris exceed 128");
    if (c.nodes.size() > 0x7FFFFFFFull) return MakeErr(Errc::LimitExceeded, "too many nodes");
    if (c.proxy.size() > 0xFFFFFFu || c.nonvg.size() > 0xFFFFFFu) return MakeErr(Errc::LimitExceeded, "too many proxy sections");

    DerivedTables dt;
    VgeoError derr;
    if (!DeriveTables(c.pages, c.header.pinnedPageCount, dt, &derr)) return derr;

    u32 groupCount = 0;
    for (const HierNode& n : c.nodes)
        for (const HierChild& ch : n.child)
            if (NodeRefIsLeaf(ch.ref)) ++groupCount;

    // 各セクションの本体を作る（PAGES は c.pages をそのまま流す）
    std::vector<u8> secBytes[kSectionCount];
    detail::AppendPod(secBytes[kSecMaterials], c.materials.data(), c.materials.size());
    detail::AppendPod(secBytes[kSecStrings], c.strings.data(), c.strings.size());
    detail::AppendPod(secBytes[kSecNodes], c.nodes.data(), c.nodes.size());
    if (pageCount > 0)
    {
        detail::AppendPod(secBytes[kSecPageDeps], dt.deps.offsets.data(), dt.deps.offsets.size());
        detail::AppendPod(secBytes[kSecPageDeps], dt.deps.deps.data(), dt.deps.deps.size());
    }
    secBytes[kSecProxy]  = SerializeProxyBlob(c.proxy);
    secBytes[kSecNonVg]  = SerializeProxyBlob(c.nonvg);
    detail::AppendPod(secBytes[kSecDebugJson], c.debugJson.data(), c.debugJson.size());

    u64 sizes[kSectionCount];
    for (u32 i = 0; i < kSectionCount; ++i) sizes[i] = secBytes[i].size();
    sizes[kSecPages] = static_cast<u64>(pageCount) * kPageSize;
    sizes[kSecPageTable] = static_cast<u64>(pageCount) * sizeof(PageTableEntry);   // 中身は PAGES の位置が決まってから作る

    VgeoHeader h{};
    h.magic = kVgeoMagic; h.versionMajor = kVersionMajor; h.versionMinor = kVersionMinor; h.headerSize = kHeaderSize;
    h.flags = (c.header.flags & 0xFFFF0000u) | (c.proxy.empty() ? 0u : kFlagHasProxy) | (c.nonvg.empty() ? 0u : kFlagHasNonVg);
    h.maxClusterVerts = c.header.maxClusterVerts ? c.header.maxClusterVerts : kMaxClusterVerts;
    h.maxClusterTris  = c.header.maxClusterTris  ? c.header.maxClusterTris  : kMaxClusterTris;
    h.pageSize = kPageSize; h.pageCount = pageCount; h.clusterCount = dt.clusterCount; h.groupCount = groupCount;
    h.nodeCount = static_cast<u32>(c.nodes.size()); h.levelCount = dt.levelCount;
    h.sourceTriangleCount = c.header.sourceTriangleCount; h.sourceVertexCount = c.header.sourceVertexCount;
    std::memcpy(h.aabbMin, c.header.aabbMin, 12); std::memcpy(h.aabbMax, c.header.aabbMax, 12);
    std::memcpy(h.boundingSphere, c.header.boundingSphere, 16);
    std::memcpy(h.posOrigin, c.header.posOrigin, 12); h.posStep = c.header.posStep;
    h.materialCount = static_cast<u32>(c.materials.size());
    h.proxySectionCount = static_cast<u32>(c.proxy.size()); h.nonVgSectionCount = static_cast<u32>(c.nonvg.size());
    f32 pe = 0.0f;
    for (const auto& s : c.proxy) pe = std::max(pe, s.error);
    h.proxyError = pe;
    h.rootNode = 0; h.rootClusterCount = dt.rootClusterCount; h.pinnedPageCount = c.header.pinnedPageCount;
    h.clusterMaterialMode = 0;
    h.sourceHash = c.header.sourceHash; h.cookParamsHash = c.header.cookParamsHash;
    std::memcpy(h.cooker, c.header.cooker, sizeof h.cooker); h.cooker[sizeof h.cooker - 1] = '\0';

    // セクション表: 4096 整列で順に置く。空セクションは offset/size/count = 0（stride は公称値）。
    static const u32 kNominalStride[kSectionCount] = {sizeof(MaterialRecord), 1, sizeof(HierNode), sizeof(PageTableEntry), 4, kPageSize, 0, 0, 1};
    const u32 counts[kSectionCount] = {
        h.materialCount, static_cast<u32>(c.strings.size()), h.nodeCount, pageCount,
        static_cast<u32>(dt.deps.deps.size()), pageCount, h.proxySectionCount, h.nonVgSectionCount, static_cast<u32>(c.debugJson.size())};
    u64 cursor = kSectionAlign;
    u64 offsets[kSectionCount] = {};
    for (u32 i = 0; i < kSectionCount; ++i)
    {
        if (sizes[i] == 0) continue;
        offsets[i] = AlignUp(cursor, kSectionAlign);
        cursor = offsets[i] + sizes[i];
    }
    for (PageTableEntry& e : dt.pageTable) e.fileOffset += offsets[kSecPages];
    detail::AppendPod(secBytes[kSecPageTable], dt.pageTable.data(), dt.pageTable.size());
    for (u32 i = 0; i < kSectionCount; ++i)
    {
        SectionEntry& e = h.sections[i];
        e.stride = kNominalStride[i];
        if (sizes[i] == 0) { e.offset = 0; e.size = 0; e.count = 0; e.crc32 = 0; continue; }
        e.offset = offsets[i]; e.size = sizes[i]; e.count = counts[i];
        e.crc32 = (i == kSecPages) ? 0u : Crc32(secBytes[i].data(), secBytes[i].size());
    }
    const u64 fileSize = AlignUp(std::max<u64>(cursor, kSectionAlign), kSectionAlign);
    h.headerCrc32 = Crc32(&h, kHeaderSize - 4);

    // 書き出し（パディングは全部ゼロ）
    static const std::array<u8, 4096> kZeros{};
    u64 pos = 0;
    auto put = [&](const void* d, u64 n) -> bool { if (!sink.Write(d, n)) return false; pos += n; return true; };
    auto padTo = [&](u64 target) -> bool
    {
        while (pos < target)
        {
            const u64 n = std::min<u64>(target - pos, kZeros.size());
            if (!put(kZeros.data(), n)) return false;
        }
        return true;
    };
    if (!put(&h, sizeof h)) return MakeErr(Errc::IoError, "sink write failed (header)");
    for (u32 i = 0; i < kSectionCount; ++i)
    {
        if (sizes[i] == 0) continue;
        if (!padTo(offsets[i])) return MakeErr(Errc::IoError, "sink write failed (padding)");
        const bool ok = (i == kSecPages) ? put(c.pages.data(), c.pages.size()) : put(secBytes[i].data(), secBytes[i].size());
        if (!ok) return MakeErr(Errc::IoError, Fmt("sink write failed (section %u)", i));
    }
    if (!padTo(fileSize)) return MakeErr(Errc::IoError, "sink write failed (tail padding)");
    return VgeoError{};
}

inline VgeoError WriteVgeoToMemory(const VgeoContent& c, std::vector<u8>& out)
{
    out.clear();
    VectorSink s(out);
    return WriteVgeo(c, s);
}
inline VgeoError WriteVgeoToFile(const VgeoContent& c, const std::string& path)
{
    FileSink f;
    if (!f.Open(path)) return detail::MakeErr(Errc::IoError, "cannot open for write: " + path);
    VgeoError e = WriteVgeo(c, f);
    if (!f.Close() && e.ok()) e = detail::MakeErr(Errc::IoError, "close failed: " + path);
    return e;
}

// ════════════════════════════════════════════════════════════════════════════
// 7. Reader（ByteSource → VgeoMeta / ページ / VgeoContent）
// ════════════════════════════════════════════════════════════════════════════
class ByteSource
{
public:
    virtual ~ByteSource() = default;
    virtual u64  Size() const = 0;
    // [offset, offset+n) を dst へ。範囲外や読み失敗は false（部分読みはしない）。
    virtual bool Read(u64 offset, void* dst, u64 n) const = 0;
};

class MemorySource final : public ByteSource
{
public:
    MemorySource(const void* data, u64 size) : m_p(static_cast<const u8*>(data)), m_n(size) {}
    explicit MemorySource(const std::vector<u8>& v) : m_p(v.data()), m_n(v.size()) {}
    u64 Size() const override { return m_n; }
    bool Read(u64 off, void* dst, u64 n) const override
    {
        if (off > m_n || n > m_n - off) return false;
        if (n) std::memcpy(dst, m_p + off, static_cast<size_t>(n));
        return true;
    }
private:
    const u8* m_p;
    u64 m_n;
};

// 1 スレッドから使う前提の単純なファイル読み（ストリーマは自前のハンドルを使う）。
class FileSource final : public ByteSource
{
public:
    FileSource() = default;
    ~FileSource() { if (m_f) std::fclose(m_f); }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    bool Open(const std::string& path)
    {
#ifdef _WIN32
        if (fopen_s(&m_f, path.c_str(), "rb") != 0) m_f = nullptr;
#else
        m_f = std::fopen(path.c_str(), "rb");
#endif
        if (!m_f) return false;
        if (Seek(0, SEEK_END) != 0) return false;
        m_size = Tell();
        return true;
    }
    u64 Size() const override { return m_size; }
    bool Read(u64 off, void* dst, u64 n) const override
    {
        if (!m_f || off > m_size || n > m_size - off) return false;
        if (Seek(static_cast<i64>(off), SEEK_SET) != 0) return false;
        u8* p = static_cast<u8*>(dst);
        while (n)
        {
            const size_t chunk = static_cast<size_t>(std::min<u64>(n, 1u << 26));
            if (std::fread(p, 1, chunk, m_f) != chunk) return false;
            p += chunk; n -= chunk;
        }
        return true;
    }
private:
    int Seek(i64 off, int whence) const
    {
#ifdef _WIN32
        return _fseeki64(m_f, off, whence);
#else
        return fseeko(m_f, static_cast<off_t>(off), whence);
#endif
    }
    u64 Tell() const
    {
#ifdef _WIN32
        return static_cast<u64>(_ftelli64(m_f));
#else
        return static_cast<u64>(ftello(m_f));
#endif
    }
    std::FILE* m_f = nullptr;
    u64 m_size = 0;
};

// 読み込み / 検証の共通オプション。
struct ReadOptions
{
    bool verifyCrc      = true;    // ヘッダ / セクション / ページの CRC32 を検証
    bool strictReserved = false;   // 予約フィールド・未使用ビットが 0 であることまで要求（cooker 出力の検査用。ローダは false）
    bool deep           = true;    // (ValidateVgeo のみ) 全クラスタの頂点/三角形をデコードして範囲・包含を検査
    u32  maxIssues      = 32;      // (ValidateVgeo のみ) 集める問題の上限。LoadMeta は最初の 1 件で止まる
    // 圧縮ページ用の展開関数(src, srcSize, dst, dstSize=131072)。未指定で圧縮ファイルなら UnsupportedFeature。
    std::function<bool(const u8*, u32, u8*, u32)> decompress;
};

// 小さいセクションを全部メモリへ載せた「ファイルの目次」。ページ本体は含まない（ReadPage で 1 枚ずつ）。
struct VgeoMeta
{
    VgeoHeader                  header{};
    std::vector<MaterialRecord> materials;
    std::vector<char>           strings;
    std::vector<HierNode>       nodes;
    std::vector<PageTableEntry> pageTable;
    std::vector<u32>            pageDepOffsets;   // pageCount + 1 個（pageCount == 0 のときは空）
    std::vector<u32>            pageDeps;
    std::string                 debugJson;
    u64                         fileSize = 0;
    // 検証済みの offset なら NUL 終端の文字列。kNone / 範囲外は空文字列。
    const char* String(u32 off) const { return (off < strings.size()) ? strings.data() + off : ""; }
    u64 PagesOffset() const { return header.sections[kSecPages].offset; }
};

namespace detail
{
struct Stop {};

struct Ctx
{
    const ByteSource& src;
    const ReadOptions& opt;
    ValidationReport rep;
    u32 limit;
    Ctx(const ByteSource& s, const ReadOptions& o, u32 lim) : src(s), opt(o), limit(std::max(1u, lim)) {}
    // 問題を記録。上限に達したら Stop を投げる。
    void Add(Errc c, std::string msg, i64 idx = -1, u64 off = 0)
    {
        rep.issues.push_back(MakeErr(c, std::move(msg), idx, off));
        if (rep.issues.size() >= limit) throw Stop{};
    }
    // 続行できない問題（構造が読めない）。必ず止める。
    [[noreturn]] void Fatal(Errc c, std::string msg, i64 idx = -1, u64 off = 0)
    {
        rep.issues.push_back(MakeErr(c, std::move(msg), idx, off));
        throw Stop{};
    }
    bool Clean() const { return rep.issues.empty(); }
};

inline bool BitsEqual(f32 a, f32 b) { return F2U(a) == F2U(b); }
inline bool Sphere4BitsEqual(const f32* a, const f32* b) { return std::memcmp(a, b, 16) == 0; }
inline bool Finite4(const f32* s) { return Finite(s[0]) && Finite(s[1]) && Finite(s[2]) && Finite(s[3]); }
inline bool IsZeroBytes(const void* p, size_t n)
{
    const u8* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) if (b[i]) return false;
    return true;
}

inline bool IsValidUtf8(const char* s, size_t n)
{
    size_t i = 0;
    while (i < n)
    {
        const u8 c = static_cast<u8>(s[i]);
        size_t len = 0; u32 cp = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k)
        {
            const u8 cc = static_cast<u8>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// .vgeo のあるフォルダ基準の相対パス（'/' 区切り、絶対パス・ドライブ・".." ・空要素を禁止）。
inline bool IsSafeRelativePath(std::string_view p)
{
    if (p.empty() || p.front() == '/' ) return false;
    for (char ch : p) if (ch == '\\' || ch == ':' || static_cast<u8>(ch) < 0x20) return false;
    size_t i = 0;
    while (i <= p.size())
    {
        const size_t j = p.find('/', i);
        const std::string_view seg = p.substr(i, (j == std::string_view::npos ? p.size() : j) - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return true;
}

inline void ReadOrFatal(Ctx& x, u64 off, void* dst, u64 n, const char* what, i64 idx = -1)
{
    if (!x.src.Read(off, dst, n)) x.Fatal(Errc::IoError, Fmt("failed to read %s (offset %llu, %llu bytes)", what, static_cast<unsigned long long>(off), static_cast<unsigned long long>(n)), idx, off);
}

// ── ヘッダの構造検査（読めなくなる類は Fatal）──────────────────────────────────
inline void CheckHeaderStruct(Ctx& x, const VgeoHeader& h, u64 fileSize)
{
    if (h.pageSize != kPageSize) x.Fatal(Errc::BadPageSize, Fmt("pageSize %u (v1 requires %u)", h.pageSize, kPageSize));
    if (h.maxClusterVerts < 3 || h.maxClusterVerts > kMaxClusterVerts)
        x.Fatal(Errc::LimitExceeded, Fmt("maxClusterVerts %u not in [3,%u]", h.maxClusterVerts, kMaxClusterVerts));
    if (h.maxClusterTris < 1 || h.maxClusterTris > kMaxClusterTris)
        x.Fatal(Errc::LimitExceeded, Fmt("maxClusterTris %u not in [1,%u]", h.maxClusterTris, kMaxClusterTris));
    if ((h.flags & 0xFFFFu) & ~kKnownRequiredFlags)
        x.Fatal(Errc::UnsupportedFeature, Fmt("unknown required feature flags 0x%04X", (h.flags & 0xFFFFu) & ~kKnownRequiredFlags));
    if (h.clusterMaterialMode != 0) x.Fatal(Errc::UnsupportedFeature, Fmt("clusterMaterialMode %u is not supported by v1", h.clusterMaterialMode));
    if (h.rootNode != 0) x.Fatal(Errc::NodeInvalid, Fmt("rootNode %u (must be 0)", h.rootNode));
    if (h.pageCount > kMaxPages) x.Fatal(Errc::LimitExceeded, Fmt("pageCount %u > %u", h.pageCount, kMaxPages));
    if (h.materialCount > kMaxMaterials) x.Fatal(Errc::LimitExceeded, Fmt("materialCount %u > %u", h.materialCount, kMaxMaterials));
    if (h.levelCount > kMaxLevels) x.Fatal(Errc::LimitExceeded, Fmt("levelCount %u > %u", h.levelCount, kMaxLevels));
    if (static_cast<u64>(h.clusterCount) > static_cast<u64>(h.pageCount) * kMaxClustersPerPage)
        x.Fatal(Errc::CountMismatch, Fmt("clusterCount %u exceeds pageCount %u * 256", h.clusterCount, h.pageCount));
    if (h.cooker[sizeof h.cooker - 1] != '\0') x.Fatal(Errc::StringInvalid, "cooker string is not NUL-terminated");

    if (h.clusterCount == 0)
    {
        if (h.pageCount || h.groupCount || h.nodeCount || h.levelCount || h.rootClusterCount || h.pinnedPageCount)
            x.Fatal(Errc::CountMismatch, "clusterCount == 0 but page/group/node/level/root/pinned counts are not all 0");
    }
    else
    {
        if (!h.pageCount || !h.groupCount || !h.nodeCount || !h.levelCount || !h.rootClusterCount || !h.pinnedPageCount)
            x.Fatal(Errc::CountMismatch, "clusterCount > 0 but page/group/node/level/root/pinned count is 0");
        if (h.groupCount > h.clusterCount || static_cast<u64>(h.groupCount) * kMaxGroupClusters < h.clusterCount)
            x.Fatal(Errc::CountMismatch, Fmt("groupCount %u is impossible for clusterCount %u", h.groupCount, h.clusterCount));
        if (h.groupCount > 4ull * h.nodeCount) x.Fatal(Errc::CountMismatch, Fmt("groupCount %u exceeds the leaf slots of %u nodes", h.groupCount, h.nodeCount));   // 悪意あるカウントでの巨大確保を防ぐ
        if (h.rootClusterCount > h.clusterCount) x.Fatal(Errc::CountMismatch, "rootClusterCount > clusterCount");
        if (h.pinnedPageCount > h.pageCount) x.Fatal(Errc::CountMismatch, "pinnedPageCount > pageCount");
        if (h.nodeCount > 0x7FFFFFFFu) x.Fatal(Errc::LimitExceeded, "nodeCount too large");
    }
    for (int a = 0; a < 3; ++a)
        if (!Finite(h.aabbMin[a]) || !Finite(h.aabbMax[a]) || !Finite(h.posOrigin[a]))
            x.Fatal(Errc::BadFloat, "non-finite aabb / posOrigin");
    if (!Finite4(h.boundingSphere) || h.boundingSphere[3] < 0.0f) x.Fatal(Errc::BadFloat, "boundingSphere is not finite / has a negative radius");
    if (!Finite(h.posStep) || !(h.posStep > 0.0f)) x.Fatal(Errc::BadFloat, "posStep must be finite and > 0");
    if (!Finite(h.proxyError) || h.proxyError < 0.0f) x.Fatal(Errc::BadFloat, "proxyError must be finite and >= 0");
    if (h.clusterCount > 0)
    {
        for (int a = 0; a < 3; ++a)
        {
            if (h.aabbMin[a] > h.aabbMax[a]) x.Fatal(Errc::BadFloat, "aabbMin > aabbMax");
            if (F2U(h.posOrigin[a]) != F2U(h.aabbMin[a])) x.Fatal(Errc::BadFloat, "posOrigin must equal aabbMin");
            const f64 reach = static_cast<f64>(h.posOrigin[a]) + static_cast<f64>(h.posStep) * static_cast<f64>(kGridMax);
            if (reach < static_cast<f64>(h.aabbMax[a]) - 0.01 * static_cast<f64>(h.posStep))
                x.Fatal(Errc::BadFloat, "posStep grid does not cover aabbMax (posOrigin + posStep * 16777215 < aabbMax)");
        }
    }
    if (((h.flags & kFlagHasProxy) != 0) != (h.proxySectionCount > 0)) x.Fatal(Errc::CountMismatch, "flags.hasProxy disagrees with proxySectionCount");
    if (((h.flags & kFlagHasNonVg) != 0) != (h.nonVgSectionCount > 0)) x.Fatal(Errc::CountMismatch, "flags.hasNonVg disagrees with nonVgSectionCount");
    if (x.opt.strictReserved)
    {
        if (h.reserved0 != 0 || !IsZeroBytes(h.reserved1, sizeof h.reserved1)) x.Add(Errc::ReservedNotZero, "header reserved bytes are not zero");
    }

    static const u32 kNominal[kSectionCount] = {sizeof(MaterialRecord), 1, sizeof(HierNode), sizeof(PageTableEntry), 4, kPageSize, 0, 0, 1};
    static const char* const kName[kSectionCount] = {"MATERIALS", "STRINGS", "NODES", "PAGE_TABLE", "PAGE_DEPS", "PAGES", "PROXY", "NONVG", "DEBUG_JSON"};
    const u32 expectCount[kSectionCount] = {h.materialCount, 0, h.nodeCount, h.pageCount, 0, h.pageCount, h.proxySectionCount, h.nonVgSectionCount, 0};
    const bool mustBeEmpty[kSectionCount] = {h.materialCount == 0, false, h.nodeCount == 0, h.pageCount == 0, h.pageCount == 0, h.pageCount == 0,
                                             h.proxySectionCount == 0, h.nonVgSectionCount == 0, false};
    u64 prevEnd = kSectionAlign;
    for (u32 i = 0; i < kSectionCount; ++i)
    {
        const SectionEntry& s = h.sections[i];
        if (x.opt.strictReserved && s.reserved != 0) x.Add(Errc::ReservedNotZero, Fmt("section %s reserved != 0", kName[i]), i);
        if (s.stride != kNominal[i]) x.Fatal(Errc::SectionShape, Fmt("%s stride %u (expected %u)", kName[i], s.stride, kNominal[i]), i);
        if (s.size == 0)
        {
            if (s.count != 0) x.Fatal(Errc::SectionShape, Fmt("%s is empty but count = %u", kName[i], s.count), i);
            if (!mustBeEmpty[i] && i != kSecStrings && i != kSecDebugJson)
                x.Fatal(Errc::CountMismatch, Fmt("%s is empty but the header says it has elements", kName[i]), i);
            continue;
        }
        if (mustBeEmpty[i]) x.Fatal(Errc::CountMismatch, Fmt("%s has data but the header says it is empty", kName[i]), i);
        if (s.offset % kSectionAlign != 0) x.Fatal(Errc::SectionAlign, Fmt("%s offset %llu is not 4096-aligned", kName[i], static_cast<unsigned long long>(s.offset)), i, s.offset);
        if (s.offset < prevEnd) x.Fatal(Errc::SectionRange, Fmt("%s offset %llu overlaps the header or the previous section (ends at %llu)", kName[i], static_cast<unsigned long long>(s.offset), static_cast<unsigned long long>(prevEnd)), i, s.offset);
        if (s.offset > fileSize || s.size > fileSize - s.offset)
            x.Fatal(Errc::SectionRange, Fmt("%s [%llu, +%llu) exceeds the file size %llu (truncated?)", kName[i], static_cast<unsigned long long>(s.offset), static_cast<unsigned long long>(s.size), static_cast<unsigned long long>(fileSize)), i, s.offset);
        prevEnd = s.offset + s.size;
        switch (i)
        {
        case kSecMaterials: case kSecNodes: case kSecPageTable:
            if (s.count != expectCount[i]) x.Fatal(Errc::CountMismatch, Fmt("%s count %u disagrees with the header (%u)", kName[i], s.count, expectCount[i]), i);
            if (s.size != static_cast<u64>(s.count) * s.stride) x.Fatal(Errc::SectionShape, Fmt("%s size %llu != count * stride", kName[i], static_cast<unsigned long long>(s.size)), i);
            break;
        case kSecStrings: case kSecDebugJson:
            if (s.count != s.size || s.size > 0xFFFFFFFFull) x.Fatal(Errc::SectionShape, Fmt("%s count must equal size (< 4 GiB)", kName[i]), i);
            break;
        case kSecPageDeps:
            if (s.size != 4ull * (static_cast<u64>(h.pageCount) + 1 + s.count)) x.Fatal(Errc::SectionShape, "PAGE_DEPS size != 4 * (pageCount + 1 + depCount)", i);
            break;
        case kSecPages:
            if (s.count != h.pageCount) x.Fatal(Errc::CountMismatch, "PAGES count disagrees with pageCount", i);
            if (!(h.flags & kFlagCompressedPages) && s.size != static_cast<u64>(h.pageCount) * kPageSize)
                x.Fatal(Errc::SectionShape, "PAGES size != pageCount * 131072", i);
            break;
        case kSecProxy: case kSecNonVg:
            if (s.count != expectCount[i]) x.Fatal(Errc::CountMismatch, Fmt("%s count %u disagrees with the header (%u)", kName[i], s.count, expectCount[i]), i);
            if (s.size < kProxyHeaderSize + static_cast<u64>(kProxyEntrySize) * s.count || (s.size % 16) != 0)
                x.Fatal(Errc::SectionShape, Fmt("%s size is too small / not 16-aligned", kName[i]), i);
            break;
        default: break;
        }
    }
}

// ── 小セクションの中身の検査 ───────────────────────────────────────────────
inline void CheckStrings(Ctx& x, const VgeoMeta& m)
{
    if (m.strings.empty()) return;
    if (m.strings.front() != '\0') x.Add(Errc::StringInvalid, "STRINGS must start with a NUL (offset 0 = empty string)");
    if (m.strings.back() != '\0') x.Add(Errc::StringInvalid, "STRINGS must end with a NUL");
    if (!IsValidUtf8(m.strings.data(), m.strings.size())) x.Add(Errc::StringInvalid, "STRINGS is not valid UTF-8");
}

inline void CheckMaterials(Ctx& x, const VgeoMeta& m)
{
    auto strOk = [&](u32 off, bool isPath, u32 mi, const char* what)
    {
        if (off == kNone) return;
        if (off >= m.strings.size() || (off > 0 && m.strings[off - 1] != '\0'))
        {
            x.Add(Errc::MaterialInvalid, Fmt("material %u: %s offset %u does not point at the start of a string", mi, what, off), mi);
            return;
        }
        if (isPath && !IsSafeRelativePath(m.strings.data() + off))
            x.Add(Errc::MaterialInvalid, Fmt("material %u: %s = \"%s\" is not a safe relative path", mi, what, m.strings.data() + off), mi);
    };
    for (u32 i = 0; i < m.materials.size(); ++i)
    {
        const MaterialRecord& r = m.materials[i];
        strOk(r.nameOff, false, i, "name");
        strOk(r.albedoPathOff, true, i, "albedo path");
        strOk(r.normalPathOff, true, i, "normal path");
        strOk(r.metalRoughPathOff, true, i, "metalRough path");
        strOk(r.emissivePathOff, true, i, "emissive path");
        if (r.sectionKind > 1) x.Add(Errc::MaterialInvalid, Fmt("material %u: sectionKind %u", i, r.sectionKind), i);
        if (r.sectionKind == 0 && (r.flags & (kMatAlphaTest | kMatBlend)))
            x.Add(Errc::MaterialInvalid, Fmt("material %u: alpha-test / blend materials must be sectionKind = 1 (NONVG)", i), i);
        const f32 fl[] = {r.metallic, r.roughness, r.emissiveColor[0], r.emissiveColor[1], r.emissiveColor[2], r.emissiveIntensity,
                          r.alphaCutoff, r.baseColorAlpha, r.baseColorFactor[0], r.baseColorFactor[1], r.baseColorFactor[2], r.baseColorFactor[3],
                          r.uvScaleOffset[0], r.uvScaleOffset[1], r.uvScaleOffset[2], r.uvScaleOffset[3]};
        for (f32 f : fl) if (!Finite(f)) { x.Add(Errc::BadFloat, Fmt("material %u has a non-finite value", i), i); break; }
        if (x.opt.strictReserved && (r.reserved != 0 || (r.flags & ~0x3Fu))) x.Add(Errc::ReservedNotZero, Fmt("material %u reserved bits are not zero", i), i);
    }
}

inline void CheckPageTableAndDeps(Ctx& x, const VgeoMeta& m)
{
    const VgeoHeader& h = m.header;
    const u32 P = h.pageCount;
    const bool compressed = (h.flags & kFlagCompressedPages) != 0;
    u64 sumClusters = 0, sumGroups = 0;
    u64 prevEnd = m.PagesOffset();
    for (u32 p = 0; p < P; ++p)
    {
        const PageTableEntry& e = m.pageTable[p];
        if (!compressed)
        {
            if (e.fileOffset != m.PagesOffset() + static_cast<u64>(p) * kPageSize || e.storedSize != kPageSize)
                x.Add(Errc::SectionRange, Fmt("page %u: fileOffset/storedSize is not pagesBase + %u * 131072 / 131072", p, p), p, e.fileOffset);
        }
        else
        {
            if (e.storedSize == 0 || e.storedSize > kPageSize || e.fileOffset < prevEnd || e.fileOffset + e.storedSize > m.PagesOffset() + h.sections[kSecPages].size)
                x.Add(Errc::SectionRange, Fmt("page %u: compressed extent is out of order / outside PAGES", p), p, e.fileOffset);
            prevEnd = e.fileOffset + e.storedSize;
        }
        if (e.clusterCount == 0 || e.clusterCount > kMaxClustersPerPage) x.Add(Errc::PageInvalid, Fmt("page %u: clusterCount %u not in [1,256]", p, e.clusterCount), p);
        if (e.groupCount == 0 || e.groupCount > e.clusterCount) x.Add(Errc::PageInvalid, Fmt("page %u: groupCount %u is impossible", p, e.groupCount), p);
        sumClusters += e.clusterCount; sumGroups += e.groupCount;
        const u32 lmin = LevelMinOf(e.levelFlags), lmax = LevelMaxOf(e.levelFlags), fl = PageFlagsOf(e.levelFlags);
        if (lmin > lmax || lmax >= h.levelCount) x.Add(Errc::PageInvalid, Fmt("page %u: level range [%u,%u] is invalid (levelCount %u)", p, lmin, lmax, h.levelCount), p);
        if (((fl & kPageFlagPinned) != 0) != (p < h.pinnedPageCount))
            x.Add(Errc::DepsInvalid, Fmt("page %u: pinned flag disagrees with pinnedPageCount %u (pinned pages are the leading pages)", p, h.pinnedPageCount), p);
        if (x.opt.strictReserved && (fl & ~kPageFlagPinned)) x.Add(Errc::ReservedNotZero, Fmt("page %u table flags reserved bits", p), p);
    }
    if (sumClusters != h.clusterCount) x.Add(Errc::CountMismatch, Fmt("sum of page clusterCount %llu != header clusterCount %u", static_cast<unsigned long long>(sumClusters), h.clusterCount));
    if (sumGroups != h.groupCount) x.Add(Errc::CountMismatch, Fmt("sum of page groupCount %llu != header groupCount %u", static_cast<unsigned long long>(sumGroups), h.groupCount));

    if (P == 0) return;
    if (m.pageDepOffsets.size() != static_cast<size_t>(P) + 1 || m.pageDepOffsets[0] != 0 || m.pageDepOffsets[P] != m.pageDeps.size())
    {
        x.Add(Errc::DepsInvalid, "PAGE_DEPS offsets[] must start at 0 and end at the dependency count");
        return;
    }
    for (u32 p = 0; p < P; ++p)
    {
        const u32 b = m.pageDepOffsets[p], e = m.pageDepOffsets[p + 1];
        if (b > e || e > m.pageDeps.size()) { x.Add(Errc::DepsInvalid, Fmt("PAGE_DEPS offsets are not monotonic at page %u", p), p); return; }
        for (u32 k = b; k < e; ++k)
        {
            if (m.pageDeps[k] >= p) x.Add(Errc::DepsInvalid, Fmt("page %u depends on page %u (must be a lower index)", p, m.pageDeps[k]), p);
            if (k > b && m.pageDeps[k] <= m.pageDeps[k - 1]) x.Add(Errc::DepsInvalid, Fmt("page %u deps are not strictly ascending", p), p);
        }
    }
}

// BVH のツリー構造とグループの分割（グループ = ページ内の連続クラスタ範囲。ページを跨がず、ページを隙間なく敷き詰める）。
struct GroupInfo { u32 page = 0, first = 0, count = 0; };

inline void CheckNodes(Ctx& x, const VgeoMeta& m, std::vector<GroupInfo>& groups)
{
    const VgeoHeader& h = m.header;
    const u32 N = h.nodeCount;
    groups.assign(h.groupCount, GroupInfo{});
    if (N == 0) return;
    std::vector<u32> parentRefs(N, 0);
    std::vector<u8> groupSeen(h.groupCount, 0);
    std::vector<std::pair<u64, u32>> order;   // (page<<8|first, groupId)
    u32 leafCount = 0;
    for (u32 n = 0; n < N; ++n)
    {
        u32 nonEmpty = 0;
        for (u32 c = 0; c < 4; ++c)
        {
            const HierChild& ch = m.nodes[n].child[c];
            if (ch.ref == kNone)
            {
                if (x.opt.strictReserved && !IsZeroBytes(&ch.cullSphere[0], 40) ) x.Add(Errc::ReservedNotZero, Fmt("node %u child %u: empty slot must be zero-filled", n, c), n);
                continue;
            }
            ++nonEmpty;
            if (!Finite4(ch.cullSphere) || !Finite4(ch.lodSphere) || ch.cullSphere[3] < 0 || ch.lodSphere[3] < 0)
                x.Add(Errc::BadFloat, Fmt("node %u child %u: sphere is not finite / negative radius", n, c), n);
            if (!Finite(ch.minOwnError) || ch.minOwnError < 0.0f || std::isnan(ch.maxParentError) || ch.maxParentError < ch.minOwnError)
                x.Add(Errc::BadFloat, Fmt("node %u child %u: minOwnError/maxParentError are invalid", n, c), n);
            if (NodeRefIsLeaf(ch.ref))
            {
                const u32 g = ch.ref & 0x7FFFFFFFu;
                if (g >= h.groupCount) { x.Add(Errc::NodeInvalid, Fmt("node %u child %u: group id %u >= groupCount %u", n, c, g, h.groupCount), n); continue; }
                if (groupSeen[g]) { x.Add(Errc::NodeInvalid, Fmt("group id %u is referenced twice", g), n); continue; }
                groupSeen[g] = 1;
                const u32 page = GroupPage(ch.groupPacked), first = GroupFirst(ch.groupPacked), cnt = GroupCount(ch.groupPacked);
                if (page >= h.pageCount || cnt < 1 || cnt > kMaxGroupClusters || first + cnt > m.pageTable[page].clusterCount || (ch.groupPacked >> 28) != 0)
                {
                    x.Add(Errc::GroupInvalid, Fmt("node %u child %u: groupPacked 0x%08X is out of range (page %u first %u count %u)", n, c, ch.groupPacked, page, first, cnt), n);
                    continue;
                }
                groups[g] = GroupInfo{page, first, cnt};
                order.emplace_back((static_cast<u64>(page) << 8) | first, g);
                ++leafCount;
            }
            else
            {
                if (ch.ref <= n || ch.ref >= N) { x.Add(Errc::NodeInvalid, Fmt("node %u child %u: child node %u is out of range / not after its parent", n, c, ch.ref), n); continue; }
                ++parentRefs[ch.ref];
                if (ch.groupPacked != 0 && x.opt.strictReserved) x.Add(Errc::ReservedNotZero, Fmt("node %u child %u: interior child groupPacked must be 0", n, c), n);
            }
        }
        if (nonEmpty == 0) x.Add(Errc::NodeInvalid, Fmt("node %u has no children", n), n);
    }
    if (parentRefs[0] != 0) x.Add(Errc::NodeInvalid, "the root node is referenced as a child");
    for (u32 n = 1; n < N; ++n)
        if (parentRefs[n] != 1) x.Add(Errc::NodeInvalid, Fmt("node %u is referenced %u times (must be exactly once)", n, parentRefs[n]), n);
    if (leafCount != h.groupCount) x.Add(Errc::CountMismatch, Fmt("BVH has %u leaves but groupCount is %u", leafCount, h.groupCount));
    if (!x.Clean()) return;

    // 決定的な番号付け: group id = (page, firstCluster) 昇順の順位。ページは隙間なく group で敷き詰められる。
    std::sort(order.begin(), order.end());
    std::vector<u32> tiled(h.pageCount, 0), perPage(h.pageCount, 0);
    for (u32 rank = 0; rank < order.size(); ++rank)
    {
        const u32 g = order[rank].second;
        if (g != rank) { x.Add(Errc::GroupInvalid, Fmt("group id %u is out of order (expected %u by (page, firstCluster) order)", g, rank), g); return; }
        const GroupInfo& gi = groups[g];
        if (gi.first != tiled[gi.page]) { x.Add(Errc::GroupInvalid, Fmt("page %u: groups do not tile the page (gap or overlap at cluster %u)", gi.page, tiled[gi.page]), g); return; }
        tiled[gi.page] += gi.count; ++perPage[gi.page];
    }
    for (u32 p = 0; p < h.pageCount; ++p)
    {
        if (tiled[p] != m.pageTable[p].clusterCount) x.Add(Errc::GroupInvalid, Fmt("page %u: groups cover %u of %u clusters", p, tiled[p], m.pageTable[p].clusterCount), p);
        if (perPage[p] != m.pageTable[p].groupCount) x.Add(Errc::CountMismatch, Fmt("page %u: %u groups in the BVH but the page table says %u", p, perPage[p], m.pageTable[p].groupCount), p);
    }
}

inline void CheckSmallSections(Ctx& x, const VgeoMeta& m, std::vector<GroupInfo>& groups)
{
    CheckStrings(x, m);
    CheckMaterials(x, m);
    CheckPageTableAndDeps(x, m);
    CheckNodes(x, m, groups);
}

// ── 目次の読み込み ─────────────────────────────────────────────────────────
inline void LoadMetaImpl(Ctx& x, VgeoMeta& m, std::vector<GroupInfo>* groupsOut)
{
    const u64 fs = x.src.Size();
    m.fileSize = fs;
    if (fs < 16) x.Fatal(Errc::Truncated, Fmt("file is %llu bytes; a .vgeo header needs 512", static_cast<unsigned long long>(fs)));
    u8 first[16];
    ReadOrFatal(x, 0, first, 16, "header prefix");
    u32 magic, hsz; u16 major;
    std::memcpy(&magic, first, 4); std::memcpy(&major, first + 4, 2); std::memcpy(&hsz, first + 8, 4);
    if (magic != kVgeoMagic) x.Fatal(Errc::BadMagic, Fmt("magic 0x%08X is not 'VGEO'", magic));
    if (major != kVersionMajor) x.Fatal(Errc::UnsupportedMajor, Fmt("versionMajor %u is not supported (this reader handles %u)", major, kVersionMajor));
    if (hsz != kHeaderSize) x.Fatal(Errc::BadHeaderSize, Fmt("headerSize %u != %u", hsz, kHeaderSize));
    if (fs < kHeaderSize) x.Fatal(Errc::Truncated, Fmt("file is %llu bytes; the header needs 512", static_cast<unsigned long long>(fs)));
    VgeoHeader h;
    ReadOrFatal(x, 0, &h, sizeof h, "header");
    if (x.opt.verifyCrc && Crc32(&h, kHeaderSize - 4) != h.headerCrc32) x.Fatal(Errc::HeaderCrcMismatch, "header CRC32 mismatch");
    CheckHeaderStruct(x, h, fs);
    m.header = h;

    auto readSection = [&](u32 i, void* dst)
    {
        const SectionEntry& s = h.sections[i];
        if (s.size == 0) return;
        ReadOrFatal(x, s.offset, dst, s.size, "section", i);
        if (x.opt.verifyCrc && s.crc32 != 0 && Crc32(dst, s.size) != s.crc32)
            x.Fatal(Errc::SectionCrcMismatch, Fmt("section %u CRC32 mismatch", i), i, s.offset);
    };
    m.materials.resize(h.materialCount);   readSection(kSecMaterials, m.materials.data());
    m.strings.resize(static_cast<size_t>(h.sections[kSecStrings].size)); readSection(kSecStrings, m.strings.data());
    m.nodes.resize(h.nodeCount);           readSection(kSecNodes, m.nodes.data());
    m.pageTable.resize(h.pageCount);       readSection(kSecPageTable, m.pageTable.data());
    if (h.pageCount > 0)
    {
        std::vector<u32> raw(static_cast<size_t>(h.sections[kSecPageDeps].size / 4));
        readSection(kSecPageDeps, raw.data());
        m.pageDepOffsets.assign(raw.begin(), raw.begin() + h.pageCount + 1);
        m.pageDeps.assign(raw.begin() + h.pageCount + 1, raw.end());
    }
    m.debugJson.resize(static_cast<size_t>(h.sections[kSecDebugJson].size)); readSection(kSecDebugJson, m.debugJson.data());

    std::vector<GroupInfo> groups;
    CheckSmallSections(x, m, groups);
    if (groupsOut) *groupsOut = std::move(groups);
}
} // namespace detail

// 目次だけ読む（ページ本体は読まない）。ヘッダ・小セクション・BVH 構造を検査し、最初の問題を返す。
inline VgeoError LoadMeta(const ByteSource& src, VgeoMeta& meta, const ReadOptions& opt = {})
{
    detail::Ctx x(src, opt, 1);
    meta = VgeoMeta{};
    try { detail::LoadMetaImpl(x, meta, nullptr); }
    catch (const detail::Stop&) {}
    return x.rep.issues.empty() ? VgeoError{} : x.rep.issues.front();
}

// ページ 1 枚（展開後 131072 B）を dst へ。opt.verifyCrc なら pageCrc32 を検証する。
inline VgeoError ReadPage(const ByteSource& src, const VgeoMeta& m, u32 pageIndex, u8* dst, const ReadOptions& opt = {})
{
    using detail::MakeErr; using detail::Fmt;
    if (pageIndex >= m.header.pageCount || pageIndex >= m.pageTable.size()) return MakeErr(Errc::PageInvalid, Fmt("page index %u out of range", pageIndex), pageIndex);
    const PageTableEntry& e = m.pageTable[pageIndex];
    if (m.header.flags & kFlagCompressedPages)
    {
        if (!opt.decompress) return MakeErr(Errc::UnsupportedFeature, "pages are compressed and no decompressor was supplied", pageIndex, e.fileOffset);
        std::vector<u8> tmp(e.storedSize);
        if (!src.Read(e.fileOffset, tmp.data(), e.storedSize)) return MakeErr(Errc::IoError, "failed to read a compressed page", pageIndex, e.fileOffset);
        if (!opt.decompress(tmp.data(), e.storedSize, dst, kPageSize)) return MakeErr(Errc::PageInvalid, "decompression failed", pageIndex, e.fileOffset);
    }
    else if (!src.Read(e.fileOffset, dst, kPageSize))
        return MakeErr(Errc::IoError, "failed to read a page", pageIndex, e.fileOffset);
    if (opt.verifyCrc && Crc32(dst, kPageSize) != e.pageCrc32)
        return MakeErr(Errc::PageCrcMismatch, Fmt("page %u CRC32 mismatch", pageIndex), pageIndex, e.fileOffset);
    return VgeoError{};
}

// ════════════════════════════════════════════════════════════════════════════
// 8. Validator（ページ・クラスタ・DAG・BVH 集約・プロキシ）
// ════════════════════════════════════════════════════════════════════════════
namespace detail
{
// 1 クラスタの検査。outH に読み取ったヘッダを返す。cursor はページ内ペイロードの積み上げ位置（更新して返す）。
inline void CheckCluster(Ctx& x, const VgeoMeta& m, u32 p, u32 i, const u8* page, u32 payloadCursorIn, u32& cursorOut, ClusterHeader& h)
{
    const VgeoHeader& hd = m.header;
    h = ReadClusterHeader(page, i);
    cursorOut = payloadCursorIn;
    const i64 gidx = static_cast<i64>(p) * kMaxClustersPerPage + i;   // 報告用の「ページ*256+i」
    const u32 vc = ClusterVertexCount(h), tc = ClusterTriangleCount(h);
    const u32 bits[3] = {ClusterBitsX(h), ClusterBitsY(h), ClusterBitsZ(h)};
    auto bad = [&](Errc c, const std::string& msg) { x.Add(c, Fmt("page %u cluster %u: ", p, i) + msg, gidx); };
    bool layoutOk = true;

    if (vc == 0 || vc > hd.maxClusterVerts) { bad(Errc::ClusterInvalid, Fmt("vertexCount %u not in [1,%u]", vc, hd.maxClusterVerts)); layoutOk = false; }
    if (tc == 0 || tc > hd.maxClusterTris)  { bad(Errc::ClusterInvalid, Fmt("triangleCount %u not in [1,%u]", tc, hd.maxClusterTris)); layoutOk = false; }
    for (int a = 0; a < 3; ++a) if (bits[a] > kGridBits) { bad(Errc::ClusterInvalid, Fmt("posBits[%d] = %u > 24", a, bits[a])); layoutOk = false; }
    if (x.opt.strictReserved && (h.posBits >> 15) != 0) bad(Errc::ReservedNotZero, "posBits reserved bits are not zero");
    for (int a = 0; a < 3; ++a) if (h.posMin[a] < 0 || static_cast<u32>(h.posMin[a]) > kGridMax) bad(Errc::ClusterInvalid, Fmt("posMin[%d] = %d is outside the 24-bit grid", a, h.posMin[a]));
    const u32 mat = ClusterMaterial(h);
    if (mat >= hd.materialCount) bad(Errc::MaterialInvalid, Fmt("materialIndex %u >= materialCount %u", mat, hd.materialCount));
    else if (m.materials[mat].sectionKind != 0) bad(Errc::MaterialInvalid, Fmt("materialIndex %u is a NONVG material", mat));

    // 浮動小数
    if (!Finite4(h.lodSphere) || !Finite4(h.parentLodSphere) || !Finite4(h.cullSphere) || h.lodSphere[3] < 0 || h.parentLodSphere[3] < 0 || h.cullSphere[3] < 0)
        bad(Errc::BadFloat, "a sphere is not finite / has a negative radius");
    if (!Finite(h.lodError) || h.lodError < 0.0f) bad(Errc::BadFloat, "lodError must be finite and >= 0");
    if (std::isnan(h.parentLodError) || (!IsPosInf(h.parentLodError) && !Finite(h.parentLodError))) bad(Errc::BadFloat, "parentLodError must be finite or +INF");
    else if (h.parentLodError < h.lodError) bad(Errc::LodInvariant, Fmt("parentLodError %g < lodError %g (errors must be monotonic)", h.parentLodError, h.lodError));
    if (!Finite(h.uvBase[0]) || !Finite(h.uvBase[1]) || !Finite(h.uvScale[0]) || !Finite(h.uvScale[1]) || h.uvScale[0] < 0 || h.uvScale[1] < 0) bad(Errc::BadFloat, "uvBase/uvScale invalid");
    if (!Finite(h.maxEdgeLength) || h.maxEdgeLength < 0) bad(Errc::BadFloat, "maxEdgeLength invalid");
    for (u32 k = 0; k < 4; ++k) if (ConeByte(h.coneS8, k) == -128) { bad(Errc::ClusterInvalid, "coneS8 contains -128 (valid range is -127..127)"); break; }

    // フラグとリンク
    const u32 level = ClusterLevel(h);
    const bool lod0 = (h.flags & kClusterFlagLod0) != 0, root = (h.flags & kClusterFlagRoot) != 0;
    if (x.opt.strictReserved && (h.flags & ~(kClusterFlagLod0 | kClusterFlagRoot | 0xFF00u))) bad(Errc::ReservedNotZero, "cluster flags reserved bits are not zero");
    if (level >= hd.levelCount) bad(Errc::ClusterInvalid, Fmt("level %u >= levelCount %u", level, hd.levelCount));
    if (lod0 != (level == 0)) bad(Errc::ClusterInvalid, "flags.lod0 disagrees with level == 0");
    if (root != IsPosInf(h.parentLodError)) bad(Errc::LodInvariant, "flags.root disagrees with parentLodError == +INF");
    if (level == 0)
    {
        if (h.childPage != kNone || h.childGroup != kNone) bad(Errc::ClusterInvalid, "LOD0 cluster must have childPage = childGroup = kNone");
        if (h.lodError != 0.0f) bad(Errc::LodInvariant, "LOD0 cluster must have lodError == 0");
        if (!Sphere4BitsEqual(h.lodSphere, h.cullSphere)) bad(Errc::LodInvariant, "LOD0 cluster must have lodSphere == cullSphere");
    }
    else
    {
        if (h.childPage >= hd.pageCount) bad(Errc::ClusterInvalid, Fmt("childPage %u out of range", h.childPage));
        if (h.childGroup == kNone || GroupPage(h.childGroup) != h.childPage || GroupCount(h.childGroup) < 1 || GroupCount(h.childGroup) > kMaxGroupClusters || (h.childGroup >> 28) != 0)
            bad(Errc::ClusterInvalid, Fmt("childGroup 0x%08X is invalid / disagrees with childPage %u", h.childGroup, h.childPage));
    }
    if (root && !Sphere4BitsEqual(h.parentLodSphere, h.lodSphere)) bad(Errc::LodInvariant, "root cluster must have parentLodSphere == lodSphere");
    if (!root && Finite4(h.parentLodSphere) && Finite4(h.lodSphere) && !SphereContains(h.parentLodSphere, h.lodSphere, 4.0 * hd.posStep))
        bad(Errc::LodInvariant, "parentLodSphere does not contain lodSphere");
    if (x.opt.strictReserved && (h.reserved[0] || h.reserved[1])) bad(Errc::ReservedNotZero, "cluster reserved words are not zero");

    // ブロック配置（正準: ペイロードは隙間なく連続、16 整列）
    if (layoutOk)
    {
        const u32 bpp = bits[0] + bits[1] + bits[2];
        const u32 vbytes = VertexBlockBytes(vc, bpp), tbytes = TriBlockBytes(tc);
        if (h.vertexOffset != payloadCursorIn) { bad(Errc::ClusterInvalid, Fmt("vertexOffset %u != expected %u (payload must be packed contiguously)", h.vertexOffset, payloadCursorIn)); layoutOk = false; }
        else if (h.triangleOffset != h.vertexOffset + vbytes) { bad(Errc::ClusterInvalid, Fmt("triangleOffset %u != vertexOffset + vertexBlockSize (%u)", h.triangleOffset, h.vertexOffset + vbytes)); layoutOk = false; }
        else if (static_cast<u64>(h.triangleOffset) + tbytes > kPageSize) { bad(Errc::ClusterInvalid, "cluster blocks extend beyond the page"); layoutOk = false; }
        if (layoutOk) cursorOut = h.triangleOffset + tbytes;
    }
    if (!layoutOk) cursorOut = kPageSize + 1;   // 呼び出し側でページ末尾検査を飛ばす合図

    // 深い検査: 頂点のデコード・三角形の範囲・包含
    if (x.opt.deep && layoutOk)
    {
        const u8* vb = page + h.vertexOffset;
        u64 bit = 0;
        const f64 tol = 2.0 * hd.posStep;
        f32 cs[4] = {h.cullSphere[0], h.cullSphere[1], h.cullSphere[2], h.cullSphere[3]};
        bool reported = false;
        for (u32 v = 0; v < vc && !reported; ++v)
        {
            f32 pos[3];
            for (u32 a = 0; a < 3; ++a)
            {
                const u32 q = static_cast<u32>(h.posMin[a]) + GetBits(vb, bit, bits[a]);
                bit += bits[a];
                if (q > kGridMax) { bad(Errc::ClusterInvalid, Fmt("vertex %u axis %u decodes outside the 24-bit grid", v, a)); reported = true; break; }
                pos[a] = DequantizeCoord(q, hd.posOrigin[a], hd.posStep);
                if (static_cast<f64>(pos[a]) < static_cast<f64>(hd.aabbMin[a]) - tol || static_cast<f64>(pos[a]) > static_cast<f64>(hd.aabbMax[a]) + tol)
                { bad(Errc::ClusterInvalid, Fmt("vertex %u lies outside the asset AABB", v)); reported = true; break; }
            }
            if (reported) break;
            const f64 dx = pos[0] - static_cast<f64>(cs[0]), dy = pos[1] - static_cast<f64>(cs[1]), dz = pos[2] - static_cast<f64>(cs[2]);
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > static_cast<f64>(cs[3]) * (1.0 + 1e-4) + tol)
            { bad(Errc::ClusterInvalid, Fmt("vertex %u lies outside the cluster cullSphere", v)); reported = true; }
        }
        const u8* tb = page + h.triangleOffset;
        for (u32 t = 0; t < tc * 3 && !reported; ++t)
            if (tb[t] >= vc) { bad(Errc::ClusterInvalid, Fmt("triangle index %u >= vertexCount %u", tb[t], vc)); reported = true; }
    }
}

inline void CheckOnePage(Ctx& x, const VgeoMeta& m, u32 p, const u8* page, ClusterHeader* outHeaders)
{
    const PageTableEntry& te = m.pageTable[p];
    const PageHeader ph = ReadPageHeader(page);
    auto bad = [&](Errc c, const std::string& msg) { x.Add(c, Fmt("page %u: ", p) + msg, p); };
    if (ph.magic != kPageMagic) { bad(Errc::PageInvalid, "bad page magic"); return; }
    if (ph.pageIndex != p) { bad(Errc::PageInvalid, Fmt("pageIndex %u != %u", ph.pageIndex, p)); return; }
    if (ph.clusterCount != te.clusterCount || ph.groupCount != te.groupCount || ph.levelMin != LevelMinOf(te.levelFlags) ||
        ph.levelMax != LevelMaxOf(te.levelFlags) || ph.flags != PageFlagsOf(te.levelFlags))
    { bad(Errc::PageInvalid, "page header disagrees with the page table entry (clusterCount/groupCount/levels/flags)"); return; }
    if (ph.clusterTableOffset != kPageHeaderSize || ph.payloadOffset != kPageHeaderSize + kClusterHeaderSize * ph.clusterCount)
    { bad(Errc::PageInvalid, "clusterTableOffset/payloadOffset are not canonical"); return; }
    if (ph.usedBytes < ph.payloadOffset || ph.usedBytes > kPageSize) { bad(Errc::PageInvalid, Fmt("usedBytes %u is out of range", ph.usedBytes)); return; }
    if (x.opt.strictReserved && !IsZeroBytes(ph.reserved, sizeof ph.reserved)) bad(Errc::ReservedNotZero, "page header reserved bytes are not zero");

    u32 cursor = ph.payloadOffset;
    u32 lvMin = 255, lvMax = 0;
    bool layoutBroken = false;
    for (u32 i = 0; i < ph.clusterCount; ++i)
    {
        u32 next = cursor;
        ClusterHeader h;
        CheckCluster(x, m, p, i, page, cursor, next, h);
        outHeaders[i] = h;
        if (next > kPageSize) layoutBroken = true;
        else cursor = next;
        lvMin = std::min(lvMin, ClusterLevel(h)); lvMax = std::max(lvMax, ClusterLevel(h));
    }
    if (!layoutBroken)
    {
        if (cursor != ph.usedBytes) bad(Errc::PageInvalid, Fmt("usedBytes %u != end of the last cluster block %u", ph.usedBytes, cursor));
        if (!IsZeroBytes(page + ph.usedBytes, kPageSize - ph.usedBytes)) bad(Errc::PageInvalid, "bytes after usedBytes are not zero");
    }
    if (ph.clusterCount && (lvMin != ph.levelMin || lvMax != ph.levelMax)) bad(Errc::PageInvalid, "page levelMin/levelMax disagree with its clusters");
}

// DAG / BVH / 依存表の整合（全クラスタヘッダが揃ってから）。
inline void CheckDagAndBvh(Ctx& x, const VgeoMeta& m, const std::vector<GroupInfo>& groups, const std::vector<u32>& base, const std::vector<ClusterHeader>& all)
{
    const VgeoHeader& hd = m.header;
    const u32 G = static_cast<u32>(groups.size());
    auto cl = [&](u32 page, u32 idx) -> const ClusterHeader& { return all[base[page] + idx]; };
    const f64 absTol = 4.0 * hd.posStep;

    // ヘッダ集計との一致
    u32 roots = 0, maxLevel = 0;
    for (u32 p = 0; p < hd.pageCount; ++p)
        for (u32 i = 0; i < m.pageTable[p].clusterCount; ++i)
        {
            const ClusterHeader& h = cl(p, i);
            maxLevel = std::max(maxLevel, ClusterLevel(h));
            if (h.flags & kClusterFlagRoot)
            {
                ++roots;
                if (p >= hd.pinnedPageCount) x.Add(Errc::DepsInvalid, Fmt("root cluster in page %u, which is not pinned (pinnedPageCount %u)", p, hd.pinnedPageCount), p);
            }
        }
    if (roots != hd.rootClusterCount) x.Add(Errc::CountMismatch, Fmt("header rootClusterCount %u != %u root clusters found", hd.rootClusterCount, roots));
    if (maxLevel + 1 != hd.levelCount) x.Add(Errc::CountMismatch, Fmt("header levelCount %u != max level + 1 (%u)", hd.levelCount, maxLevel + 1));

    // グループ内の一致と親子リンク
    std::unordered_map<u32, u32> keyToGroup;
    keyToGroup.reserve(G * 2);
    for (u32 g = 0; g < G; ++g) keyToGroup.emplace((groups[g].page << 8) | groups[g].first, g);
    std::vector<u32> refCount(G, 0);
    for (u32 g = 0; g < G; ++g)
    {
        const GroupInfo& gi = groups[g];
        const ClusterHeader& a = cl(gi.page, gi.first);
        for (u32 k = 1; k < gi.count; ++k)
        {
            const ClusterHeader& b = cl(gi.page, gi.first + k);
            // 消費されるグループのメンバーは (parentLodSphere, parentLodError) を共有する。ルートグループ（parentLodError = +INF）は
            // 消費するグループが無く、各クラスタの parentLodSphere は自分の lodSphere なので、球は共有しなくてよい（誤差の +INF だけ共通）。
            const bool rootGroup = IsPosInf(a.parentLodError);
            if ((!rootGroup && !Sphere4BitsEqual(a.parentLodSphere, b.parentLodSphere)) || !BitsEqual(a.parentLodError, b.parentLodError) ||
                ClusterLevel(a) != ClusterLevel(b) || ((a.flags ^ b.flags) & kClusterFlagRoot))
            { x.Add(Errc::LodInvariant, Fmt("group %u: members do not share (parentLodSphere, parentLodError, level)", g), g); break; }
        }
    }
    for (u32 p = 0; p < hd.pageCount; ++p)
        for (u32 i = 0; i < m.pageTable[p].clusterCount; ++i)
        {
            const ClusterHeader& d = cl(p, i);
            if (d.childGroup == kNone) continue;
            const auto it = keyToGroup.find((GroupPage(d.childGroup) << 8) | GroupFirst(d.childGroup));
            if (it == keyToGroup.end() || groups[it->second].count != GroupCount(d.childGroup))
            { x.Add(Errc::LodInvariant, Fmt("page %u cluster %u: childGroup 0x%08X does not name a group", p, i, d.childGroup), p); continue; }
            const GroupInfo& gi = groups[it->second];
            ++refCount[it->second];
            for (u32 k = 0; k < gi.count; ++k)
            {
                const ClusterHeader& c = cl(gi.page, gi.first + k);
                if (ClusterLevel(c) + 1 != ClusterLevel(d)) { x.Add(Errc::LodInvariant, Fmt("page %u cluster %u: level is not (child level + 1)", p, i), p); break; }
                if (!BitsEqual(c.parentLodError, d.lodError) || !Sphere4BitsEqual(c.parentLodSphere, d.lodSphere))
                { x.Add(Errc::LodInvariant, Fmt("page %u cluster %u: (lodSphere, lodError) differs from its children's (parentLodSphere, parentLodError)", p, i), p); break; }
            }
        }
    for (u32 g = 0; g < G; ++g)
    {
        const ClusterHeader& a = cl(groups[g].page, groups[g].first);
        const bool rootGroup = IsPosInf(a.parentLodError);
        if (rootGroup && refCount[g] != 0) x.Add(Errc::LodInvariant, Fmt("group %u is a root group (parentLodError = +INF) but %u clusters claim it as their child group", g, refCount[g]), g);
        if (!rootGroup && refCount[g] == 0) x.Add(Errc::LodInvariant, Fmt("group %u has a finite parentLodError but no parent cluster was generated from it", g), g);
    }

    // 依存表 / priority を再導出して一致を要求
    DerivedDeps dd; VgeoError derr;
    if (!DeriveDepsFromHeaders(hd.pageCount, base, all, dd, &derr)) x.Add(derr.code, derr.message, derr.index);
    else
    {
        if (dd.offsets != m.pageDepOffsets || dd.deps != m.pageDeps) x.Add(Errc::DepsInvalid, "PAGE_DEPS differs from the dependencies implied by the clusters' childPage");
        for (u32 p = 0; p < hd.pageCount; ++p)
            if (m.pageTable[p].priority != dd.priority[p]) { x.Add(Errc::DepsInvalid, Fmt("page %u priority %u != derived %u", p, m.pageTable[p].priority, dd.priority[p]), p); break; }
    }

    // BVH の集約値（葉 = メンバークラスタ、内部 = 子ノードの子）
    auto checkAgg = [&](u32 n, u32 c, const HierChild& ch)
    {
        auto bad = [&](const char* msg) { x.Add(Errc::LodInvariant, Fmt("node %u child %u: %s", n, c, msg), n); };
        f32 minOwn = std::numeric_limits<f32>::infinity(); f32 maxPar = 0.0f; bool any = false;
        auto absorb = [&](const f32* cull, const f32* lod, f32 mo, f32 mp)
        {
            if (!SphereContains(ch.cullSphere, cull, absTol)) bad("cullSphere does not contain a member");
            if (!SphereContains(ch.lodSphere, lod, absTol)) bad("lodSphere does not contain a member's (parent) lod sphere");
            minOwn = any ? std::min(minOwn, mo) : mo; maxPar = any ? std::max(maxPar, mp) : mp; any = true;
        };
        if (NodeRefIsLeaf(ch.ref))
        {
            const GroupInfo& gi = groups[ch.ref & 0x7FFFFFFFu];
            for (u32 k = 0; k < gi.count; ++k)
            {
                const ClusterHeader& mcl = cl(gi.page, gi.first + k);
                absorb(mcl.cullSphere, mcl.parentLodSphere, mcl.lodError, mcl.parentLodError);
            }
        }
        else
        {
            for (const HierChild& cc : m.nodes[ch.ref].child)
                if (cc.ref != kNone) absorb(cc.cullSphere, cc.lodSphere, cc.minOwnError, cc.maxParentError);
        }
        if (any && (!BitsEqual(ch.minOwnError, minOwn) || !BitsEqual(ch.maxParentError, maxPar))) bad("minOwnError / maxParentError are not the exact min / max of the members");
    };
    for (u32 n = 0; n < hd.nodeCount; ++n)
        for (u32 c = 0; c < 4; ++c)
            if (m.nodes[n].child[c].ref != kNone) checkAgg(n, c, m.nodes[n].child[c]);
    for (const HierChild& cc : m.nodes[0].child)
        if (cc.ref != kNone && !SphereContains(hd.boundingSphere, cc.cullSphere, absTol)) { x.Add(Errc::LodInvariant, "header boundingSphere does not contain the root node's children"); break; }
}

// PROXY / NONVG セクションの解析（validator と ReadProxySections が共有）。
inline VgeoError ParseProxyBlob(const std::vector<u8>& d, u32 expectedCount, const VgeoMeta& m, u32 expectedKind, bool strict, std::vector<ProxySectionData>* out)
{
    auto E = [](const std::string& msg, i64 idx = -1) { return MakeErr(Errc::ProxyInvalid, msg, idx); };
    if (out) out->clear();
    if (d.size() < kProxyHeaderSize + static_cast<u64>(kProxyEntrySize) * expectedCount) return E("section is smaller than its header + entry table");
    ProxyHeader ph;
    std::memcpy(&ph, d.data(), sizeof ph);
    if (ph.sectionCount != expectedCount) return E("ProxyHeader.sectionCount disagrees with the section entry count");
    if (strict && !IsZeroBytes(ph.reserved, sizeof ph.reserved)) return MakeErr(Errc::ReservedNotZero, "ProxyHeader reserved bytes are not zero");
    u64 cursor = kProxyHeaderSize + static_cast<u64>(kProxyEntrySize) * expectedCount;
    u64 sumV = 0, sumI = 0;
    for (u32 i = 0; i < expectedCount; ++i)
    {
        ProxySectionEntry e;
        std::memcpy(&e, d.data() + kProxyHeaderSize + static_cast<size_t>(i) * kProxyEntrySize, sizeof e);
        if (e.vertexCount < 3 || e.indexCount == 0 || e.indexCount % 3 != 0) return E(Fmt("entry %u: vertexCount %u / indexCount %u is invalid", i, e.vertexCount, e.indexCount), i);
        cursor = AlignUp(cursor, 16);
        if (e.vertexOffset != cursor) return E(Fmt("entry %u: vertexOffset %u != canonical %llu", i, e.vertexOffset, static_cast<unsigned long long>(cursor)), i);
        cursor += static_cast<u64>(e.vertexCount) * kProxyVertexSize;
        cursor = AlignUp(cursor, 16);
        if (e.indexOffset != cursor) return E(Fmt("entry %u: indexOffset %u != canonical %llu", i, e.indexOffset, static_cast<unsigned long long>(cursor)), i);
        cursor += static_cast<u64>(e.indexCount) * 4;
        if (cursor > d.size()) return E(Fmt("entry %u extends beyond the section", i), i);
        if (e.materialIndex >= m.header.materialCount) return E(Fmt("entry %u: materialIndex %u out of range", i, e.materialIndex), i);
        if (m.materials[e.materialIndex].sectionKind != expectedKind) return E(Fmt("entry %u: material %u has the wrong sectionKind for this section", i, e.materialIndex), i);
        if (!Finite(e.error) || e.error < 0.0f) return E(Fmt("entry %u: error must be finite and >= 0", i), i);
        for (int a = 0; a < 3; ++a) if (!Finite(e.aabbMin[a]) || !Finite(e.aabbMax[a]) || e.aabbMin[a] > e.aabbMax[a]) return E(Fmt("entry %u: aabb invalid", i), i);
        if (strict && !IsZeroBytes(e.reserved, sizeof e.reserved)) return MakeErr(Errc::ReservedNotZero, "ProxySectionEntry reserved words are not zero", i);
        sumV += e.vertexCount; sumI += e.indexCount;
        ProxySectionData* sd = nullptr;
        if (out) { out->emplace_back(); sd = &out->back(); sd->materialIndex = e.materialIndex; sd->flags = e.flags; sd->error = e.error; std::memcpy(sd->aabbMin, e.aabbMin, 12); std::memcpy(sd->aabbMax, e.aabbMax, 12);
                   sd->vertices.resize(e.vertexCount); sd->indices.resize(e.indexCount);
                   std::memcpy(sd->vertices.data(), d.data() + e.vertexOffset, static_cast<size_t>(e.vertexCount) * kProxyVertexSize);
                   std::memcpy(sd->indices.data(), d.data() + e.indexOffset, static_cast<size_t>(e.indexCount) * 4); }
        // 深い検査: 位置が有限・インデックスが範囲内
        for (u32 v = 0; v < e.vertexCount; ++v)
        {
            f32 pos[3];
            std::memcpy(pos, d.data() + e.vertexOffset + static_cast<size_t>(v) * kProxyVertexSize, 12);
            if (!Finite(pos[0]) || !Finite(pos[1]) || !Finite(pos[2])) return E(Fmt("entry %u vertex %u has a non-finite position", i, v), i);
        }
        for (u32 k = 0; k < e.indexCount; ++k)
        {
            u32 idx;
            std::memcpy(&idx, d.data() + e.indexOffset + static_cast<size_t>(k) * 4, 4);
            if (idx >= e.vertexCount) return E(Fmt("entry %u index %u >= vertexCount", i, idx), i);
        }
    }
    cursor = AlignUp(cursor, 16);
    if (cursor != d.size()) return E("section size is not the canonical packed size");
    if (ph.totalVertices != sumV || ph.totalIndices != sumI) return E("ProxyHeader totals disagree with the entries");
    return VgeoError{};
}
} // namespace detail

namespace detail
{
// PROXY / NONVG セクションの生バイトを読む（CRC 検証つき）。
inline VgeoError ReadProxyBlob(const ByteSource& src, const VgeoMeta& m, u32 which, const ReadOptions& opt, std::vector<u8>& d)
{
    d.clear();
    const SectionEntry& s = m.header.sections[which];
    if (s.size == 0) return VgeoError{};
    d.resize(static_cast<size_t>(s.size));
    if (!src.Read(s.offset, d.data(), s.size)) return MakeErr(Errc::IoError, "failed to read the proxy section", which, s.offset);
    if (opt.verifyCrc && s.crc32 != 0 && Crc32(d.data(), d.size()) != s.crc32) return MakeErr(Errc::SectionCrcMismatch, Fmt("section %u CRC32 mismatch", which), which, s.offset);
    return VgeoError{};
}
} // namespace detail

// PROXY(kSecProxy) / NONVG(kSecNonVg) セクションを構造体へ展開する（エンジンの Mesh::Initialize 用）。
inline VgeoError ReadProxySections(const ByteSource& src, const VgeoMeta& m, u32 which, std::vector<ProxySectionData>& out, const ReadOptions& opt = {})
{
    out.clear();
    if (which != kSecProxy && which != kSecNonVg) return detail::MakeErr(Errc::ProxyInvalid, "which must be kSecProxy or kSecNonVg");
    if (m.header.sections[which].size == 0) return VgeoError{};
    std::vector<u8> d;
    const VgeoError e = detail::ReadProxyBlob(src, m, which, opt, d);
    if (!e.ok()) return e;
    return detail::ParseProxyBlob(d, m.header.sections[which].count, m, which == kSecProxy ? 0u : 1u, opt.strictReserved, &out);
}

// ファイル全体の検証。ヘッダ・目次・BVH 構造 → 全ページ(CRC・レイアウト・デコード)→ DAG 不変条件・BVH 集約値・依存表 → プロキシ の順。
// 手前の段階で問題があれば後段は飛ばす（連鎖エラーを避ける）。
inline ValidationReport ValidateVgeo(const ByteSource& src, const ReadOptions& opt = {})
{
    detail::Ctx x(src, opt, opt.maxIssues);
    try
    {
        VgeoMeta m;
        std::vector<detail::GroupInfo> groups;
        detail::LoadMetaImpl(x, m, &groups);
        if (!x.Clean()) return x.rep;

        const VgeoHeader& hd = m.header;
        std::vector<u32> base(hd.pageCount + 1, 0);
        for (u32 p = 0; p < hd.pageCount; ++p) base[p + 1] = base[p] + m.pageTable[p].clusterCount;
        std::vector<ClusterHeader> all(base[hd.pageCount]);
        std::vector<u8> buf(kPageSize);
        for (u32 p = 0; p < hd.pageCount; ++p)
        {
            const VgeoError e = ReadPage(src, m, p, buf.data(), opt);
            if (!e.ok()) { x.Add(e.code, e.message, p, e.offset); continue; }
            detail::CheckOnePage(x, m, p, buf.data(), all.data() + base[p]);
        }
        if (!x.Clean()) return x.rep;

        if (hd.clusterCount > 0) detail::CheckDagAndBvh(x, m, groups, base, all);
        if (!x.Clean()) return x.rep;

        for (u32 which : {static_cast<u32>(kSecProxy), static_cast<u32>(kSecNonVg)})
        {
            const SectionEntry& s = hd.sections[which];
            if (s.size == 0) continue;
            std::vector<u8> d;
            VgeoError e = detail::ReadProxyBlob(src, m, which, opt, d);
            if (e.ok()) e = detail::ParseProxyBlob(d, s.count, m, which == kSecProxy ? 0u : 1u, opt.strictReserved, nullptr);
            if (!e.ok()) { x.Add(e.code, e.message, which, s.offset); continue; }
            if (which == kSecProxy)
            {
                f32 mx = 0;
                for (u32 i = 0; i < s.count; ++i)
                {
                    ProxySectionEntry pe;
                    std::memcpy(&pe, d.data() + kProxyHeaderSize + static_cast<size_t>(i) * kProxyEntrySize, sizeof pe);
                    mx = std::max(mx, pe.error);
                }
                if (!detail::BitsEqual(mx, hd.proxyError)) x.Add(Errc::CountMismatch, "header proxyError != max(PROXY entry error)");
            }
        }
    }
    catch (const detail::Stop&) {}
    return x.rep;
}

// 全部をメモリへ読む（テスト / cooker の再読み込み / 小さいファイル向け。ページは連続バッファ）。
inline VgeoError LoadContent(const ByteSource& src, VgeoContent& out, const ReadOptions& opt = {})
{
    VgeoMeta m;
    VgeoError e = LoadMeta(src, m, opt);
    if (!e.ok()) return e;
    out = VgeoContent{};
    out.header = m.header;
    out.materials = m.materials; out.strings = m.strings; out.nodes = m.nodes; out.debugJson = m.debugJson;
    out.pages.resize(static_cast<size_t>(m.header.pageCount) * kPageSize);
    for (u32 p = 0; p < m.header.pageCount; ++p)
    {
        e = ReadPage(src, m, p, out.pages.data() + static_cast<size_t>(p) * kPageSize, opt);
        if (!e.ok()) return e;
    }
    e = ReadProxySections(src, m, kSecProxy, out.proxy, opt);
    if (!e.ok()) return e;
    return ReadProxySections(src, m, kSecNonVg, out.nonvg, opt);
}

// content を（メモリ上で）書いて検証する。cooker / テストの「出力が仕様どおりか」検査用（strictReserved を既定で ON）。
inline ValidationReport ValidateContent(const VgeoContent& c, ReadOptions opt = {})
{
    opt.strictReserved = true;
    std::vector<u8> bytes;
    const VgeoError e = WriteVgeoToMemory(c, bytes);
    if (!e.ok()) { ValidationReport r; r.issues.push_back(e); return r; }
    return ValidateVgeo(MemorySource(bytes), opt);
}

} // namespace dx12e::vg
