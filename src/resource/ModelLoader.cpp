#include "resource/ModelLoader.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Texture.h"
#include "resource/ResourceManager.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <Windows.h>

namespace dx12e
{

namespace
{

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

    std::wstring result(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, result.data(), len);
    return result;
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

void BuildSkeletonRecursive(
    const aiNode* node,
    i32 parentIndex,
    const std::unordered_map<std::string, const aiBone*>& boneMap,
    Skeleton& skeleton)
{
    std::string nodeName(node->mName.C_Str());
    auto it = boneMap.find(nodeName);

    i32 currentIndex = parentIndex;

    if (it != boneMap.end())
    {
        BoneNode boneNode;
        boneNode.name = nodeName;
        boneNode.parentIndex = parentIndex;
        boneNode.inverseBindPose = ToXMFLOAT4X4(it->second->mOffsetMatrix);
        boneNode.localBindPose = ToXMFLOAT4X4(node->mTransformation);

        currentIndex = static_cast<i32>(skeleton.GetBoneCount());
        skeleton.AddBone(std::move(boneNode));
    }

    for (unsigned int ci = 0; ci < node->mNumChildren; ++ci)
    {
        BuildSkeletonRecursive(node->mChildren[ci], currentIndex, boneMap, skeleton);
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
    BuildSkeletonRecursive(scene->mRootNode, -1, boneMap, *skeleton);

    Logger::Info("Skeleton built: {} bones", skeleton->GetBoneCount());
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

    Logger::Info("AnimationClip built: '{}' {} tracks, duration={:.2f}",
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

} // anonymous namespace

ModelData ModelLoader::LoadFromFile(
    GraphicsDevice& device,
    ID3D12GraphicsCommandList* cmdList,
    const std::filesystem::path& filePath,
    ResourceManager& resourceManager)
{
    Assimp::Importer importer;

    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_GenNormals |
        aiProcess_JoinIdenticalVertices |
        aiProcess_LimitBoneWeights;

    const aiScene* scene = importer.ReadFile(filePath.string(), flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        Logger::Error("Failed to load model: {}", filePath.string());
        return {};
    }

    const std::filesystem::path parentDir = filePath.parent_path();

    // Build skeleton first (needed for bone weight assignment)
    auto skeleton = BuildSkeleton(scene);

    ModelData result;

    for (unsigned int meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
    {
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

            // boneIndices / boneWeights are zero-initialized by default
            vertices.push_back(vertex);
        }

        // Write bone weights if skeleton exists
        if (skeleton && aiMeshPtr->mNumBones > 0)
        {
            WriteBoneWeightsToVertices(aiMeshPtr, vertices, *skeleton);
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

        // --- Create Mesh ---
        auto mesh = std::make_unique<Mesh>();
        mesh->Initialize(device, vertices, indices);

        // --- Create Material ---
        auto material = std::make_unique<Material>();

        if (aiMeshPtr->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* aiMat = scene->mMaterials[aiMeshPtr->mMaterialIndex];

            if (aiMat->GetTextureCount(aiTextureType_DIFFUSE) > 0)
            {
                aiString texPath;
                if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                {
                    if (texPath.C_Str()[0] != '*')
                    {
                        std::wstring wideTexPath = ToWideString(texPath.C_Str());

                        std::filesystem::path fullTexPath(wideTexPath);
                        if (fullTexPath.is_relative())
                        {
                            fullTexPath = parentDir / fullTexPath;
                        }

                        Texture* texture = resourceManager.GetOrLoadTexture(
                            fullTexPath.wstring(), cmdList);

                        if (texture)
                        {
                            material->albedoTexture = texture;
                        }
                    }
                }
            }
        }

        mesh->SetMaterial(material.get());

        result.meshes.push_back(std::move(mesh));
        result.materials.push_back(std::move(material));
    }

    // Build animation clips if skeleton and animations exist
    if (skeleton)
    {
        result.animClips = BuildAllAnimationClips(scene, *skeleton);
    }

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

    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_LimitBoneWeights;

    const aiScene* scene = importer.ReadFile(filePath.string(), flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        Logger::Error("Failed to load animations: {}", filePath.string());
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
