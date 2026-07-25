#include "editor/ScenePick.h"
#include "editor/RayGeometry.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Mesh.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace dx12e
{

using namespace DirectX;

namespace
{
    // ブロードフェーズを通過した 1 エンティティ。ここまでで ComputeWorldMatrix は
    // 1 回も呼んでいない（描画リストのワールド行列をそのまま借りている）。
    struct PickCandidate
    {
        entt::entity        e        = entt::null;
        const MeshRenderer* renderer = nullptr;
        XMFLOAT4X4          world{};
        // スキンドか（描画リストの SkinningBuffer 有無で判定。エディタ側で
        // reg.all_of<SkeletalAnimation>() を呼ぶと unique_ptr メンバの完全型が必要になり、
        // アニメーションのヘッダを丸ごと引きずるので、描画リストの情報で済ませる）。
        bool                skinned  = false;
        f32                 tSphere  = 0.0f;
    };

    // ワールドスプライト(worldSpace=true)のクアッド 2 三角形でレイ判定。
    // 描画と同じ 4 隅を作るので、絵の全面がクリック対象になる。
    f32 RayTestSprite(const Sprite2D& sp, const XMMATRIX& world,
                      XMVECTOR rayOrigin, XMVECTOR rayDir,
                      XMVECTOR camRight, XMVECTOR camUp)
    {
        const f32 hx = sp.size.x * 0.5f, hy = sp.size.y * 0.5f;
        XMVECTOR tl, tr, br, bl;
        if (sp.billboard)
        {
            const XMVECTOR ctr = world.r[3];
            const f32 sx = XMVectorGetX(XMVector3Length(world.r[0]));
            const f32 sy = XMVectorGetX(XMVector3Length(world.r[1]));
            const XMVECTOR R = XMVectorScale(camRight, hx * sx);
            const XMVECTOR U = XMVectorScale(camUp,    hy * sy);
            tl = XMVectorSubtract(XMVectorAdd(ctr, U), R);
            tr = XMVectorAdd(XMVectorAdd(ctr, U), R);
            br = XMVectorAdd(XMVectorSubtract(ctr, U), R);
            bl = XMVectorSubtract(XMVectorSubtract(ctr, U), R);
        }
        else
        {
            tl = XMVector3Transform(XMVectorSet(-hx,  hy, 0.0f, 1.0f), world);
            tr = XMVector3Transform(XMVectorSet( hx,  hy, 0.0f, 1.0f), world);
            br = XMVector3Transform(XMVectorSet( hx, -hy, 0.0f, 1.0f), world);
            bl = XMVector3Transform(XMVectorSet(-hx, -hy, 0.0f, 1.0f), world);
        }
        const f32 t0 = raygeo::RayTriangle(rayOrigin, rayDir, tl, tr, br);
        const f32 t1 = raygeo::RayTriangle(rayOrigin, rayDir, tl, br, bl);
        if (t0 < 0.0f) return t1;
        if (t1 < 0.0f) return t0;
        return (std::min)(t0, t1);
    }
} // namespace

void ScreenRay(const Camera& camera,
               f32 vpX, f32 vpY, f32 vpW, f32 vpH,
               f32 screenX, f32 screenY,
               XMFLOAT3& outOrigin, XMFLOAT3& outDir)
{
    outOrigin = {0.0f, 0.0f, 0.0f};
    outDir    = {0.0f, 0.0f, 1.0f};
    if (vpW <= 0.0f || vpH <= 0.0f) return;

    const f32 ndcX = ((screenX - vpX) / vpW) * 2.0f - 1.0f;
    const f32 ndcY = 1.0f - ((screenY - vpY) / vpH) * 2.0f;

    const XMMATRIX invViewProj =
        XMMatrixInverse(nullptr, camera.GetViewMatrix() * camera.GetProjectionMatrix());

    XMVECTOR pNear = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
    XMVECTOR pFar  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
    const f32 wn = XMVectorGetW(pNear), wf = XMVectorGetW(pFar);
    if (std::abs(wn) < 1e-8f || std::abs(wf) < 1e-8f) return;
    pNear = XMVectorScale(pNear, 1.0f / wn);
    pFar  = XMVectorScale(pFar,  1.0f / wf);

    XMStoreFloat3(&outOrigin, pNear);
    XMStoreFloat3(&outDir, XMVector3Normalize(XMVectorSubtract(pFar, pNear)));
}

namespace
{
// ブロードフェーズ + ナローフェーズ（MeshRenderer のみ）。ワールドのレイだけで完結するので、
// スクリーン座標版（RaycastScene）とワールドレイ版（RaycastSceneRay）が同じ実装を共有する
// ＝エディタのクリック選択と MCP のレイキャストで結果が食い違わない。
// アイコン / ワールドスプライトはカメラ基底が要るので呼び出し側が足す。
// dir は正規化済みであること（返る distance をワールド距離として扱うため）。
void RaycastSceneMeshes(entt::registry& reg,
                        const std::vector<DrawItem>* drawItems,
                        const XMFLOAT3& orig, const XMFLOAT3& dir,
                        const ScenePickOptions& opt,
                        std::vector<ScenePickHit>& hits)
{
    const XMVECTOR rayOrigin = XMLoadFloat3(&orig);
    const XMVECTOR rayDir    = XMLoadFloat3(&dir);

    // ================= 1) ブロードフェーズ =================
    std::vector<PickCandidate> cands;
    if (drawItems && !drawItems->empty())
    {
        cands.reserve(32);
        for (const DrawItem& it : *drawItems)
        {
            const f32 ts = raygeo::RaySphere(orig, dir, it.center, it.radius);
            if (ts < 0.0f) continue;

            // DrawItem::renderer は entt ストレージへの生ポインタ。描画リストを組んだ後に
            // エディタ UI（Inspector のコンポーネント追加など）が同じフレーム内で
            // ストレージを再確保すると宙に浮く。球テストを通った数件だけ引き直せば安全＆安価。
            if (!reg.valid(it.e)) continue;
            const MeshRenderer* mr = reg.try_get<MeshRenderer>(it.e);
            if (!mr || mr->meshes.empty()) continue;

            PickCandidate c;
            c.e        = it.e;
            c.renderer = mr;
            c.world    = it.world;
            c.skinned  = (it.skin != nullptr);
            c.tSphere  = ts;
            cands.push_back(c);
        }
    }
    else
    {
        // フォールバック: 描画リストがまだ無い（起動直後のフレーム / ヘッドレス）。
        // 従来どおり entt を走査する。結果は同じで、ワールド行列を都度作るぶん遅いだけ。
        // 球は BuildDrawList と同じ作り方（メッシュAABB中心をワールドへ + スケール込み半径）。
        auto meshView = reg.view<const Transform, const MeshRenderer>(entt::exclude<GridPlane>);
        for (auto [e, transform, renderer] : meshView.each())
        {
            if (renderer.meshes.empty()) continue;

            const XMMATRIX wm = (transform.parent != entt::null)
                ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

            XMVECTOR lmn = XMVectorReplicate( FLT_MAX);
            XMVECTOR lmx = XMVectorReplicate(-FLT_MAX);
            bool hasAabb = false;
            for (const Mesh* m : renderer.meshes)
            {
                if (!m) continue;
                XMFLOAT3 a = m->GetAABBMin(), b = m->GetAABBMax();
                lmn = XMVectorMin(lmn, XMLoadFloat3(&a));
                lmx = XMVectorMax(lmx, XMLoadFloat3(&b));
                hasAabb = true;
            }
            if (!hasAabb) continue;

            const f32 ms = (std::max)((std::max)(
                XMVectorGetX(XMVector3Length(wm.r[0])),
                XMVectorGetX(XMVector3Length(wm.r[1]))),
                XMVectorGetX(XMVector3Length(wm.r[2])));
            const XMVECTOR localCenter = XMVectorScale(XMVectorAdd(lmn, lmx), 0.5f);
            const f32 meshRadius = (std::max)(1.0f,
                0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(lmx, lmn))));

            PickCandidate c;
            XMFLOAT3 center{};
            XMStoreFloat3(&center, XMVector3Transform(localCenter, wm));
            const f32 ts = raygeo::RaySphere(orig, dir, center, meshRadius * ms * 2.0f);
            if (ts < 0.0f) continue;

            c.e        = e;
            c.renderer = &renderer;
            XMStoreFloat4x4(&c.world, wm);
            // この経路ではスキン判定を省略＝バインドポーズの三角形で判定する。
            // 描画リストが無いのは起動直後の 1 フレームだけなので実害はない。
            c.skinned  = false;
            c.tSphere  = ts;
            cands.push_back(c);
        }
    }

    // 近い順。以降のナローフェーズは「AABB の t 順 ≠ 実際の三角形ヒット順」なので
    // 打ち切らず全候補を評価し、最後に三角形ヒットの t で並べ直す。
    std::sort(cands.begin(), cands.end(),
              [](const PickCandidate& a, const PickCandidate& b) { return a.tSphere < b.tSphere; });
    if (cands.size() > static_cast<size_t>(opt.maxCandidates))
        cands.resize(static_cast<size_t>(opt.maxCandidates));

    // ================= 2) ナローフェーズ =================
    for (const PickCandidate& c : cands)
    {
        // スキンメッシュは CPU 側の頂点がバインドポーズのままで、画面に見えている変形後の
        // 形とは一致しない。CPU 再スキニングはやらない方針なので AABB 止まりにする
        // （AABB もバインドポーズ基準＝おおよその当たり。実用上これで困らない）。
        const bool skinned = c.skinned;

        const XMMATRIX world = XMLoadFloat4x4(&c.world);
        const MeshRenderer& mr = *c.renderer;

        for (u32 mi = 0; mi < static_cast<u32>(mr.meshes.size()); ++mi)
        {
            const Mesh* mesh = mr.meshes[mi];
            if (!mesh) continue;

            // 描画側（Application::RenderSceneMeshes）と同じくノード行列を掛ける。
            // これを無視していたのが「ノードアニメ付きモデルで当たりがズレる」原因やった。
            // 描画側は hasNodeAnim のときだけ掛けるが、meshNodeTransforms はモデル生成時に
            // 単位行列で初期化され、NodeAnimationComp が無ければ単位行列のまま
            // （Scene.cpp の spawn 処理）。よって無条件に掛けても結果は同じで、
            // ここで NodeAnimationComp を probe せずに済む。
            XMMATRIX meshWorld = world;
            if (mi < static_cast<u32>(mr.meshNodeTransforms.size()))
                meshWorld = XMLoadFloat4x4(&mr.meshNodeTransforms[mi]) * world;

            XMVECTOR det = XMVectorZero();
            const XMMATRIX inv = XMMatrixInverse(&det, meshWorld);
            if (std::abs(XMVectorGetX(det)) < 1e-20f) continue;   // 退化（scale 0 等）

            // レイをローカル空間へ。方向は正規化しない＝返る t がワールドの t と一致する。
            XMFLOAT3 lo{}, ld{};
            XMStoreFloat3(&lo, XMVector3TransformCoord(rayOrigin, inv));
            XMStoreFloat3(&ld, XMVector3TransformNormal(rayDir, inv));

            const f32 tBox = raygeo::RayAabb(lo, ld, mesh->GetAABBMin(), mesh->GetAABBMax());
            if (tBox < 0.0f) continue;

            const std::vector<u32>&      idx = mesh->GetIndices();
            const std::vector<XMFLOAT3>& pos = mesh->GetPositions();

            ScenePickHit hit;
            hit.entity       = c.e;
            hit.submeshIndex = mi;

            if (skinned || !opt.trianglePrecise || idx.size() < 3 || pos.empty())
            {
                hit.distance = tBox;
                XMStoreFloat3(&hit.worldPos,
                              XMVectorAdd(rayOrigin, XMVectorScale(rayDir, tBox)));
                XMStoreFloat3(&hit.worldNormal, XMVectorNegate(rayDir));
                hits.push_back(hit);
                continue;
            }

            const XMVECTOR loV = XMLoadFloat3(&lo);
            const XMVECTOR ldV = XMLoadFloat3(&ld);
            const u32 vertCount = static_cast<u32>(pos.size());
            const size_t triCount = idx.size() / 3;

            f32 best = FLT_MAX;
            XMVECTOR bestNormal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            for (size_t tri = 0; tri < triCount; ++tri)
            {
                const u32 i0 = idx[tri * 3 + 0];
                const u32 i1 = idx[tri * 3 + 1];
                const u32 i2 = idx[tri * 3 + 2];
                if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;
                const XMVECTOR v0 = XMLoadFloat3(&pos[i0]);
                const XMVECTOR v1 = XMLoadFloat3(&pos[i1]);
                const XMVECTOR v2 = XMLoadFloat3(&pos[i2]);
                const f32 t = raygeo::RayTriangle(loV, ldV, v0, v1, v2);
                if (t > 0.0f && t < best)
                {
                    best = t;
                    bestNormal = XMVector3Cross(XMVectorSubtract(v1, v0),
                                                XMVectorSubtract(v2, v0));
                }
            }
            if (best == FLT_MAX) continue;   // AABB には当たったが実体には当たっていない

            hit.distance = best;
            XMStoreFloat3(&hit.worldPos, XMVectorAdd(rayOrigin, XMVectorScale(rayDir, best)));
            // 法線のローカル→ワールドは逆行列の転置（非一様スケールでも正しい向きになる）。
            XMStoreFloat3(&hit.worldNormal,
                          XMVector3Normalize(XMVector3TransformNormal(bestNormal,
                                                                      XMMatrixTranspose(inv))));
            hits.push_back(hit);
        }
    }
}

} // namespace

std::vector<ScenePickHit> RaycastScene(entt::registry& reg,
                                       const std::vector<DrawItem>* drawItems,
                                       const Camera& camera,
                                       f32 vpX, f32 vpY, f32 vpW, f32 vpH,
                                       f32 screenX, f32 screenY,
                                       const ScenePickOptions& opt)
{
    std::vector<ScenePickHit> hits;
    if (vpW <= 0.0f || vpH <= 0.0f) return hits;

    XMFLOAT3 orig{}, dir{};
    ScreenRay(camera, vpX, vpY, vpW, vpH, screenX, screenY, orig, dir);
    const XMVECTOR rayOrigin = XMLoadFloat3(&orig);
    const XMVECTOR rayDir    = XMLoadFloat3(&dir);

    const XMMATRIX view     = camera.GetViewMatrix();
    const XMMATRIX viewProj = view * camera.GetProjectionMatrix();
    const XMMATRIX invView  = XMMatrixInverse(nullptr, view);
    const XMVECTOR camRight = invView.r[0];   // ビルボードスプライトの展開用
    const XMVECTOR camUp    = invView.r[1];

    // ================= 1)+2) メッシュ（スクリーン版とワールド版で共有）=================
    RaycastSceneMeshes(reg, drawItems, orig, dir, opt, hits);

    // ================= 3) メッシュを持たないもの（アイコン / ワールドスプライト） =================
    if (opt.includeNonMesh)
    {
        // UIRect/UICanvas 持ちは 3D 空間に実体が無く（全部ワールド原点に居る）、
        // 拾うと原点付近のクリックを丸ごと奪ってしまう。UI は UI 編集モード／UI エディタで選ぶ。
        auto iconView = reg.view<const Transform>(
            entt::exclude<MeshRenderer, GridPlane, UIRect, UICanvas>);
        const f32 iconR2 = opt.iconPixelRadius * opt.iconPixelRadius;

        for (auto [e, transform] : iconView.each())
        {
            const XMMATRIX wm = (transform.parent != entt::null && reg.valid(transform.parent))
                ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

            // ワールドスプライトは実際のクアッド全面で判定（固定サイズの箱だと中央しか反応しない）
            if (const Sprite2D* sp = reg.try_get<Sprite2D>(e))
            {
                if (sp->worldSpace && !sp->texturePath.empty())
                {
                    const f32 t = RayTestSprite(*sp, wm, rayOrigin, rayDir, camRight, camUp);
                    if (t > 0.0f)
                    {
                        ScenePickHit hit;
                        hit.entity   = e;
                        hit.distance = t;
                        XMStoreFloat3(&hit.worldPos,
                                      XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t)));
                        XMStoreFloat3(&hit.worldNormal, XMVectorNegate(rayDir));
                        hits.push_back(hit);
                    }
                    continue;
                }
            }

            // アイコン: ワールド位置をスクリーンへ投影してピクセル半径で判定する。
            // 旧実装は 0.5m の固定ボックスで、遠景では豆粒・近景では巨大というデタラメな
            // 当たりやった。EditorIconRenderer のアイコンは「常に一定スクリーンサイズの
            // ビルボード」なので、見た目と一致させるにはスクリーン基準しかない。
            XMFLOAT3 wpos{};
            XMStoreFloat3(&wpos, wm.r[3]);
            const XMVECTOR clip =
                XMVector4Transform(XMVectorSet(wpos.x, wpos.y, wpos.z, 1.0f), viewProj);
            const f32 cw = XMVectorGetW(clip);
            if (cw <= 1e-4f) continue;                       // カメラの後ろ
            const f32 sx = vpX + (XMVectorGetX(clip) / cw * 0.5f + 0.5f) * vpW;
            const f32 sy = vpY + (0.5f - XMVectorGetY(clip) / cw * 0.5f) * vpH;
            const f32 dx = sx - screenX, dy = sy - screenY;
            if (dx * dx + dy * dy > iconR2) continue;

            // 奥行きはカメラ（レイ原点）からアイコン中心までのレイ方向成分。
            const f32 t = XMVectorGetX(XMVector3Dot(
                XMVectorSubtract(XMLoadFloat3(&wpos), rayOrigin), rayDir));
            if (t <= 0.0f) continue;

            ScenePickHit hit;
            hit.entity   = e;
            hit.distance = t;
            hit.isIcon   = true;
            hit.worldPos = wpos;
            XMStoreFloat3(&hit.worldNormal, XMVectorNegate(rayDir));
            hits.push_back(hit);
        }
    }

    // ================= 4) 並べ替え =================
    // 基本は t の昇順。ただしアイコンヒットはメッシュヒットより前に出す。
    // 理由: ライト/カメラ/空オブジェクトはクリックできるジオメトリを持たず、当たりは
    // 18px の的だけ。壁の手前にあろうが奥にあろうが「アイコンを狙って押した」以外の
    // 解釈が無いので、狙った的を必ず取れる方が自然（奥のメッシュは連続クリックの
    // 循環選択で 2 手目に取れる）。アイコン同士・メッシュ同士は距離順のまま。
    raygeo::SortHitsByDistance(hits);
    std::stable_sort(hits.begin(), hits.end(),
                     [](const ScenePickHit& a, const ScenePickHit& b) {
                         return a.isIcon && !b.isIcon;
                     });
    return hits;
}

std::vector<ScenePickHit> RaycastSceneRay(entt::registry& reg,
                                          const std::vector<DrawItem>* drawItems,
                                          const XMFLOAT3& origin,
                                          const XMFLOAT3& direction,
                                          f32 maxDistance,
                                          const ScenePickOptions& opt)
{
    std::vector<ScenePickHit> hits;
    const XMVECTOR d = XMLoadFloat3(&direction);
    if (!(XMVectorGetX(XMVector3LengthSq(d)) > 1e-12f)) return hits;   // 0 / NaN 方向は判定不能
    XMFLOAT3 dir{};
    XMStoreFloat3(&dir, XMVector3Normalize(d));

    RaycastSceneMeshes(reg, drawItems, origin, dir, opt, hits);

    if (maxDistance > 0.0f)
    {
        hits.erase(std::remove_if(hits.begin(), hits.end(),
                                  [maxDistance](const ScenePickHit& h) {
                                      return h.distance > maxDistance;
                                  }),
                   hits.end());
    }
    raygeo::SortHitsByDistance(hits);
    return hits;
}

} // namespace dx12e
