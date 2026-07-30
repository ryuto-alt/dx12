#include "resource/ModelLoader.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Texture.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ResourceManager.h"
#include "resource/VfsIOSystem.h"
#include "core/vfs/Vfs.h"

#include <cstdlib>
#include <cmath>    // 祖先変換が単位行列かの判定
#include <cctype>   // ::tolower(拡張子の小文字化)
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <algorithm>
#include <functional>      // Probe のノード再帰(ワールド AABB)用
#include <unordered_set>   // Probe のボーン名ユニーク数え上げ用

#include <Windows.h>

using namespace DirectX;

namespace dx12e
{

namespace
{

// FBX の単位系をメートルへ正規化する共通フラグ。
//   FBX は GlobalSettings::UnitScaleFactor で「1単位 = 何 cm か」を宣言する。Blender/Maya/3ds Max
//   の既定書き出しは cm 基準なので、1m の立方体がノード変換 100 倍(= 100 単位)で入ってくる。
//   assimp は既定ではこの単位を解釈しないため、そのまま読むと **100 倍の大きさ** になる。
//   aiProcess_GlobalScale が UnitScaleFactor*0.01 を頂点・ボーンオフセット・ノード平行移動・
//   アニメーションの位置キーへ一括で適用してメートルに揃える。
//   assimp で SetFileScale() を呼ぶインポータは FBX **だけ** なので(v5.4.3 で確認)、
//   glTF/glb・OBJ・STL・PLY・DAE 等には完全な no-op。Collada の <unit meter> は
//   ColladaLoader がルートノード変換へ焼き込み済みなので二重適用にもならない。
//   ★ロード系(LoadFromFile/LoadAnimationsFromFile)と Probe で必ず同じ値を使うこと。
//     片方だけ外すと asset_info の申告サイズと実際の描画サイズがずれる。
constexpr unsigned int kUnitScaleFlag = aiProcess_GlobalScale;

// ★ただしスキンメッシュには掛けてはいけない。
//   assimp の FBX インポータが返すスキン情報は boneGlobal * offsetMatrix のスケールが常に 1
//   になる(= スキンメッシュはメッシュローカル座標のまま描かれ、ノード変換の 100 倍は
//   ボーン側で打ち消される)。そこへ kUnitScaleFlag が頂点だけ k 倍すると、打ち消しが
//   崩れて描画結果がそのまま k 倍ずれる。実測: Blender 既定書き出しの 1m スキンキューブが 1cm。
//   静的メッシュはノード変換を頂点へ焼き込むので、逆に掛けないと 100m になる。
//   → ボーンの有無で出し分ける。判定に使う「k」がこれ。
//   FBX 以外は UnitScaleFactor を持たないので常に 1.0 = 再インポートは起きない。
float UnitScaleFactorOf(const aiScene* scene)
{
    if (!scene || !scene->mMetaData) return 1.0f;
    // ★型は assimp のバージョンで float だったり double だったりする
    //   (v5.4.3 の FBXConverter は GlobalSettings の float をそのまま Set する)。
    //   aiMetadata::Get は型が合わないと黙って false を返すので両方試すこと。
    double unitScale = 0.0;
    float  unitScaleF = 0.0f;
    if (scene->mMetaData->Get("UnitScaleFactor", unitScale)) { /* ok */ }
    else if (scene->mMetaData->Get("UnitScaleFactor", unitScaleF)) unitScale = unitScaleF;
    else return 1.0f;
    if (unitScale == 0.0) return 1.0f;
    return static_cast<float>(unitScale * 0.01);   // assimp の fileScale と同じ式
}

bool SceneHasBones(const aiScene* scene)
{
    for (unsigned i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i]->mNumBones > 0) return true;
    return false;
}

std::wstring ToWideString(const char* str)
{
    if (!str || str[0] == '\0')
    {
        return {};
    }

    const int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (len <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, result.data(), len);
    result.pop_back();
    return result;
}

std::filesystem::path ResolveTexturePath(
    const char* rawPath,
    const std::filesystem::path& modelDir)
{
    std::wstring wide = ToWideString(rawPath);
    std::filesystem::path p(wide);

    // ディスクだけでなく VFS(game.pak) も見る。ゲームモードではルーズファイルが
    // 存在しないため、fs::exists だけだと .mtl の map_Kd 等が全て解決失敗して白くなる。
    auto found = [](const std::filesystem::path& candidate) {
        if (vfs::ExistsAbs(candidate.wstring()))
            return true;
        std::error_code ec;
        return std::filesystem::exists(candidate, ec);
    };

    // 1. 絶対パスがそのまま存在する
    if (p.is_absolute() && found(p))
        return p;

    // 2. モデルと同じディレクトリにファイル名だけで探す
    auto byFilename = modelDir / p.filename();
    if (found(byFilename))
        return byFilename;

    // 3. モデルディレクトリからの相対パス
    auto relative = modelDir / p;
    if (found(relative))
        return relative;

    // 4. textures/ サブフォルダ
    auto inTextures = modelDir / "textures" / p.filename();
    if (found(inTextures))
        return inTextures;

    return {}; // 見つからない
}

DirectX::XMFLOAT4X4 ToXMFLOAT4X4(const aiMatrix4x4& m)
{
    // Assimp: translation in a4,b4,c4 (last column)
    // DirectXMath: translation in _41,_42,_43 (last row)
    // → transpose needed
    return DirectX::XMFLOAT4X4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4
    );
}

void CollectBoneNames(const aiScene* scene, std::unordered_map<std::string, const aiBone*>& boneMap)
{
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi)
        {
            const aiBone* bone = mesh->mBones[bi];
            std::string name(bone->mName.C_Str());
            if (boneMap.find(name) == boneMap.end())
            {
                boneMap[name] = bone;
            }
        }
    }
}

// skippedAncestors: 直近のボーン祖先より下にある「ボーンではないノード」のローカル変換の積
//   （行ベクトル規約 = 子 * 親 の順）。glTF の "Z_UP" / "Armature"、Collada/FBX の軸変換
//   ノードはスケルトンに現れないので、ここで拾って直下のボーンへ畳み込む。
//   これを捨てると Z-up でオーサリングされた glTF（CesiumMan.glb）が寝たまま入る。
//   ★glTF は仕様上 Y-up 固定で、assimp の glTF2 インポータは軸変換を一切足さない
//     （glTF2Importer::ImportNodes を確認済み。aiProcess_ にも該当フラグは無い）。
//     「Z-up の glTF」に見えるものは、ファイル側が変換ノードを持っているだけ。
void BuildSkeletonRecursive(
    const aiNode* node,
    i32 parentIndex,
    DirectX::FXMMATRIX skippedAncestors,
    const std::unordered_map<std::string, const aiBone*>& boneMap,
    Skeleton& skeleton)
{
    std::string nodeName(node->mName.C_Str());
    auto it = boneMap.find(nodeName);

    i32 currentIndex = parentIndex;

    const DirectX::XMFLOAT4X4 localF = ToXMFLOAT4X4(node->mTransformation);
    DirectX::XMMATRIX childSkipped =
        DirectX::XMMatrixMultiply(DirectX::XMLoadFloat4x4(&localF), skippedAncestors);

    if (it != boneMap.end())
    {
        BoneNode boneNode;
        boneNode.name = nodeName;
        boneNode.parentIndex = parentIndex;
        boneNode.inverseBindPose = ToXMFLOAT4X4(it->second->mOffsetMatrix);
        boneNode.localBindPose = localF;

        // 祖先の変換が単位行列でなければ preTransform として持たせる。
        // （単位行列なら hasPreTransform=false のまま = 従来と完全に同じ計算）
        DirectX::XMFLOAT4X4 pre;
        DirectX::XMStoreFloat4x4(&pre, skippedAncestors);
        constexpr float kEps = 1e-6f;
        bool isIdentity = true;
        for (int r = 0; r < 4 && isIdentity; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::fabs(pre.m[r][c] - ((r == c) ? 1.0f : 0.0f)) > kEps) { isIdentity = false; break; }
        if (!isIdentity)
        {
            boneNode.preTransform    = pre;
            boneNode.hasPreTransform = true;
        }

        currentIndex = static_cast<i32>(skeleton.GetBoneCount());
        skeleton.AddBone(std::move(boneNode));
        childSkipped = DirectX::XMMatrixIdentity();
    }

    for (unsigned int ci = 0; ci < node->mNumChildren; ++ci)
    {
        BuildSkeletonRecursive(node->mChildren[ci], currentIndex, childSkipped, boneMap, skeleton);
    }
}

std::unique_ptr<Skeleton> BuildSkeleton(const aiScene* scene)
{
    std::unordered_map<std::string, const aiBone*> boneMap;
    CollectBoneNames(scene, boneMap);

    if (boneMap.empty())
    {
        return nullptr;
    }

    auto skeleton = std::make_unique<Skeleton>();
    BuildSkeletonRecursive(scene->mRootNode, -1, DirectX::XMMatrixIdentity(), boneMap, *skeleton);

    Logger::Info("Skeleton built: {} bones", skeleton->GetBoneCount());

    // FK（ComputeGlobalMatrices）の単一ループは「親が子より前に並んでいる」ことを前提にしている。
    // assimp のノード階層を前順走査で積んでいるので通常は必ず成立するが、
    // 破れたら全ボーンが静かに壊れる（親のグローバル行列が未計算のまま読まれる）ので検証する。
    if (!skeleton->AreBonesCorrectlyOrdered())
    {
        Logger::Warn("Skeleton bones are NOT in parent-before-child order. "
                     "FK will read uninitialized parent matrices and the character will be deformed.");
    }
    // トラックの無いボーンは localBindPose を TRS 分解した値でポーズを埋める。
    // せん断を含む行列は分解できないので、その場合だけバインドポーズがずれる。
    if (skeleton->HasUndecomposableBind())
    {
        Logger::Warn("Skeleton has bone(s) whose localBindPose cannot be decomposed into TRS "
                     "(shear?). Bones without animation tracks may be posed incorrectly.");
    }
    if (skeleton->GetBoneCount() > Skeleton::kMaxBones)
    {
        Logger::Warn("Skeleton has {} bones but the skinning buffer holds only {}. "
                     "Bones beyond the limit will not be uploaded.",
                     skeleton->GetBoneCount(), Skeleton::kMaxBones);
    }
    return skeleton;
}

std::unique_ptr<AnimationClip> BuildAnimationClipFromAnim(
    const aiAnimation* anim, const Skeleton& skeleton)
{
    auto clip = std::make_unique<AnimationClip>();
    clip->SetDuration(static_cast<float>(anim->mDuration));
    clip->SetTicksPerSecond(anim->mTicksPerSecond > 0.0
        ? static_cast<float>(anim->mTicksPerSecond)
        : 25.0f);
    clip->SetName(anim->mName.C_Str());

    for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci)
    {
        const aiNodeAnim* channel = anim->mChannels[ci];
        std::string boneName(channel->mNodeName.C_Str());

        i32 boneIndex = skeleton.FindBoneIndex(boneName);
        if (boneIndex < 0)
        {
            continue;
        }

        BoneTrack track;
        track.boneIndex = static_cast<u32>(boneIndex);

        // Position keys
        track.positionKeys.reserve(channel->mNumPositionKeys);
        for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k)
        {
            const aiVectorKey& key = channel->mPositionKeys[k];
            track.positionKeys.push_back({
                static_cast<float>(key.mTime),
                { key.mValue.x, key.mValue.y, key.mValue.z }
            });
        }

        // Rotation keys: aiQuaternion (w,x,y,z) -> XMFLOAT4 (x,y,z,w)
        track.rotationKeys.reserve(channel->mNumRotationKeys);
        for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k)
        {
            const aiQuatKey& key = channel->mRotationKeys[k];
            track.rotationKeys.push_back({
                static_cast<float>(key.mTime),
                { key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w }
            });
        }

        // Scale keys
        track.scaleKeys.reserve(channel->mNumScalingKeys);
        for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k)
        {
            const aiVectorKey& key = channel->mScalingKeys[k];
            track.scaleKeys.push_back({
                static_cast<float>(key.mTime),
                { key.mValue.x, key.mValue.y, key.mValue.z }
            });
        }

        clip->AddTrack(std::move(track));
    }

    // ticks → 秒へ正規化（glTF=1000ticks/s(ms)、FBX=30等の単位差をロード時に吸収）
    clip->NormalizeToSeconds();

    Logger::Info("AnimationClip built: '{}' {} tracks, duration={:.2f}s",
                 clip->GetName(), clip->GetTrackCount(), clip->GetDuration());
    return clip;
}

std::vector<std::unique_ptr<AnimationClip>> BuildAllAnimationClips(
    const aiScene* scene, const Skeleton& skeleton)
{
    std::vector<std::unique_ptr<AnimationClip>> clips;

    if (!scene->mAnimations || scene->mNumAnimations == 0)
    {
        return clips;
    }

    for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai)
    {
        auto clip = BuildAnimationClipFromAnim(scene->mAnimations[ai], skeleton);
        if (clip)
        {
            clips.push_back(std::move(clip));
        }
    }

    return clips;
}

void WriteBoneWeightsToVertices(
    const aiMesh* aiMeshPtr,
    std::vector<Vertex>& vertices,
    const Skeleton& skeleton)
{
    for (unsigned int bi = 0; bi < aiMeshPtr->mNumBones; ++bi)
    {
        const aiBone* bone = aiMeshPtr->mBones[bi];
        i32 boneIndex = skeleton.FindBoneIndex(bone->mName.C_Str());
        if (boneIndex < 0)
        {
            continue;
        }

        for (unsigned int wi = 0; wi < bone->mNumWeights; ++wi)
        {
            const aiVertexWeight& vw = bone->mWeights[wi];
            Vertex& vert = vertices[vw.mVertexId];

            // Find empty slot in boneIndices/boneWeights (weight == 0)
            float* weights = &vert.boneWeights.x;
            u32*   indices = &vert.boneIndices.x;

            for (u32 slot = 0; slot < 4; ++slot)
            {
                if (weights[slot] == 0.0f)
                {
                    indices[slot] = static_cast<u32>(boneIndex);
                    weights[slot] = vw.mWeight;
                    break;
                }
            }
        }
    }

    // Normalize bone weights
    for (auto& vert : vertices)
    {
        float* weights = &vert.boneWeights.x;
        float total = weights[0] + weights[1] + weights[2] + weights[3];
        if (total > 0.0f && std::abs(total - 1.0f) > 1e-6f)
        {
            float invTotal = 1.0f / total;
            weights[0] *= invTotal;
            weights[1] *= invTotal;
            weights[2] *= invTotal;
            weights[3] *= invTotal;
        }
    }
}

// ---------------------------------------------------------------
// Node Animation: ノード階層の構築
// ---------------------------------------------------------------
void BuildNodeGraphRecursive(
    const aiNode* node,
    i32 parentIndex,
    NodeGraph& graph)
{
    SceneNode sn;
    sn.name = node->mName.C_Str();
    sn.parentIndex = parentIndex;

    sn.localDefault = ToXMFLOAT4X4(node->mTransformation);

    for (unsigned int m = 0; m < node->mNumMeshes; ++m)
    {
        sn.meshIndices.push_back(node->mMeshes[m]);
    }

    i32 myIndex = static_cast<i32>(graph.GetNodeCount());
    graph.AddNode(std::move(sn));

    for (unsigned int c = 0; c < node->mNumChildren; ++c)
    {
        BuildNodeGraphRecursive(node->mChildren[c], myIndex, graph);
    }
}

std::unique_ptr<NodeGraph> BuildNodeGraph(const aiScene* scene)
{
    if (!scene->mRootNode)
    {
        return nullptr;
    }

    auto graph = std::make_unique<NodeGraph>();
    BuildNodeGraphRecursive(scene->mRootNode, -1, *graph);

    // 初期グローバル行列の逆行列を計算（inverseBindPose相当）
    graph->ComputeInverseDefaultGlobals();

    Logger::Info("NodeGraph built: {} nodes (root='{}')",
                 graph->GetNodeCount(), scene->mRootNode->mName.C_Str());

    // デバッグ: メッシュ→ノード マッピング状況
    u32 mappedMeshCount = 0;
    for (u32 ni = 0; ni < graph->GetNodeCount(); ++ni)
    {
        const auto& node = graph->GetNode(ni);
        mappedMeshCount += static_cast<u32>(node.meshIndices.size());
        if (!node.meshIndices.empty())
        {
            Logger::Info("  Node[{}] '{}': {} meshes, parent={}",
                         ni, node.name, node.meshIndices.size(), node.parentIndex);
        }
    }
    Logger::Info("  Total mesh-node mappings: {}/{}", mappedMeshCount, scene->mNumMeshes);
    return graph;
}

std::unique_ptr<NodeAnimationClip> BuildNodeAnimClipFromAnim(
    const aiAnimation* anim, const NodeGraph& graph)
{
    auto clip = std::make_unique<NodeAnimationClip>();
    clip->SetDuration(static_cast<float>(anim->mDuration));
    clip->SetTicksPerSecond(anim->mTicksPerSecond > 0.0
        ? static_cast<float>(anim->mTicksPerSecond)
        : 25.0f);
    clip->SetName(anim->mName.C_Str());

    for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci)
    {
        const aiNodeAnim* channel = anim->mChannels[ci];
        std::string nodeName(channel->mNodeName.C_Str());

        i32 nodeIndex = graph.FindNodeIndex(nodeName);
        if (nodeIndex < 0)
        {
            Logger::Warn("  NodeAnim チャンネル '{}' に対応するノードが無いためスキップしました", nodeName);
            continue;
        }

        NodeTrack track;
        track.nodeIndex = static_cast<u32>(nodeIndex);

        // Position keys
        track.positionKeys.reserve(channel->mNumPositionKeys);
        for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k)
        {
            const aiVectorKey& key = channel->mPositionKeys[k];
            track.positionKeys.push_back({
                static_cast<float>(key.mTime),
                { key.mValue.x, key.mValue.y, key.mValue.z }
            });
        }

        // Rotation keys: aiQuaternion (w,x,y,z) -> XMFLOAT4 (x,y,z,w)
        track.rotationKeys.reserve(channel->mNumRotationKeys);
        for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k)
        {
            const aiQuatKey& key = channel->mRotationKeys[k];
            track.rotationKeys.push_back({
                static_cast<float>(key.mTime),
                { key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w }
            });
        }

        // Scale keys
        track.scaleKeys.reserve(channel->mNumScalingKeys);
        for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k)
        {
            const aiVectorKey& key = channel->mScalingKeys[k];
            track.scaleKeys.push_back({
                static_cast<float>(key.mTime),
                { key.mValue.x, key.mValue.y, key.mValue.z }
            });
        }

        clip->AddTrack(std::move(track));
    }

    // ticks → 秒へ正規化（AnimationClip と同じ規約）
    clip->NormalizeToSeconds();

    Logger::Info("NodeAnimClip built: '{}' {} tracks, duration={:.2f}s",
                 clip->GetName(), clip->GetTrackCount(), clip->GetDuration());
    return clip;
}

std::vector<std::unique_ptr<NodeAnimationClip>> BuildAllNodeAnimClips(
    const aiScene* scene, const NodeGraph& graph)
{
    std::vector<std::unique_ptr<NodeAnimationClip>> clips;

    if (!scene->mAnimations || scene->mNumAnimations == 0)
    {
        return clips;
    }

    for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai)
    {
        auto clip = BuildNodeAnimClipFromAnim(scene->mAnimations[ai], graph);
        if (clip && clip->GetTrackCount() > 0)
        {
            clips.push_back(std::move(clip));
        }
    }

    return clips;
}

} // anonymous namespace

ModelProbeInfo ModelLoader::Probe(const std::filesystem::path& filePath)
{
    ModelProbeInfo info;
    Assimp::Importer importer;
    // メタ情報だけ欲しいので後処理は最小限。三角形化しないので faces は
    // ポリゴン数(三角形換算前)になる。単位正規化だけは LoadFromFile と揃える
    // (揃えないと asset_info の申告サイズが実際の描画サイズと食い違う)。
    //
    // ★★ここは**スキンメッシュでも kUnitScaleFlag を外さない**。それで正しい。
    //   LoadFromFile はスキン時にフラグを外すので「揃っていない」ように見えるが、
    //   両者は経路が違うだけで結果は一致する:
    //     Probe        … ノード変換(cm ファイルなら 100 倍) × 単位正規化(0.01) = 1 倍
    //     LoadFromFile … ノード変換を頂点へ焼かず(ボーン側で打ち消される) + フラグも外す
    //   2026-07-30 に「揃っていない」と判断して Probe 側でもフラグを外す変更を入れたところ、
    //   `ModelScaleTests` の cube1m_skinned.fbx が 1m → **100m** になって落ちた。
    //   触る前に必ず `ctest -R ModelScaleTests` を通すこと（tests/data に FBX 一式がある）。
    const aiScene* scene = importer.ReadFile(filePath.string(), kUnitScaleFlag);
    if (!scene || !scene->mRootNode)
    {
        info.error = importer.GetErrorString();
        if (info.error.empty()) info.error = "unknown import error";
        return info;
    }
    info.meshCount     = scene->mNumMeshes;
    info.materialCount = scene->mNumMaterials;

    std::unordered_set<std::string> boneNames;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i)
    {
        const aiMesh* mesh = scene->mMeshes[i];
        info.totalVertices += mesh->mNumVertices;
        info.totalFaces    += mesh->mNumFaces;
        for (unsigned b = 0; b < mesh->mNumBones; ++b)
            boneNames.insert(mesh->mBones[b]->mName.C_Str());
    }
    info.boneCount   = static_cast<uint32_t>(boneNames.size());
    info.hasSkeleton = !boneNames.empty();

    // AABB はノード変換を掛けたワールド空間で出す。メッシュローカルのままだと
    // 「スケールがノード変換側に乗っている」モデル(FBX 全般・Blender 由来の glTF)で
    // 実際の描画サイズと桁違いの値を報告してしまい、サイズ不具合の発見が不可能になる。
    bool anyVertex = false;
    const std::function<void(const aiNode*, const aiMatrix4x4&)> walk =
        [&](const aiNode* node, const aiMatrix4x4& parent)
    {
        const aiMatrix4x4 global = parent * node->mTransformation;
        for (unsigned i = 0; i < node->mNumMeshes; ++i)
        {
            const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            for (unsigned v = 0; v < mesh->mNumVertices; ++v)
            {
                const aiVector3D p = global * mesh->mVertices[v];
                const float c[3] = { p.x, p.y, p.z };
                for (int k = 0; k < 3; ++k)
                {
                    if (!anyVertex) { info.aabbMin[k] = c[k]; info.aabbMax[k] = c[k]; }
                    else
                    {
                        info.aabbMin[k] = (std::min)(info.aabbMin[k], c[k]);
                        info.aabbMax[k] = (std::max)(info.aabbMax[k], c[k]);
                    }
                }
                anyVertex = true;
            }
        }
        for (unsigned c = 0; c < node->mNumChildren; ++c) walk(node->mChildren[c], global);
    };
    walk(scene->mRootNode, aiMatrix4x4());

    for (unsigned a = 0; a < scene->mNumAnimations; ++a)
    {
        const aiAnimation* anim = scene->mAnimations[a];
        const double tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 25.0;
        info.animations.push_back({ anim->mName.C_Str(), anim->mDuration / tps });
    }
    info.ok = true;
    return info;
}

ModelData ModelLoader::LoadFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::filesystem::path& filePath,
    ResourceManager& resourceManager)
{
    Assimp::Importer importer;
    importer.SetIOHandler(new VfsIOSystem()); // importer が所有権を持ち dtor で delete する
    // スキンメッシュ用の「単位正規化なし再読み」(下記)。Assimp::Importer は
    // 生成だけで全インポータを登録する重いオブジェクトなので、必要になってから作る。
    std::unique_ptr<Assimp::Importer> rawImporter;

    // 全フォーマットで FlipUVs を掛ける(従来挙動)。assimp の glTF インポータは UV を
    // 左下原点(OpenGL流)で返すため、FlipUVs で D3D の左上原点に揃う(実機検証済み)。
    unsigned int flags =
        aiProcess_Triangulate |
        // ★SortByPType: Triangulate だけだと線・点のプリミティブがそのまま残り、
        //   同じインデックスバッファへ 1〜2 個ずつ流し込まれてインデックス数が 3 の倍数で
        //   なくなる（meshopt_generateVertexRemap / meshopt_simplify にも壊れた入力が渡る）。
        //   三角形以外を別メッシュへ分けておけば、下の面ループで自然に弾ける。
        aiProcess_SortByPType |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_LimitBoneWeights |
        aiProcess_PopulateArmatureData |
        aiProcess_FlipUVs |
        kUnitScaleFlag;

    const aiScene* scene = importer.ReadFile(filePath.string(), flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        Logger::Error("モデルの読み込みに失敗しました: {}", filePath.string());
        return {};
    }

    // 単位正規化はスキンメッシュには掛けない(kUnitScaleFlag のコメント参照)。
    // 掛かった形跡(k != 1)があるスキンモデルだけ、フラグを外して読み直す。
    // 再パースが走るのは「ボーン有り かつ cm 基準の FBX」だけ。glTF/OBJ 等は k == 1 なので起きない。
    if (SceneHasBones(scene))
    {
        const float k = UnitScaleFactorOf(scene);
        if (std::fabs(k - 1.0f) > 1e-6f)
        {
            rawImporter = std::make_unique<Assimp::Importer>();
            rawImporter->SetIOHandler(new VfsIOSystem());
            const aiScene* raw = rawImporter->ReadFile(filePath.string(), flags & ~kUnitScaleFlag);
            if (raw && !(raw->mFlags & AI_SCENE_FLAGS_INCOMPLETE) && raw->mRootNode)
            {
                Logger::Info("スキンメッシュのため単位正規化(×{:.4f})を外して読み直しました: {}",
                             k, filePath.filename().string());
                scene = raw;
            }
            else
            {
                Logger::Warn("単位正規化なしの再読み込みに失敗したため正規化済みデータを使います "
                             "(スキンメッシュが {:.4f} 倍で表示される可能性があります): {}",
                             k, filePath.string());
            }
        }
    }

    const std::filesystem::path parentDir = filePath.parent_path();

    // Build skeleton first (needed for bone weight assignment)
    auto skeleton = BuildSkeleton(scene);

    ModelData result;

    // ---------------------------------------------------------------
    // Build nodeGraph + nodeAnimClips BEFORE mesh creation
    // (needed for vertex baking with static animation pose)
    // ---------------------------------------------------------------
    std::unique_ptr<NodeGraph> nodeGraph;
    std::vector<std::unique_ptr<NodeAnimationClip>> nodeAnimClips;
    std::vector<DirectX::XMMATRIX> bakeGlobalMatrices; // per-node bake matrices (empty if not applicable)

    if (!skeleton)
    {
        nodeGraph = BuildNodeGraph(scene);
        if (nodeGraph && scene->mNumAnimations == 0)
        {
            // 静的モデル: ノード階層のデフォルト変換(mTransformation)を頂点へ焼き込む。
            // これが無いと DCC ツール側で回転/移動させたオブジェクトがメッシュローカルの
            // 姿勢のまま出てしまう(例: Blender で90度回転させた円柱が未回転で刺さる)。
            const u32 nodeCount = nodeGraph->GetNodeCount();
            bakeGlobalMatrices.resize(nodeCount, DirectX::XMMatrixIdentity());
            for (u32 i = 0; i < nodeCount; ++i)
            {
                const SceneNode& sceneNode = nodeGraph->GetNode(i);
                const DirectX::XMMATRIX localMatrix = DirectX::XMLoadFloat4x4(&sceneNode.localDefault);
                bakeGlobalMatrices[i] = (sceneNode.parentIndex >= 0)
                    ? localMatrix * bakeGlobalMatrices[static_cast<u32>(sceneNode.parentIndex)]
                    : localMatrix;
            }
        }
        else if (nodeGraph)
        {
            nodeAnimClips = BuildAllNodeAnimClips(scene, *nodeGraph);

            // ---------------------------------------------------------------
            // Compute bake matrices: rest clip at time=0
            // Uses the same S*R*T pipeline as NodeAnimator::ComputeRawNodeMatrices
            // to avoid mismatch with FBX's complex node transforms (Pivot, PreRotation, etc.)
            // ★クリップの選び方は Scene 側(inverseRest を作る所)と必ず同じにすること。
            //   描画は inv(rest) * current なので、違うクリップで焼くと残差が絵に出る。
            //   → 選択は PickRestClip に集約した（以前はここが clips[0] 決め打ちで、
            //     Scene は "static" 優先だったため "static" が先頭でないファイルで食い違った）。
            // ---------------------------------------------------------------
            // ★アニメーションが「在るが使えるトラックが 0 本」だとここが空になる
            //   （glTF のモーフターゲット専用アニメ、空の FBX テイク等。
            //   BuildAllNodeAnimClips は GetTrackCount()==0 のクリップを捨てる）。
            //   以前はその場合ベイクを丸ごと飛ばしていたので、`mNumAnimations == 0` の
            //   分岐にも `nodeAnimClips` 非空の分岐にも入らず **meshNodeTransforms が
            //   一度も作られない** → 各サブメッシュがエンティティ原点へ重なって
            //   「モデルが原点で潰れた山になる」。しかもファイルからアニメを消すと直る、
            //   という理由の分からない壊れ方だった。
            //   使えるクリップが無いなら「アニメ無し」と同じ扱い＝各ノードの既定変換で焼く。
            if (nodeAnimClips.empty())
            {
                Logger::Warn("使えるノードアニメが 1 本もありません（トラック 0 本）。"
                             "静的モデルとして各ノードの既定変換で焼きます: {}", filePath.string());
                const u32 nodeCount = nodeGraph->GetNodeCount();
                bakeGlobalMatrices.resize(nodeCount, DirectX::XMMatrixIdentity());
                for (u32 i = 0; i < nodeCount; ++i)
                {
                    const SceneNode& sceneNode = nodeGraph->GetNode(i);
                    const DirectX::XMMATRIX localMatrix =
                        DirectX::XMLoadFloat4x4(&sceneNode.localDefault);
                    bakeGlobalMatrices[i] = (sceneNode.parentIndex >= 0)
                        ? localMatrix * bakeGlobalMatrices[static_cast<u32>(sceneNode.parentIndex)]
                        : localMatrix;
                }
            }

            if (!nodeAnimClips.empty())
            {
                const NodeAnimationClip* staticClip = PickRestClip(nodeAnimClips);
                u32 nodeCount = nodeGraph->GetNodeCount();
                bakeGlobalMatrices.resize(nodeCount, DirectX::XMMatrixIdentity());

                for (u32 i = 0; i < nodeCount; ++i)
                {
                    const SceneNode& sceneNode = nodeGraph->GetNode(i);
                    const NodeTrack* track = staticClip->FindTrackForNode(i);

                    DirectX::XMMATRIX localMatrix;

                    if (track)
                    {
                        // Use time=0 keyframe values (first key or interpolated at 0)
                        DirectX::XMFLOAT3 pos = { 0.0f, 0.0f, 0.0f };
                        DirectX::XMFLOAT4 rot = { 0.0f, 0.0f, 0.0f, 1.0f };
                        DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };

                        if (!track->positionKeys.empty())
                        {
                            pos = track->positionKeys.front().value;
                        }
                        if (!track->rotationKeys.empty())
                        {
                            rot = track->rotationKeys.front().value;
                        }
                        if (!track->scaleKeys.empty())
                        {
                            scale = track->scaleKeys.front().value;
                        }

                        DirectX::XMMATRIX S = DirectX::XMMatrixScalingFromVector(DirectX::XMLoadFloat3(&scale));
                        DirectX::XMMATRIX R = DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&rot));
                        DirectX::XMMATRIX T = DirectX::XMMatrixTranslationFromVector(DirectX::XMLoadFloat3(&pos));

                        localMatrix = S * R * T;
                    }
                    else
                    {
                        localMatrix = DirectX::XMLoadFloat4x4(&sceneNode.localDefault);
                    }

                    if (sceneNode.parentIndex >= 0)
                    {
                        bakeGlobalMatrices[i] = localMatrix * bakeGlobalMatrices[static_cast<u32>(sceneNode.parentIndex)];
                    }
                    else
                    {
                        bakeGlobalMatrices[i] = localMatrix;
                    }
                }

                Logger::Info("Bake matrices computed from static clip '{}' for {} nodes",
                             staticClip->GetName(), nodeCount);
            }
        }
    }

    // ---------------------------------------------------------------
    // Create meshes (with vertex baking for node animation)
    // ---------------------------------------------------------------
    // ★どの (メッシュ, 焼き込み先ノード) を作るかの一覧。
    //   既定は「aiMesh 1 個につき 1 個」。ただし**静的モデル**（スキンもアニメも無い）は
    //   ノードごとに 1 個ずつ作る。glTF は同じ mesh を複数ノードから参照できるので
    //   （チェスセットの駒、並べた椅子など）、1 個しか作らないと**2 個目以降が丸ごと
    //   消える**（実際 ABeautifulGame.glb は駒が 32 個あるのに 5 個しか出ていなかった）。
    //   焼き込み行列はノードごとに違うので、頂点も個別に持つしかない。
    //   ★ノードアニメ経路（mNumAnimations > 0）はここでは展開しない。あちらは
    //     meshNodeTransforms を描画時に掛ける別の仕組みで、メッシュ枠とノードの対応を
    //     前提にしているため。必要になったらそちらは別途。
    struct MeshBuildRef { unsigned int meshIdx; i32 bakeNode; };
    std::vector<MeshBuildRef> buildRefs;
    const bool expandPerNode = (nodeGraph && !skeleton && scene->mNumAnimations == 0);
    if (expandPerNode)
    {
        for (u32 n = 0; n < nodeGraph->GetNodeCount(); ++n)
            for (u32 mi : nodeGraph->GetNode(n).meshIndices)
                if (mi < scene->mNumMeshes)
                    buildRefs.push_back({mi, static_cast<i32>(n)});
        if (buildRefs.size() > scene->mNumMeshes)
            Logger::Info("同じメッシュを複数ノードが参照しています: {} → 描画単位 {} 個へ展開 ({})",
                         scene->mNumMeshes, buildRefs.size(), filePath.filename().string());

        // ★どのノードからも参照されていないメッシュを取りこぼさない。
        //   ノード走査だけで組を作ると、孤立メッシュが**黙って消える**（従来は全 aiMesh を
        //   作っていたので出ていた）。壊れたファイルで「モデルの一部が無い」を作らないため、
        //   参照が無いものは従来どおり bakeNode=-1 で 1 個作る。
        std::vector<bool> referenced(scene->mNumMeshes, false);
        for (const auto& r : buildRefs) referenced[r.meshIdx] = true;
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
            if (!referenced[i]) buildRefs.push_back({i, -1});
    }
    if (buildRefs.empty())
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
            buildRefs.push_back({i, -1});

    for (const MeshBuildRef& buildRef : buildRefs)
    {
        const unsigned int meshIdx = buildRef.meshIdx;
        const aiMesh* aiMeshPtr = scene->mMeshes[meshIdx];

        // --- Build vertex data ---
        std::vector<Vertex> vertices;
        vertices.reserve(aiMeshPtr->mNumVertices);

        for (unsigned int v = 0; v < aiMeshPtr->mNumVertices; ++v)
        {
            Vertex vertex = {};

            // Position
            vertex.position.x = aiMeshPtr->mVertices[v].x;
            vertex.position.y = aiMeshPtr->mVertices[v].y;
            vertex.position.z = aiMeshPtr->mVertices[v].z;

            // Normal
            if (aiMeshPtr->mNormals)
            {
                vertex.normal.x = aiMeshPtr->mNormals[v].x;
                vertex.normal.y = aiMeshPtr->mNormals[v].y;
                vertex.normal.z = aiMeshPtr->mNormals[v].z;
            }

            // Color (white default)
            if (aiMeshPtr->mColors[0])
            {
                vertex.color.x = aiMeshPtr->mColors[0][v].r;
                vertex.color.y = aiMeshPtr->mColors[0][v].g;
                vertex.color.z = aiMeshPtr->mColors[0][v].b;
                vertex.color.w = aiMeshPtr->mColors[0][v].a;
            }
            else
            {
                vertex.color = { 1.0f, 1.0f, 1.0f, 1.0f };
            }

            // TexCoord
            if (aiMeshPtr->mTextureCoords[0])
            {
                vertex.texCoord.x = aiMeshPtr->mTextureCoords[0][v].x;
                vertex.texCoord.y = aiMeshPtr->mTextureCoords[0][v].y;
            }

            // Tangent (PBR normal mapping用)
            if (aiMeshPtr->mTangents)
            {
                vertex.tangent.x = aiMeshPtr->mTangents[v].x;
                vertex.tangent.y = aiMeshPtr->mTangents[v].y;
                vertex.tangent.z = aiMeshPtr->mTangents[v].z;
                vertex.tangent.w = 1.0f;
                if (aiMeshPtr->mBitangents)
                {
                    // handedness 判定
                    XMVECTOR N = XMLoadFloat3(&vertex.normal);
                    XMVECTOR T = XMVectorSet(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, 0);
                    XMVECTOR B_expected = XMVectorSet(
                        aiMeshPtr->mBitangents[v].x,
                        aiMeshPtr->mBitangents[v].y,
                        aiMeshPtr->mBitangents[v].z, 0);
                    XMVECTOR B_computed = XMVector3Cross(N, T);
                    float dot = XMVectorGetX(XMVector3Dot(B_computed, B_expected));
                    vertex.tangent.w = (dot < 0.0f) ? -1.0f : 1.0f;
                }
            }

            // boneIndices / boneWeights are zero-initialized by default
            vertices.push_back(vertex);
        }

        // Write bone weights if skeleton exists
        if (skeleton && aiMeshPtr->mNumBones > 0)
        {
            WriteBoneWeightsToVertices(aiMeshPtr, vertices, *skeleton);
        }

        // --- Bake vertices with static animation's global matrix ---
        // Transform mesh vertices into model space using the rest pose (time=0)
        // so that inv(rest) * current at render time produces correct results
        if (nodeGraph && !bakeGlobalMatrices.empty())
        {
            // 展開したときは「この描画単位が属するノード」が確定している。
            // していないときだけ従来どおり最初に見つかったノードを使う。
            i32 nodeIdx = (buildRef.bakeNode >= 0) ? buildRef.bakeNode
                                                   : nodeGraph->FindNodeForMesh(meshIdx);
            if (nodeIdx >= 0)
            {
                DirectX::XMMATRIX bakeMatrix = bakeGlobalMatrices[static_cast<u32>(nodeIdx)];
                // ★鏡映（行列式が負）を含むノードでは接空間の利き手が反転する。
                //   tangent.w は法線マップのビタンジェント符号なので、ここで一緒に返さないと
                //   凹凸が逆に見える。
                const float bakeDet =
                    DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(bakeMatrix));
                const float handednessFlip = (bakeDet < 0.0f) ? -1.0f : 1.0f;

                for (auto& vert : vertices)
                {
                    // Transform position
                    DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&vert.position);
                    pos = DirectX::XMVector3TransformCoord(pos, bakeMatrix);
                    DirectX::XMStoreFloat3(&vert.position, pos);

                    // Transform normal
                    DirectX::XMVECTOR norm = DirectX::XMLoadFloat3(&vert.normal);
                    norm = DirectX::XMVector3TransformNormal(norm, bakeMatrix);
                    norm = DirectX::XMVector3Normalize(norm);
                    DirectX::XMStoreFloat3(&vert.normal, norm);

                    // ★tangent も変換する。position/normal だけ回して tangent を
                    //   メッシュローカルのまま残していたので、ノードに回転を持つ静的モデル
                    //   （DamagedHelmet.glb はノードに +90°X を持つ）で**法線マップの向きだけ
                    //   狂っていた**。凹凸が斜めに寝る形で出るので「ライティングがなんか変」に
                    //   しか見えず、原因が分かりにくい。
                    DirectX::XMVECTOR tan = DirectX::XMVectorSet(
                        vert.tangent.x, vert.tangent.y, vert.tangent.z, 0.0f);
                    tan = DirectX::XMVector3TransformNormal(tan, bakeMatrix);
                    // 退化（スケール 0 の軸など）で 0 ベクトルになったら元の値を残す
                    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(tan)) > 1.0e-12f)
                    {
                        tan = DirectX::XMVector3Normalize(tan);
                        vert.tangent.x = DirectX::XMVectorGetX(tan);
                        vert.tangent.y = DirectX::XMVectorGetY(tan);
                        vert.tangent.z = DirectX::XMVectorGetZ(tan);
                    }
                    vert.tangent.w *= handednessFlip;
                }
            }
        }

        // --- Build index data ---
        std::vector<u32> indices;
        for (unsigned int f = 0; f < aiMeshPtr->mNumFaces; ++f)
        {
            const aiFace& face = aiMeshPtr->mFaces[f];
            for (unsigned int i = 0; i < face.mNumIndices; ++i)
            {
                indices.push_back(face.mIndices[i]);
            }
        }

        // --- UV Transform（タイリング/オフセット）を頂点 UV へ焼く ---
        //
        // ★ここは元々 mesh->Initialize() の**後ろ**にあった。Initialize は入口で
        //   vertices のコピーを取り meshoptimizer で並べ替えてから VRAM へ上げ切るので、
        //   後からローカルの vertices を書き換えても一切反映されない。
        //   それでいて「[UV] scale=(4.00,4.00)」というデバッグ出力だけは出るので、
        //   効いているように見えていた（この関数内で vertices を読む箇所は 979 が最後だった）。
        //   ※アップロード後に m_verticesCache を書き換える方式は不可（並びが変わっている）。
        if (aiMeshPtr->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* uvMat = scene->mMaterials[aiMeshPtr->mMaterialIndex];
            aiUVTransform uvTransform;
            if (uvMat->Get(AI_MATKEY_UVTRANSFORM(aiTextureType_DIFFUSE, 0), uvTransform) == AI_SUCCESS)
            {
                const float sx = uvTransform.mScaling.x;
                const float sy = uvTransform.mScaling.y;
                const float ox = uvTransform.mTranslation.x;
                const float oy = uvTransform.mTranslation.y;
                if (sx != 1.0f || sy != 1.0f || ox != 0.0f || oy != 0.0f)
                {
                    for (auto& v : vertices)
                    {
                        v.texCoord.x = v.texCoord.x * sx + ox;
                        v.texCoord.y = v.texCoord.y * sy + oy;
                    }
                    char dbg[256];
                    snprintf(dbg, sizeof(dbg), "[UV] scale=(%.2f,%.2f) offset=(%.2f,%.2f)\n",
                             sx, sy, ox, oy);
                    OutputDebugStringA(dbg);
                }
            }
        }

        // --- Create Mesh ---
        auto mesh = std::make_unique<Mesh>();
        mesh->Initialize(device, vertices, indices, cmdList);   // DEFAULT ヒープ(VRAM)常駐

        // --- Create Material ---
        auto material = std::make_unique<Material>();

        if (aiMeshPtr->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* aiMat = scene->mMaterials[aiMeshPtr->mMaterialIndex];

            // UV Transform は mesh->Initialize より前で処理済み（上のブロック）

            if (aiMat->GetTextureCount(aiTextureType_DIFFUSE) > 0)
            {
                aiString texPath;
                if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                {
                    {
                        char dbg[512];
                        snprintf(dbg, sizeof(dbg), "[Texture] mat=%u path='%s' embedded=%u\n",
                            aiMeshPtr->mMaterialIndex, texPath.C_Str(), scene->mNumTextures);
                        OutputDebugStringA(dbg);
                    }

                    // Assimp: 埋め込みテクスチャは "*N" か GetEmbeddedTexture で取得
                    const aiTexture* embTex = scene->GetEmbeddedTexture(texPath.C_Str());
                    if (embTex)
                    {
                        // 埋め込みテクスチャ（パスが "*N" でなくても GetEmbeddedTexture で見つかる）
                        std::string key = filePath.string() + "_emb_" + texPath.C_Str();
                        if (embTex->mHeight == 0)
                        {
                            Texture* texture = resourceManager.GetOrLoadEmbeddedTexture(
                                key,
                                reinterpret_cast<const uint8_t*>(embTex->pcData),
                                embTex->mWidth,
                                embTex->achFormatHint,
                                cmdList, /*srgb=*/true, TextureUsage::BaseColor);
                            if (texture)
                                material->albedoTexture = texture;
                        }
                        else
                        {
                            // ★mHeight != 0 = assimp が非圧縮 ARGB8888 で返した埋め込みテクスチャ。
                            //   ここに else が無く、**何のログも出さずに捨てて**いた。
                            //   結果 albedoTexture が null のまま既定の白テクスチャが張られ、
                            //   「読み込みは成功したのにモデルが真っ白」になる
                            //   （外部パスが見つからない側はちゃんと警告を出しているので、片方だけ無言だった）。
                            Logger::Warn("埋め込みテクスチャが非圧縮形式のため読み込めません "
                                         "({}x{}, hint='{}')。モデルは白いまま描画されます: {}",
                                         embTex->mWidth, embTex->mHeight,
                                         embTex->achFormatHint, filePath.string());
                        }
                    }
                    else if (texPath.C_Str()[0] == '*')
                    {
                        // 埋め込みテクスチャ: "*0", "*1" → インデックス
                        int embIdx = std::atoi(texPath.C_Str() + 1);
                        if (embIdx >= 0 && static_cast<unsigned>(embIdx) < scene->mNumTextures)
                        {
                            const aiTexture* aiTex = scene->mTextures[embIdx];
                            if (aiTex->mHeight == 0)
                            {
                                // 圧縮形式（jpg/png等）: mWidth = バイトサイズ
                                std::string key = filePath.string() + "_emb_" + std::to_string(embIdx);
                                Texture* texture = resourceManager.GetOrLoadEmbeddedTexture(
                                    key,
                                    reinterpret_cast<const uint8_t*>(aiTex->pcData),
                                    aiTex->mWidth,
                                    aiTex->achFormatHint,
                                    cmdList, /*srgb=*/true, TextureUsage::BaseColor);
                                if (texture)
                                    material->albedoTexture = texture;
                            }
                        }
                    }
                    else
                    {
                        auto resolvedPath = ResolveTexturePath(texPath.C_Str(), parentDir);
                        if (!resolvedPath.empty())
                        {
                            Texture* texture = resourceManager.GetOrLoadTexture(
                                resolvedPath.wstring(), cmdList,
                                /*srgb=*/true, TextureUsage::BaseColor);
                            if (texture)
                            {
                                material->albedoTexture = texture;
                            }
                        }
                        else
                        {
                            Logger::Warn("テクスチャが見つかりません: {}", texPath.C_Str());
                        }
                    }
                }
            }

            // --- PBR: Normal Map ---
            // usage は BC 圧縮の形式選択（Normal→BC5_UNORM / NonColor→BC7_UNORM）に使う。
            auto loadPBRTexture = [&](aiTextureType type, bool srgb, TextureUsage usage) -> Texture* {
                if (aiMat->GetTextureCount(type) == 0) return nullptr;
                aiString tp;
                if (aiMat->GetTexture(type, 0, &tp) != AI_SUCCESS) return nullptr;

                const aiTexture* emb = scene->GetEmbeddedTexture(tp.C_Str());
                if (emb && emb->mHeight == 0)
                {
                    std::string key = filePath.string() + "_emb_" + tp.C_Str();
                    return resourceManager.GetOrLoadEmbeddedTexture(
                        key, reinterpret_cast<const uint8_t*>(emb->pcData),
                        emb->mWidth, emb->achFormatHint, cmdList, srgb, usage);
                }
                if (tp.C_Str()[0] != '*')
                {
                    auto resolved = ResolveTexturePath(tp.C_Str(), parentDir);
                    if (!resolved.empty())
                        return resourceManager.GetOrLoadTexture(resolved.wstring(), cmdList, srgb, usage);
                }
                return nullptr;
            };

            // Normal map (linear)
            material->normalMapTexture = loadPBRTexture(aiTextureType_NORMALS, false, TextureUsage::Normal);
            if (!material->normalMapTexture)
                material->normalMapTexture = loadPBRTexture(aiTextureType_HEIGHT, false, TextureUsage::Normal);

            // Metalness (linear)
            material->metalRoughnessTexture = loadPBRTexture(aiTextureType_METALNESS, false, TextureUsage::NonColor);
            if (!material->metalRoughnessTexture)
                material->metalRoughnessTexture = loadPBRTexture(aiTextureType_DIFFUSE_ROUGHNESS, false, TextureUsage::NonColor);

            // PBR scalar factors
            float metallic = 0.0f, roughness = 0.5f;
            aiMat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
            aiMat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
            material->defaultMetallic = metallic;
            material->defaultRoughness = roughness;
        }

        // PBR SRVブロック確保（albedo, normal, metalRoughness の3連続SRV）
        {
            auto* srvHeap = resourceManager.GetSrvHeap();
            auto* dev = resourceManager.GetDevice();
            Texture* albedo = material->albedoTexture ? material->albedoTexture : resourceManager.GetDefaultWhiteTexture();
            Texture* normal = material->normalMapTexture ? material->normalMapTexture : resourceManager.GetDefaultNormalTexture();
            Texture* mr     = material->metalRoughnessTexture ? material->metalRoughnessTexture : resourceManager.GetDefaultMetalRoughnessTexture();

            // albedo/normal/metalRoughness は連続3スロットでなければならない
            // (描画側が srvBlockIndex 起点のテーブルとして1回でバインドするため)。
            // free-list 化後は個別確保だと連続性が保証されないので必ずブロック確保する。
            u32 blockStart = srvHeap->AllocateBlock(3);

            albedo->CreateSRV(*dev, srvHeap->GetCpuHandle(blockStart));
            normal->CreateSRV(*dev, srvHeap->GetCpuHandle(blockStart + 1));
            mr->CreateSRV(*dev, srvHeap->GetCpuHandle(blockStart + 2));

            material->srvBlockIndex = blockStart;
        }

        mesh->SetMaterial(material.get());

        result.meshes.push_back(std::move(mesh));
        result.materials.push_back(std::move(material));
    }

    // Build animation clips if skeleton exists
    if (skeleton)
    {
        result.animClips = BuildAllAnimationClips(scene, *skeleton);
    }

    // Move nodeGraph + nodeAnimClips into result
    result.nodeGraph = std::move(nodeGraph);
    result.nodeAnimClips = std::move(nodeAnimClips);

    result.skeleton = std::move(skeleton);

    Logger::Info("Model loaded: {} ({} meshes)", filePath.string(),
                 static_cast<u32>(result.meshes.size()));

    return result;
}

std::vector<std::unique_ptr<AnimationClip>> ModelLoader::LoadAnimationsFromFile(
    const std::filesystem::path& filePath,
    const Skeleton& skeleton)
{
    Assimp::Importer importer;
    importer.SetIOHandler(new VfsIOSystem()); // importer が所有権を持ち dtor で delete する

    // kUnitScaleFlag は付けない。この関数は既存スケルトンへ流し込む用＝常にスキンメッシュで、
    // LoadFromFile 側もスキンなら正規化を外して読んでいる。ここだけ掛けると位置キーの単位が
    // ボーンとずれてキャラが吹っ飛ぶ。
    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_LimitBoneWeights |
        aiProcess_PopulateArmatureData;

    const aiScene* scene = importer.ReadFile(filePath.string(), flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        Logger::Error("アニメーションの読み込みに失敗しました: {}", filePath.string());
        return {};
    }

    auto clips = BuildAllAnimationClips(scene, skeleton);

    // ファイル名をプレフィックスとしてクリップ名にセット（名前が空の場合）
    std::string filePrefix = filePath.stem().string();
    for (auto& clip : clips)
    {
        if (clip->GetName().empty())
        {
            clip->SetName(filePrefix);
        }
    }

    Logger::Info("Loaded {} animation(s) from {}", clips.size(), filePath.string());
    return clips;
}

} // namespace dx12e
