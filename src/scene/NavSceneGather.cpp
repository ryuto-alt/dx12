#include "scene/NavSceneGather.h"

#include "ecs/Components.h"
#include "ecs/InstanceGroup.h"   // インスタンス群: インスタンスごとに展開して集める
#include "renderer/Mesh.h"
// SkeletalAnimation は unique_ptr メンバを持つので、entt がストレージを実体化するとき
// 完全型が要る（try_get/all_of だけでもデストラクタが必要）。
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"

#include <DirectXMath.h>
#include <algorithm>

namespace dx12e
{

using namespace DirectX;

bool GatherNavGeometry(entt::registry& reg, nav::NavInputGeometry& out, NavGatherStats& stats)
{
    out.Clear();
    stats = NavGatherStats{};

    // 事前に総三角形数を数えて 1 回で確保する（大きなシーンだと再確保が効いてくる）。
    size_t vertGuess = 0, triGuess = 0;
    auto view = reg.view<const Transform, const MeshRenderer>(entt::exclude<GridPlane, InstanceGroup>);
    auto groupView = reg.view<const Transform, const MeshRenderer, const InstanceGroup>(entt::exclude<GridPlane>);
    for (auto [e, transform, renderer] : view.each())
    {
        for (const Mesh* m : renderer.meshes)
        {
            if (!m) continue;
            vertGuess += m->GetPositions().size();
            triGuess  += m->GetIndices().size() / 3;
        }
    }
    for (auto [e, transform, renderer, grp] : groupView.each())
    {
        const size_t n = grp._set ? grp._set->items.size() : 0;
        for (const Mesh* m : renderer.meshes)
        {
            if (!m) continue;
            vertGuess += m->GetPositions().size() * n;
            triGuess  += m->GetIndices().size() / 3 * n;
        }
    }
    out.verts.reserve(vertGuess * 3);
    out.tris.reserve(triGuess * 3);

    // 1 つのワールド行列ぶんのメッシュを積む（普通のエンティティも、インスタンス群の各インスタンスも同じ式）。
    auto appendEntity = [&](const MeshRenderer& renderer, const XMMATRIX& world) -> bool
    {
        bool used = false;
        for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
        {
            const Mesh* mesh = renderer.meshes[mi];
            if (!mesh) continue;
            const std::vector<u32>&      idx = mesh->GetIndices();
            const std::vector<XMFLOAT3>& pos = mesh->GetPositions();
            if (idx.size() < 3 || pos.empty()) continue;

            // 描画側と同じくノード行列を掛ける（掛けないとノードアニメ付きモデルでズレる）。
            XMMATRIX meshWorld = world;
            if (mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
                meshWorld = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]) * world;

            const i32 base = static_cast<i32>(out.verts.size() / 3);
            for (const XMFLOAT3& p : pos)
            {
                XMFLOAT3 w{};
                XMStoreFloat3(&w, XMVector3Transform(XMLoadFloat3(&p), meshWorld));
                out.verts.push_back(w.x);
                out.verts.push_back(w.y);
                out.verts.push_back(w.z);
            }
            const u32 vertCount = static_cast<u32>(pos.size());
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
            {
                const u32 i0 = idx[t + 0], i1 = idx[t + 1], i2 = idx[t + 2];
                if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;
                out.tris.push_back(base + static_cast<i32>(i0));
                out.tris.push_back(base + static_cast<i32>(i1));
                out.tris.push_back(base + static_cast<i32>(i2));
                ++stats.triCount;
            }
            ++stats.meshCount;
            used = true;
        }
        return used;
    };

    // ---- インスタンス群: インスタンスごとに展開（スキン・nav 無視タグは群のエンティティで判定）----
    for (auto [e, transform, renderer, grp] : groupView.each())
    {
        if (renderer.meshes.empty() || !grp._set) continue;
        if (reg.all_of<SkeletalAnimation>(e)) { ++stats.skippedSkinned; continue; }
        if (const Tag* tag = reg.try_get<Tag>(e))
        {
            if (std::find(tag->tags.begin(), tag->tags.end(), kNavIgnoreTag) != tag->tags.end())
            { ++stats.skippedTagged; continue; }
        }
        const auto& worlds = instgroup::WorldMatrices(reg, e, *grp._set);
        bool any = false;
        for (const auto& wf : worlds)
            any = appendEntity(renderer, XMLoadFloat4x4(&wf)) || any;
        if (any) ++stats.entityCount;
    }

    for (auto [e, transform, renderer] : view.each())
    {
        if (renderer.meshes.empty()) continue;

        // スキンメッシュは CPU 側がバインドポーズのままで、実際に見えている形と一致しない。
        // 動くキャラを地形として焼いても意味が無いので丸ごと外す。
        if (reg.all_of<SkeletalAnimation>(e)) { ++stats.skippedSkinned; continue; }

        if (const Tag* tag = reg.try_get<Tag>(e))
        {
            if (std::find(tag->tags.begin(), tag->tags.end(), kNavIgnoreTag) != tag->tags.end())
            { ++stats.skippedTagged; continue; }
        }

        const XMMATRIX world = (transform.parent != entt::null)
            ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

        const bool used = appendEntity(renderer, world);
        if (used) ++stats.entityCount;
    }

    out.ComputeBounds();
    return !out.Empty();
}

} // namespace dx12e
