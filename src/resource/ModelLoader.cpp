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

} // anonymous namespace

std::vector<MeshData> ModelLoader::LoadFromFile(
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
        aiProcess_JoinIdenticalVertices;

    const aiScene* scene = importer.ReadFile(filePath.string(), flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        Logger::Error("Failed to load model: {}", filePath.string());
        return {};
    }

    const std::filesystem::path parentDir = filePath.parent_path();

    std::vector<MeshData> result;
    result.reserve(scene->mNumMeshes);

    for (unsigned int meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
    {
        const aiMesh* aiMeshPtr = scene->mMeshes[meshIdx];

        // --- 頂点データ構築 ---
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

            // Color (白がデフォルト)
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

            vertices.push_back(vertex);
        }

        // --- インデックスデータ構築 ---
        std::vector<u32> indices;
        for (unsigned int f = 0; f < aiMeshPtr->mNumFaces; ++f)
        {
            const aiFace& face = aiMeshPtr->mFaces[f];
            for (unsigned int i = 0; i < face.mNumIndices; ++i)
            {
                indices.push_back(face.mIndices[i]);
            }
        }

        // --- Mesh 作成 ---
        auto mesh = std::make_unique<Mesh>();
        mesh->Initialize(device, vertices, indices);

        // --- Material 作成 ---
        auto material = std::make_unique<Material>();

        if (aiMeshPtr->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* aiMat = scene->mMaterials[aiMeshPtr->mMaterialIndex];

            // ディフューズテクスチャ取得
            if (aiMat->GetTextureCount(aiTextureType_DIFFUSE) > 0)
            {
                aiString texPath;
                if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                {
                    // 埋め込みテクスチャはPhase 3Aでは非対応
                    if (texPath.C_Str()[0] != '*')
                    {
                        std::wstring wideTexPath = ToWideString(texPath.C_Str());

                        // 相対パスならモデルファイルの親ディレクトリからの相対パスとして解決
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

        MeshData data;
        data.mesh     = std::move(mesh);
        data.material = std::move(material);
        result.push_back(std::move(data));
    }

    Logger::Info("Model loaded: {} ({} meshes)", filePath.string(),
                 static_cast<u32>(result.size()));

    return result;
}

} // namespace dx12e
