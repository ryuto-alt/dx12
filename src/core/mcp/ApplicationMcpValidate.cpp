// ===========================================================================
// MCP: 配置検査（validate_layout）— AI が置いた物の「埋まり / ちらつき / 二重 / 当たり無し」を機械が拾う
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
//
// なぜ要るか（2026-09-10 にユーザーと合意した設計）:
//   測る道具（get_bounds / raycast_precise / snap_to_ground / list_lights）は前から揃っていた。
//   足りなかったのは **強制力**。AI は自分で検査を思いつかない限り検査しないし、
//   ツールの返り値に書いていないことは見ない。だから
//     ① ここで「置いた結果」を一括検査して entityId 付きで返し、
//     ② play / save_scene の返り値へ要約を必ず載せる（ApplicationMcp.cpp の McpLayoutSummary）
//   の 2 段構えにしてある。②が本体で、①だけ足しても効かない。
//
// 検査は全部「ワールド AABB」と「三角形精密レイキャスト」だけで済ませてある。
// 物理（Jolt）を使わないので **Editor モードで動く**（Play しないと分からない、では手遅れ）。
// ===========================================================================
#include "core/ApplicationInternal.h"

#include <set>   // 二重配置ペアの記録（ApplicationInternal.h は unordered_set しか引いていない）

namespace dx12e
{
using namespace appdetail;

namespace
{
using DirectX::XMFLOAT3;

struct Box
{
    XMFLOAT3 mn{}, mx{};
    float SizeX() const { return mx.x - mn.x; }
    float SizeY() const { return mx.y - mn.y; }
    float SizeZ() const { return mx.z - mn.z; }
    float MaxSide() const { return (std::max)({SizeX(), SizeY(), SizeZ()}); }
    float MinSide() const { return (std::min)({SizeX(), SizeY(), SizeZ()}); }
    float Volume() const { return (std::max)(0.0f, SizeX()) * (std::max)(0.0f, SizeY())
                                * (std::max)(0.0f, SizeZ()); }
};

// 子孫を含めたワールド AABB。モデルのルートが empty なことが多いので、
// エンティティ単体の AABB だけを見ると「大きさゼロの点」として扱ってしまう。
bool WorldAabbDeep(const entt::registry& reg, entt::entity e, Box& out, bool& outHasMesh)
{
    XMFLOAT3 mn, mx;
    bool hasMesh = false;
    if (!McpWorldAabb(reg, e, mn, mx, hasMesh)) return false;
    for (auto c : reg.view<const Transform>())
    {
        if (c == e || !McpIsDescendantOf(reg, c, e)) continue;
        XMFLOAT3 cmn, cmx;
        bool chm = false;
        if (!McpWorldAabb(reg, c, cmn, cmx, chm) || !chm) continue;
        if (!hasMesh) { mn = cmn; mx = cmx; hasMesh = true; continue; }
        mn = { (std::min)(mn.x, cmn.x), (std::min)(mn.y, cmn.y), (std::min)(mn.z, cmn.z) };
        mx = { (std::max)(mx.x, cmx.x), (std::max)(mx.y, cmx.y), (std::max)(mx.z, cmx.z) };
    }
    out.mn = mn; out.mx = mx;
    outHasMesh = hasMesh;
    return true;
}

// 2 つの AABB の重なり。重ならなければ false。
bool Intersect(const Box& a, const Box& b, Box& out)
{
    out.mn = { (std::max)(a.mn.x, b.mn.x), (std::max)(a.mn.y, b.mn.y), (std::max)(a.mn.z, b.mn.z) };
    out.mx = { (std::min)(a.mx.x, b.mx.x), (std::min)(a.mx.y, b.mx.y), (std::min)(a.mx.z, b.mx.z) };
    return out.mx.x > out.mn.x && out.mx.y > out.mn.y && out.mx.z > out.mn.z;
}

std::string NameOf(const entt::registry& reg, entt::entity e)
{
    return reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string("(no name)");
}

// 「置き物（プロップ）」か。床・巨大な壁・足場のようなレベル形状は対象外にする。
//
// なぜ分けるか（実測して決めた 2026-09-10）:
//   接地の検査をレベル形状に掛けると使い物にならない報告が出る。
//   ・20m の床は「その下の板に 0.2m 埋まっている」と言われる（床が床に乗るわけがない）
//   ・宙に浮いた足場は**そういう設計**なのに毎回「浮いている」と言われる
//   ・壁の根元が床に 10cm 刺さっているのは隙間を出さないための正しい作り方
//   検査したいのは「箱・岩・樽・椅子を置いたら埋まっていた」の方だけ。
bool IsProp(const Box& b)
{
    const float footprint = b.SizeX() * b.SizeZ();
    return footprint <= 25.0f && b.SizeY() <= 6.0f;   // 5m 四方・高さ 6m まで
}

// 「これは地面として使ってよい面か」。床・地形・大きな壁だけを地面と認める。
//
// なぜ置き物を地面から外すのか（2026-09-10 に実測で決めた）:
//   置き物を地面として認めると、物が重なっているときに嘘の報告が出る。
//   ・箱 A の上に箱 B が半分めり込んでいると、A のレイが B の底面を拾って
//     「A が 0.5m 埋まっている」と言う（A はちゃんと床に乗っているのに）
//   ・同じ場所に二重配置した双子は、互いを地面と誤認して「1m 埋まっている」になる
//   物同士のめり込みは OVERLAP が別に報告するので、ここで重ねて言う必要も無い。
//   ＝BURIED / FLOATING は**床に対する話だけ**にするのが一番嘘が少ない。
bool IsGroundLike(const entt::registry& reg, entt::entity e)
{
    if (reg.all_of<Terrain>(e)) return true;
    Box b; bool hm = false;
    if (!WorldAabbDeep(reg, e, b, hm) || !hm) return false;
    return !IsProp(b);
}

// 検査から外すもの。エディタ内部用・UI・非メッシュは配置の良し悪しを語れない。
bool Excluded(const entt::registry& reg, entt::entity e)
{
    if (reg.all_of<GridPlane>(e)) return true;      // 編集ビュー専用の床
    if (reg.all_of<UIRect>(e))    return true;      // ゲーム内 UI は 3D 空間の話ではない
    if (reg.all_of<Terrain>(e))   return true;      // 地形は「めり込んで当たり前」の相手
    return false;
}
} // namespace

// 配置検査の本体。MCP からも play/save の要約からも呼ぶので、ハンドラの外に出してある。
// fixMode: 0=検査のみ / 1=安全な修正だけ / 2=全部（重なりの押し出しも）
Application::LayoutReport Application::RunLayoutValidation(int fixMode, float tolerance)
{
    using namespace DirectX;
    LayoutReport rep;
    if (!m_scene) return rep;
    auto& reg = m_scene->GetRegistry();

    // ---- ① 全メッシュのワールド AABB を 1 巡で集める ----
    struct Item { entt::entity e; Box box; bool hasMesh; };
    std::vector<Item> items;
    items.reserve(256);
    // 「1 個のオブジェクト」の定義: **自分がメッシュを持ち、先祖にメッシュ持ちが居ない**もの。
    //
    //  ★「親が居たら飛ばす」ではいけない（2026-09-10 に実際に踏んだ）。
    //    グループ分け（dx12_organize_scene）を通した後は全部が ENV / LVL 等の空の親の下に入るので、
    //    その条件だと**検査対象が 0 件になり**、代わりにグループのルート同士を比べて
    //    「LVL が GAMEPLAY にめり込んでいる」という無意味な報告を出していた。
    //    メッシュの有無で見れば、空のグループは自動的に外れ、
    //    家具の引き出しのような「モデルの部品」も（先祖がメッシュ持ちなので）自動的に外れる。
    auto ancestorHasMesh = [&](entt::entity e) {
        entt::entity cur = reg.all_of<Transform>(e) ? reg.get<Transform>(e).parent : entt::null;
        for (int d = 0; cur != entt::null && reg.valid(cur) && d < 64; ++d)
        {
            if (reg.all_of<MeshRenderer>(cur)) return true;
            auto* pt = reg.try_get<Transform>(cur);
            cur = pt ? pt->parent : entt::null;
        }
        return false;
    };
    for (auto e : reg.view<const Transform, const MeshRenderer>())
    {
        if (Excluded(reg, e)) continue;
        if (ancestorHasMesh(e)) continue;
        Box b; bool hasMesh = false;
        if (!WorldAabbDeep(reg, e, b, hasMesh) || !hasMesh) continue;
        items.push_back({e, b, hasMesh});
    }
    rep.checked = static_cast<int>(items.size());

    auto add = [&](const char* kind, int level, entt::entity e, const std::string& text,
                   entt::entity other = entt::null)
    {
        LayoutIssue is;
        is.kind   = kind;
        is.level  = level;     // 2=error / 1=warning
        is.entity = e;
        is.other  = other;
        is.text   = text;
        rep.issues.push_back(std::move(is));
    };

    char buf[512];

    // ---- ② スケール異常 / NaN ----
    for (const auto& it : items)
    {
        const Transform& t = reg.get<Transform>(it.e);
        const float* p = &t.position.x;
        bool nan = false;
        for (int i = 0; i < 3; ++i) if (!std::isfinite(p[i])) nan = true;
        if (!std::isfinite(t.scale.x) || !std::isfinite(t.scale.y) || !std::isfinite(t.scale.z)) nan = true;
        if (nan)
        {
            add("NAN_TRANSFORM", 2, it.e,
                NameOf(reg, it.e) + ": 座標かスケールが NaN/Inf。描画もカリングも壊れる。"
                "dx12_set_transform で数値を入れ直すこと");
            continue;
        }
        if (t.scale.x <= 0.0f || t.scale.y <= 0.0f || t.scale.z <= 0.0f)
        {
            add("SCALE_ANOMALY", 2, it.e,
                NameOf(reg, it.e) + ": スケールに 0 か負の値が入っている（面が裏返る/消える）");
            continue;
        }
        const float big = it.box.MaxSide();
        if (big > 1000.0f)
        {
            std::snprintf(buf, sizeof(buf),
                "%s: 一辺 %.0fm。単位の取り違え（cm→m）か spawn スケールの事故を疑う",
                NameOf(reg, it.e).c_str(), big);
            add("SCALE_ANOMALY", 1, it.e, buf);
        }
        else if (big > 0.0f && big < 0.005f)
        {
            std::snprintf(buf, sizeof(buf),
                "%s: 一辺 %.1fmm。画面にはまず映らない。モデルの実寸は dx12_asset_info で確認",
                NameOf(reg, it.e).c_str(), big * 1000.0f);
            add("SCALE_ANOMALY", 1, it.e, buf);
        }
    }

    // ---- ③ 当たり判定の欠落 ----
    //  ★このエンジンでは RigidBody が無いと Jolt にボディが作られない。
    //    boxCollider だけ付けて満足していると、プレイヤーは床をすり抜けて落ち続ける。
    //    これは実際に何度も踏んでいる罠なので error 扱いにする。
    //
    //  「コライダーが無い」の方（NO_COLLIDER）は**物理を使っているシーンでだけ**言う。
    //  当たり判定を 1 つも使っていないシーン（見せ物・カットシーン・UI 主体）で全オブジェクトに
    //  「すり抜ける」と言い続けると、報告が渋滞して肝心のエラーが埋もれる。
    bool sceneUsesPhysics = false;
    for (auto e : reg.view<const RigidBody>())        { (void)e; sceneUsesPhysics = true; break; }
    if (!sceneUsesPhysics)
        for (auto e : reg.view<const CharacterController>()) { (void)e; sceneUsesPhysics = true; break; }

    for (const auto& it : items)
    {
        const bool hasRb   = reg.all_of<RigidBody>(it.e);
        const bool hasCc   = reg.all_of<CharacterController>(it.e);
        const bool hasCol  = reg.any_of<BoxCollider, SphereCollider, CapsuleCollider>(it.e);
        if (hasCol && !hasRb && !hasCc)
        {
            add("COLLIDER_WITHOUT_BODY", 2, it.e,
                NameOf(reg, it.e) + ": コライダーはあるが rigidBody が無い。"
                "このエンジンは rigidBody が無いと Jolt に載らない＝当たり判定は効いていない。"
                "dx12_set_component(component:\"rigidBody\", data:{motionType:0, mass:0}) を足すこと");
            continue;
        }
        // 人がぶつかりうる大きさ（2m 四方の床面 or 高さ 2m 以上）だけ言う。
        const bool walkable = (it.box.SizeX() * it.box.SizeZ() >= 4.0f) || it.box.SizeY() >= 2.0f;
        if (sceneUsesPhysics && !hasRb && !hasCc && walkable)
        {
            std::snprintf(buf, sizeof(buf),
                "%s: 一辺 %.1fm あるのに当たり判定が無い（すり抜ける）。床/壁/足場なら rigidBody を付ける",
                NameOf(reg, it.e).c_str(), it.box.MaxSide());
            add("NO_COLLIDER", 1, it.e, buf);
        }
    }

    // ---- ④ 二重配置（リトライで同じ物を 2 回置いた）----
    //  同じモデルがほぼ同じ場所に 2 つ = Z ファイティングと当たり判定の二重掛けを同時に起こす。
    //  ★「位置が同じ」だけでは足りない。原点に置いた床（板）と床（箱）のように、
    //    別物でも中心が一致することはいくらでもある。3 軸とも大きさが揃って初めて同じ物。
    std::set<std::pair<size_t, size_t>> dupPairs;
    std::set<entt::entity>              dupEntities;   // 接地検査から外す（相方を「地面」と誤認するため）
    for (size_t i = 0; i < items.size(); ++i)
    {
        for (size_t j = i + 1; j < items.size(); ++j)
        {
            const Transform& a = reg.get<Transform>(items[i].e);
            const Transform& b = reg.get<Transform>(items[j].e);
            const float d = std::sqrt(
                (a.position.x - b.position.x) * (a.position.x - b.position.x) +
                (a.position.y - b.position.y) * (a.position.y - b.position.y) +
                (a.position.z - b.position.z) * (a.position.z - b.position.z));
            if (d > 0.01f) continue;
            const Box& ba = items[i].box; const Box& bb = items[j].box;
            if (std::fabs(ba.SizeX() - bb.SizeX()) > 0.01f) continue;
            if (std::fabs(ba.SizeY() - bb.SizeY()) > 0.01f) continue;
            if (std::fabs(ba.SizeZ() - bb.SizeZ()) > 0.01f) continue;
            dupPairs.insert({i, j});
            dupEntities.insert(items[i].e);
            dupEntities.insert(items[j].e);
            add("DUPLICATE", 2, items[j].e,
                NameOf(reg, items[j].e) + " が " + NameOf(reg, items[i].e) +
                " と同じ場所に重なっている（同じ生成を 2 回撃った疑い）。"
                "片方を dx12_delete_entity で消すこと", items[i].e);
        }
    }

    // ---- ⑤ 面同士の重なり（Z ファイティング＝ちらつきの正体）----
    //  判定: 2 つの AABB の重なりが「薄い板」= 1 辺だけ極端に薄く、他 2 辺は広い。
    //  床の上に絨毯を y=0 で置いた、壁と壁紙が同一平面、といった典型をこれで拾える。
    //  ★三角形単位で見なくても、実害が出る形は必ずこの形になる。
    const float kThin = (std::max)(0.001f, tolerance);   // 既定 1mm
    for (size_t i = 0; i < items.size(); ++i)
    {
        for (size_t j = i + 1; j < items.size(); ++j)
        {
            if (dupPairs.count({i, j})) continue;   // ④で言った。同じ事故を 2 度書かない
            Box ov;
            if (!Intersect(items[i].box, items[j].box, ov)) continue;
            const float thin = ov.MinSide();
            if (thin > kThin) continue;                          // 薄くない＝ただの貫通（⑥で見る）
            const float wide = ov.MaxSide();
            if (wide < 0.10f) continue;                          // 接しているだけの角。実害なし

            const char* axis = (ov.SizeX() <= ov.SizeY() && ov.SizeX() <= ov.SizeZ()) ? "X"
                             : (ov.SizeY() <= ov.SizeZ() ? "Y" : "Z");
            // ★動かすのは**小さい方**。20m の床を 5mm 下げて絨毯に合わせるのは筋が悪い
            //   （床は他の全部の基準面なので、動かすと別の物とのちらつきを作りかねない）。
            const bool iIsSmaller = items[i].box.Volume() <= items[j].box.Volume();
            const size_t mover = iIsSmaller ? i : j;
            const size_t anchor = iIsSmaller ? j : i;
            std::snprintf(buf, sizeof(buf),
                "%s と %s の面が %s 軸で %.2fmm しか離れていない（%.1fm 四方が重なる）。"
                "描画がちらつく。小さい方の %s を %s 方向へ 5mm ずらすこと",
                NameOf(reg, items[anchor].e).c_str(), NameOf(reg, items[mover].e).c_str(),
                axis, thin * 1000.0f, wide, NameOf(reg, items[mover].e).c_str(), axis);
            add("Z_FIGHT", 2, items[mover].e, buf, items[anchor].e);
        }
    }

    // ---- ⑥ 深い貫通（めり込み）----
    //  重なった体積が小さい方の体積の一定割合を超えたら「めり込み」。
    //  壁が床に少し刺さっているのは正常なので、割合で足切りする。
    for (size_t i = 0; i < items.size(); ++i)
    {
        for (size_t j = i + 1; j < items.size(); ++j)
        {
            if (dupPairs.count({i, j})) continue;   // ④で言った
            Box ov;
            if (!Intersect(items[i].box, items[j].box, ov)) continue;
            if (ov.MinSide() <= kThin) continue;   // ⑤で報告済み
            const float smaller = (std::min)(items[i].box.Volume(), items[j].box.Volume());
            if (smaller <= 1e-6f) continue;
            const float ratio = ov.Volume() / smaller;
            if (ratio < 0.30f) continue;           // 3 割未満は「隣接」の範囲
            std::snprintf(buf, sizeof(buf),
                "%s が %s に体積比 %.0f%% めり込んでいる。どちらかをずらすか片方を消すこと",
                NameOf(reg, items[j].e).c_str(), NameOf(reg, items[i].e).c_str(), ratio * 100.0f);
            add("OVERLAP", ratio > 0.80f ? 2 : 1, items[j].e, buf, items[i].e);
        }
    }

    // ---- ⑦ 浮き / 地面へのめり込み（三角形精密レイキャストで真下を見る）----
    //  ここだけレイを飛ばすので、対象は「置き物っぽいもの」に絞る（床/壁そのものは対象外）。
    for (const auto& it : items)
    {
        const Box& b = it.box;
        if (!IsProp(b)) continue;                     // 床・巨大な壁・足場は対象外（IsProp の理由参照）
        if (dupEntities.count(it.e)) continue;        // 二重配置は④で言った。相方を地面と誤認する

        // ★真下を 1 回撃って「支え」と「地面」を同時に取る。
        //   supportY = 真下で最初に当たった面（机・箱・足場を含む＝snap_to_ground と同じ規則）
        //   groundY  = 真下で最初に当たった**地面らしい**面（床・地形・大きな壁だけ）
        //   この 2 つを分けるのが肝。以前は groundY しか無く、しかも
        //   「1 本も当たらなかった」を 0.0f（ワールド原点の高さ）と区別していなかったので:
        //     ・机の上にぴったり置いた箱が、机を無視して床までの距離で「1.00m 浮いている」
        //     ・真下に何も無い物が「地面から 49.75m 浮いている」（その地面は存在しない）
        //   と報告され、しかも fix:"safe" が同じ規則で**机を貫通させて床へ落とし**、
        //   存在しない地面へ 50m テレポートさせていた。
        //   指摘文自身が案内している dx12_snap_to_ground は机の天面に正しく乗せるので、
        //   道具と自動修正が真逆のことをしていたことになる。
        //   浮きは「支えがあるか」の話なので supportY で見る。
        //   埋まりは従来どおり床に対してだけ言う（IsGroundLike のコメント参照）。
        const XMFLOAT3 o{ (b.mn.x + b.mx.x) * 0.5f, b.mx.y + 0.05f, (b.mn.z + b.mx.z) * 0.5f };
        const XMFLOAT3 d{ 0.0f, -1.0f, 0.0f };
        ScenePickOptions popt;
        popt.includeNonMesh  = false;
        popt.trianglePrecise = true;
        popt.maxCandidates   = 128;
        float groundY = 0.0f, supportY = 0.0f;
        bool  hasGround = false, hasSupport = false;
        for (const ScenePickHit& h : RaycastSceneRay(reg, &GetDrawItems(), o, d, 0.0f, popt))
        {
            if (h.entity == it.e || McpIsDescendantOf(reg, h.entity, it.e)
                || McpIsDescendantOf(reg, it.e, h.entity)) continue;
            if (!hasSupport) { supportY = h.worldPos.y; hasSupport = true; }
            if (IsGroundLike(reg, h.entity)) { groundY = h.worldPos.y; hasGround = true; break; }
        }

        // 真下に何も無いなら、埋まりも浮きも判定材料が無い。黙る（0.0f を地面と称さない）。
        if (!hasSupport) continue;

        const float gap = hasGround ? (b.mn.y - groundY) : 1.0f;   // 床が無ければ埋まりは語れない
        if (gap < 0.0f)
        {
            // 埋まりは**割合**で見る。壁の根元が床へ 10cm 刺さっているのは隙間を出さない
            // ための正しい作り方で、指摘すると害の方が大きい。「4 分の 1 以上沈んでいる」
            // か「50cm 以上沈んでいる」を実害の線にする。
            const float depth = -gap;
            if (depth > (std::max)(0.05f, (std::min)(0.5f, b.SizeY() * 0.25f)))
            {
                std::snprintf(buf, sizeof(buf),
                    "%s が地面へ %.2fm 埋まっている（高さ %.2fm の %.0f%%）。dx12_snap_to_ground で接地させること",
                    NameOf(reg, it.e).c_str(), depth, b.SizeY(),
                    b.SizeY() > 0.0f ? depth / b.SizeY() * 100.0f : 0.0f);
                add("BURIED", 2, it.e, buf);
            }
        }
        else
        {
            // 浮きは**支えとの隙間**で見る。机の天面に乗っているものは支えとの隙間が 0 なので
            // 浮いていない（床までの高さは関係ない）。
            const float lift = b.mn.y - supportY;
            if (lift > (std::max)(0.10f, b.SizeY() * 0.5f))
            {
                std::snprintf(buf, sizeof(buf),
                    "%s が真下の面から %.2fm 浮いている。dx12_snap_to_ground で接地させること",
                    NameOf(reg, it.e).c_str(), lift);
                add("FLOATING", 1, it.e, buf);
            }
        }
    }

    // ---- ⑧ 自動修正 ----
    //  安全な修正だけ既定で許す。「安全」= 元に戻せる or 見た目が壊れない、の 2 条件。
    if (fixMode > 0)
    {
        for (LayoutIssue& is : rep.issues)
        {
            if (!reg.valid(is.entity) || !reg.all_of<Transform>(is.entity)) continue;
            auto& t = reg.get<Transform>(is.entity);

            if (is.kind == "BURIED" || is.kind == "FLOATING")
            {
                // snap_to_ground と同じ計算をここでやる（ツールを跨がず 1 回で直すため）
                Box b; bool hm = false;
                if (!WorldAabbDeep(reg, is.entity, b, hm) || !hm) continue;
                const XMFLOAT3 o{ (b.mn.x + b.mx.x) * 0.5f, b.mx.y + 0.05f, (b.mn.z + b.mx.z) * 0.5f };
                const XMFLOAT3 d{ 0.0f, -1.0f, 0.0f };
                ScenePickOptions popt;
                popt.includeNonMesh = false; popt.trianglePrecise = true; popt.maxCandidates = 128;
                // ★着地先は「真下で最初に当たった面」＝dx12_snap_to_ground と同じ規則。
                //   以前は IsGroundLike の面しか着地先にしなかったので、机の上の箱を
                //   **机を貫通させて床へ落とし**、真下に何も無い物は groundY=0.0f の
                //   既定値のままワールド原点の高さへテレポートさせていた。
                //   指摘文が「dx12_snap_to_ground で接地させること」と案内している以上、
                //   自動修正が同じ結果にならないのは単純に誤り。
                float landY = 0.0f;
                bool  hasLand = false;
                for (const ScenePickHit& h : RaycastSceneRay(reg, &GetDrawItems(), o, d, 0.0f, popt))
                {
                    if (h.entity == is.entity || McpIsDescendantOf(reg, h.entity, is.entity)
                        || McpIsDescendantOf(reg, is.entity, h.entity)) continue;
                    landY = h.worldPos.y; hasLand = true; break;
                }
                // 真下に面が無いなら動かさない。どこが地面か分からないまま動かすのは
                // 「安全な修正」ではない（元の位置の方がまだ作者の意図に近い）。
                if (!hasLand) continue;
                t.position.y += (landY - b.mn.y);
                is.fixed = true;
                ++rep.fixed;
            }
            else if (is.kind == "Z_FIGHT")
            {
                // 重なっている軸へ 5mm 逃がす。どちらへ逃がすかは中心の位置関係で決める
                //（上に乗っている物は上へ、手前の壁は手前へ＝見た目の前後が入れ替わらない）。
                if (!reg.valid(is.other)) continue;
                Box a, o;
                bool ha = false, ho = false;
                if (!WorldAabbDeep(reg, is.entity, a, ha) || !ha) continue;
                if (!WorldAabbDeep(reg, is.other,  o, ho) || !ho) continue;
                Box ov;
                if (!Intersect(a, o, ov)) continue;
                const float ex = ov.SizeX(), ey = ov.SizeY(), ez = ov.SizeZ();
                const float bias = 0.005f;
                if (ey <= ex && ey <= ez)
                {
                    const float ca = (a.mn.y + a.mx.y) * 0.5f, co = (o.mn.y + o.mx.y) * 0.5f;
                    t.position.y += (ca >= co) ? bias : -bias;
                }
                else if (ex <= ez)
                {
                    const float ca = (a.mn.x + a.mx.x) * 0.5f, co = (o.mn.x + o.mx.x) * 0.5f;
                    t.position.x += (ca >= co) ? bias : -bias;
                }
                else
                {
                    const float ca = (a.mn.z + a.mx.z) * 0.5f, co = (o.mn.z + o.mx.z) * 0.5f;
                    t.position.z += (ca >= co) ? bias : -bias;
                }
                is.fixed = true;
                ++rep.fixed;
            }
            else if (is.kind == "COLLIDER_WITHOUT_BODY")
            {
                // 静的な当たりとして登録する。これが本来の意図であることがほぼ確実
                //（動かしたいなら motionType を後から変えればいい）。
                RigidBody rb;
                rb.motionType = MotionType::Static;
                rb.mass       = 0.0f;
                reg.emplace_or_replace<RigidBody>(is.entity, rb);
                is.fixed = true;
                ++rep.fixed;
            }
        }
    }

    for (const LayoutIssue& is : rep.issues)
    {
        if (is.fixed) continue;
        if (is.level >= 2) ++rep.errors; else ++rep.warnings;
    }

    // 何を検出したかはログにも残す。返り値は次の 1 手で流れて消えるが、ログは残る＝
    // 共同開発者が「AI が何を見て何を直したか」を後から追える（先頭 30 本で足切り）。
    if (!rep.issues.empty())
    {
        int n = 0;
        for (const LayoutIssue& is : rep.issues)
        {
            if (++n > 30) { Logger::Info("  配置検査: ... 他 {} 件", rep.issues.size() - 30); break; }
            Logger::Info("  配置検査 [{}] {}{}", is.kind, is.text, is.fixed ? " (修正済み)" : "");
        }
    }
    return rep;
}

// play / save_scene の返り値へ載せる要約。ここが「AI に必ず読ませる」仕掛けの本体。
nlohmann::json Application::McpLayoutSummary()
{
    LayoutReport rep = RunLayoutValidation(0, 0.001f);
    nlohmann::json top = nlohmann::json::array();
    // error を先に、次に warning。多くても 5 本まで（全部欲しければ validate_layout を撃つ）。
    for (int wantLevel = 2; wantLevel >= 1; --wantLevel)
        for (const LayoutIssue& is : rep.issues)
        {
            if (is.level != wantLevel || top.size() >= 5) continue;
            top.push_back(is.kind + std::string(": ") + is.text);
        }
    nlohmann::json j{
        {"checked",  rep.checked},
        {"errors",   rep.errors},
        {"warnings", rep.warnings},
    };
    if (!top.empty()) j["top"] = std::move(top);
    if (rep.errors > 0)
        j["next"] = "dx12_validate_layout(fix:\"safe\") で自動修正できるものを直してから続けること";
    return j;
}

void Application::RegisterMcpValidateMethods()
{
    McpDefine("validate_layout", "fix:string,tolerance:number", DX12E_MCP_HANDLER
        {
            // Editor 限定。Play 中は物理が物を動かしているので、そこで測った「浮き」は嘘になる。
            if (m_engineMode == EngineMode::Playing)
                throw McpError(McpErr::ModeConflict,
                    "cannot validate layout while Playing; call dx12_stop first",
                    "先に dx12_stop で Editor へ戻してくれ（Play 中は物理が動かした後の位置を測ってしまう）");

            const std::string fixStr = params.value("fix", std::string("none"));
            int fixMode = 0;
            if (fixStr == "none")      fixMode = 0;
            else if (fixStr == "safe") fixMode = 1;
            else if (fixStr == "all")  fixMode = 2;
            else throw McpError(McpErr::InvalidParam, "unknown fix mode: " + fixStr,
                                "有効値のどれかを指定してくれ", {"none", "safe", "all"});

            const float tol = params.value("tolerance", 0.001f);
            LayoutReport rep = RunLayoutValidation(fixMode, tol);

            nlohmann::json issues = nlohmann::json::array();
            auto& reg = m_scene->GetRegistry();
            for (const LayoutIssue& is : rep.issues)
            {
                nlohmann::json j{
                    {"kind",  is.kind},
                    {"level", is.level >= 2 ? "error" : "warning"},
                    {"text",  is.text},
                    {"fixed", is.fixed},
                };
                if (reg.valid(is.entity))
                {
                    j["entityId"] = static_cast<u32>(is.entity);
                    j["name"]     = NameOf(reg, is.entity);
                }
                if (is.other != entt::null && reg.valid(is.other))
                {
                    j["otherEntityId"] = static_cast<u32>(is.other);
                    j["otherName"]     = NameOf(reg, is.other);
                }
                issues.push_back(std::move(j));
            }

            resp["ok"] = true;
            resp["result"] = {
                {"pass",     rep.errors == 0},
                {"checked",  rep.checked},
                {"errors",   rep.errors},
                {"warnings", rep.warnings},
                {"fixed",    rep.fixed},
                {"issues",   std::move(issues)},
                {"sceneGeneration", m_sceneGeneration},
            };
            if (fixMode == 0 && rep.errors > 0)
                resp["result"]["next"] =
                    "fix:\"safe\" で BURIED/FLOATING/Z_FIGHT/COLLIDER_WITHOUT_BODY は自動で直せる";
            Logger::Info("MCP validate_layout: {} 件検査 / エラー {} / 注意 {} / 修正 {}",
                         rep.checked, rep.errors, rep.warnings, rep.fixed);

        });
}

} // namespace dx12e
