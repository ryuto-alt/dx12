#pragma once

#include <vector>
#include <memory>
#include <filesystem>

#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class GraphicsDevice;
class ResourceManager;

struct ModelData
{
    std::vector<std::unique_ptr<Mesh>>     meshes;
    std::vector<std::unique_ptr<Material>> materials;
    std::unique_ptr<Skeleton>              skeleton;   // null = static mesh
    std::vector<std::unique_ptr<AnimationClip>> animClips;  // empty = no animation

    // Node animation（skeleton が null でアニメーションがある場合）
    std::unique_ptr<NodeGraph>                         nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>>    nodeAnimClips;

    // .vgeo のプロキシだけ: meshes[i] が .vgeo のどの材質（MaterialRecord の番号）か。VG の材質（.dxmat / 上書き）を
    // サブメッシュ番号から引くのに使う。通常のモデルは空。
    std::vector<uint32_t> vgMaterialOfMesh;
};

// MCP asset_info 用: GPU を使わずモデルのメタ情報だけ読む(Assimp のみ)。
struct ModelProbeInfo
{
    bool        ok = false;
    std::string error;
    uint32_t meshCount = 0, materialCount = 0, boneCount = 0;
    uint64_t totalVertices = 0, totalFaces = 0;
    bool     hasSkeleton = false;
    struct Anim { std::string name; double durationSec = 0.0; };
    std::vector<Anim> animations;
    // ノード変換込みのワールド空間 AABB(= スケール1で置いた時の実サイズ)
    float aabbMin[3] = {0, 0, 0}, aabbMax[3] = {0, 0, 0};
};

// glTF の occlusionTexture の読み取り結果（マテリアル 1 つ分。GPU 不要。テスト・診断用）。
struct OcclusionProbe
{
    std::string material;
    bool  hasOcclusion      = false;   // 使える occlusionTexture がある（glTF/GLB・UV0）
    bool  hasMetalRoughness = false;
    bool  sharedWithMR      = false;   // metallicRoughness と同じ画像（ORM 1 枚）。false なら読み込み時に ORM を作る
    float strength          = 1.0f;
};

// モデルごとの取り込みオプション。モデルファイルの隣の `<モデルファイル名>.import.json`
// （例 BistroExterior.fbx.import.json）から読む。キーを足すときはここへメンバーと Parse の行を足す。
struct ModelImportOptions
{
    // FBX の SpecularColor スロットを metallicRoughness（G=roughness / B=metallic、R は未使用で AO にしない）として読む（Falcor 規約）。
    // 一般の FBX では Specular は本物の鏡面反射色なので既定は false。
    bool specularIsORM = false;

    // 静的モデル（スキン無し・アニメ無し）の Mesh を「同じ aiMaterial × XZ 空間セル」ごとに 1 個へ結合する
    // （都市 FBX のようにメッシュが数万ある時のドローコール削減）。既定 false＝従来と同じ。
    bool mergeStatic = false;
    // 結合セルの一辺（モデル空間の単位）。0 以下 / 省略は自動 = モデル AABB の XZ 最大辺 / 8。
    float mergeCellSize = 0.0f;

    // 右手系の座標（Blender / FBX / glTF）を左手系のエンジンへ正しく変換する（aiProcess_MakeLeftHanded + FlipWindingOrder）。
    // ★既定 false = 従来どおり「右手系の座標をそのまま左手系として読む」＝モデルが鏡写しになる（文字・時計回りが逆、
    //   Blender の +Y が奥(-Z)へ行く）。既存シーンはこの向きで置かれているので全体の既定は変えず、モデルごとに選ぶ。
    //   true のとき Blender の (X, Y, Z) は (X, Z, Y) に来る（UE と同じく見た目が保たれる）。2026-11-06、UE 版の FBX の
    //   ラベルが左右反転して見えた件で追加。
    bool leftHanded = false;

    // 仮想ジオメトリ（VG）の自動 cook（resource/VgAutoCook）。"auto"（既定）= 静的・不透明・三角形数がしきい値以上なら裏で cook して
    // VG で描く / "on" = しきい値・材質数に関係なく cook する（アルファ材質は NONVG セクションへ）/ "off" = 常に通常メッシュ。
    // ★鍵（キャッシュ）には import.json の中身ごと入るので、ここを変えると作り直される。
    enum class VgMode : uint8_t { Auto, On, Off };
    VgMode virtualGeometry = VgMode::Auto;

    // JSON 文字列から読む。壊れている / キーが無い / 型違いは既定値（全部 false / 0）。
    static ModelImportOptions Parse(const std::string& jsonText);
    // モデルの隣の import.json を vfs 経由で読む（配布ゲームの pak でも読める）。無ければ既定値。
    static ModelImportOptions LoadFor(const std::filesystem::path& modelPath);
};

// ── 仮想ジオメトリの自動 cook 用: GPU を使わずに「エンジンが描くのと同じ頂点」と材質の出どころを取り出す ──
//   LoadFromFile と同じ import 設定（import.json の leftHanded・単位正規化・FlipUVs）・同じノード変換の焼き込み・
//   同じ UV 変換・同じ三角形の拾い方。材質ごとに 1 セクションへまとめる（mergeStatic の結合は描画単位の都合なので頂点は同じ）。
struct StaticTexSource
{
    std::filesystem::path file;          // 実ファイル（空なら bytes）
    std::vector<uint8_t>  bytes;         // 埋め込み / 合成（ORM）の画像バイト列
    std::string           ext;           // 書き出すときの拡張子（"png" / "jpg" …。file のときは元の拡張子）
    bool Empty() const { return file.empty() && bytes.empty(); }
};

struct StaticMaterialCpu
{
    std::string     name;
    StaticTexSource albedo, normal, metalRough, emissive;
    bool  occlusionInMR = false;         // metalRough の R を AO として読む（Material::occlusionInMR と同じ）
    float aoStrength    = 1.0f;
    float metallic      = 0.0f, roughness = 0.5f;
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff   = 0.5f;
    float baseColorAlpha = 1.0f;
    bool  doubleSided   = false;         // AI_MATKEY_TWOSIDED（glTF doubleSided）
};

struct StaticModelCpu
{
    std::vector<float>    positions;     // xyz（エンジン空間・ノード変換を焼いた後）
    std::vector<float>    normals;       // xyz（正規化済み）
    std::vector<float>    uvs;           // uv（UV 変換を焼いた後・D3D の左上原点）
    std::vector<uint32_t> indices;       // 三角形リスト（sections が先頭から隙間なく覆う）
    struct Section { uint32_t material = 0, firstIndex = 0, indexCount = 0; };
    std::vector<Section>  sections;      // 材質ごとに 1 個
    std::vector<StaticMaterialCpu> materials;   // sections[i].material の指す先（使われている aiMaterial だけ）
    uint64_t triangles = 0;
    bool hasSkin = false, hasAnimation = false;
    bool hasVertexColor = false;         // 白以外の頂点カラーがある（VG は頂点カラーを持たない）
    uint32_t mirroredNodes = 0;          // 行列式が負のノード（巻き順を反転して外向きに揃えた）
};

class ModelLoader
{
public:
    // VG の自動 cook 用（裏のスレッドから呼んでよい。GPU・ResourceManager に触らない）。失敗は false と err。
    // countOnly = true なら頂点を作らず triangles / hasSkin / hasAnimation / 材質だけ埋める（しきい値の判定用）。
    static bool ExtractStaticForVg(const std::filesystem::path& filePath, StaticModelCpu& out, std::string* err,
                                   bool countOnly = false);
    // LoadFromFile が Assimp に渡す後処理フラグ（import 設定込み）。解析キャッシュの鍵にもなる。
    static unsigned int LoadFlags(const ModelImportOptions& opts);

    // glTF の occlusionTexture をマテリアルごとに調べる（LoadFromFile と同じ判定）。読めなければ空。
    static std::vector<OcclusionProbe> ProbeOcclusion(const std::filesystem::path& filePath);

    static ModelProbeInfo Probe(const std::filesystem::path& filePath);

    static ModelData LoadFromFile(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const std::filesystem::path& filePath,
        ResourceManager& resourceManager);

    static std::vector<std::unique_ptr<AnimationClip>> LoadAnimationsFromFile(
        const std::filesystem::path& filePath,
        const Skeleton& skeleton);
};

} // namespace dx12e
