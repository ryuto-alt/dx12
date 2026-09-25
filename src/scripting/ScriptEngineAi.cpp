// ============================================================================
// ScriptEngine: ナビ（nav）/ 群衆 / ゲーム AI（Brain）の Lua API
// ============================================================================
// ScriptEngine.cpp が 5000 行を超えたので、AI まわりの束縛だけをこの TU に分けた。
// ★sol2 の設定マクロ（SOL_ALL_SAFETIES_ON）は ScriptEngine.cpp と必ず同じにすること
//   （TU ごとに違うと ODR 違反で usertype のメタテーブルが食い違う）。
//
// nav の関数は `nav.f(...)` でも `nav:f(...)` でも呼べる（先頭が nav テーブル自身なら読み飛ばす）。
// ★以前は `nav:sample(p)` と書くと第 1 引数に nav テーブルが入って型エラーになり、
//   ドキュメント（':' で呼ぶ）の通りに書くと動かなかった（MikuChase.lua の冒頭の注意書き）。
// ============================================================================
#include "scripting/ScriptEngine.h"

#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4244 4267 4996)
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#pragma warning(pop)

#include "scene/Scene.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "nav/NavCorridor.h"
#include "nav/NavCrowd.h"
#include "ai/AiSystem.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

namespace dx12e
{

namespace
{
using DirectX::XMFLOAT3;

// Lua の値を XMFLOAT3 へ。Vec3 usertype / {x=,y=,z=} / {1,2,3} を受ける。
bool LuaToVec3(const sol::object& v, XMFLOAT3& dst)
{
    if (v.is<XMFLOAT3>()) { dst = v.as<XMFLOAT3>(); return true; }
    if (v.get_type() != sol::type::table) return false;
    sol::table t = v.as<sol::table>();
    sol::optional<float> nx = t["x"], ny = t["y"], nz = t["z"];
    if (nx && ny && nz) { dst = { *nx, *ny, *nz }; return true; }
    sol::optional<float> i1 = t[1], i2 = t[2], i3 = t[3];
    if (i1 && i2 && i3) { dst = { *i1, *i2, *i3 }; return true; }
    return false;
}

// nav.f / nav:f の両対応。先頭が nav テーブル自身（ready 関数を持つテーブル）なら 1 つ読み飛ばす。
struct NavArgs
{
    sol::variadic_args& va;
    int base = 0;
    explicit NavArgs(sol::variadic_args& v) : va(v)
    {
        if (va.size() > 0 && va.get_type(0) == sol::type::table)
        {
            sol::table t = va[0];
            sol::object r = t.raw_get<sol::object>("ready");
            if (r.get_type() == sol::type::function) base = 1;
        }
    }
    int Count() const { return static_cast<int>(va.size()) - base; }
    sol::object At(int i) const
    {
        // ★範囲外は「状態つきの nil」を返す。素の sol::lua_nil から作った object は lua_State を
        //   持たず、get_type() でヌル参照して落ちる（省略可能な引数を省いた時に実際に落ちた）。
        if (i < 0 || i >= Count()) return sol::make_object(va.lua_state(), sol::lua_nil);
        return va[static_cast<size_t>(base + i)];
    }
    XMFLOAT3 Vec(int i, const char* fn, const char* what) const
    {
        XMFLOAT3 v{};
        if (!LuaToVec3(At(i), v))
            throw sol::error(std::string("nav.") + fn + ": " + what +
                             " には Vec3（または {x=,y=,z=}）を渡すこと");
        return v;
    }
    std::optional<float> Num(int i) const
    {
        sol::object o = At(i);
        if (o.get_type() == sol::type::number) return o.as<float>();
        return std::nullopt;
    }
    nav::NavPolyRef Ref(int i) const
    {
        sol::object o = At(i);
        if (o.get_type() != sol::type::number) return nav::kNullPolyRef;
        const lua_Integer v = o.as<lua_Integer>();
        return v > 0 ? static_cast<nav::NavPolyRef>(v) : nav::kNullPolyRef;
    }
};

lua_Integer RefToLua(nav::NavPolyRef r) { return static_cast<lua_Integer>(r); }

// エンティティ引数: Entity / 数値 id / 名前 / self テーブル（self.entity）のどれでも受ける
entt::entity ResolveEntityArg(Scene* scene, const sol::object& o)
{
    if (!scene) return entt::null;
    auto& reg = scene->GetRegistry();
    auto valid = [&](entt::entity e) { return reg.valid(e) ? e : entt::null; };
    if (o.is<Entity>()) return valid(o.as<Entity>().GetHandle());
    if (o.get_type() == sol::type::number)
        return valid(static_cast<entt::entity>(o.as<std::uint32_t>()));
    auto byName = [&](const std::string& n) -> entt::entity
    {
        for (auto [e, nt] : reg.view<NameTag>().each())
            if (nt.name == n) return e;
        return entt::null;
    };
    if (o.get_type() == sol::type::string) return byName(o.as<std::string>());
    if (o.get_type() == sol::type::table)
    {
        sol::table t = o;
        sol::object id = t["entity"];
        if (id.get_type() == sol::type::number) return valid(static_cast<entt::entity>(id.as<std::uint32_t>()));
        sol::object nm = t["name"];
        if (nm.get_type() == sol::type::string) return byName(nm.as<std::string>());
    }
    return entt::null;
}

// { radius=, maxSpeed=, ... } を群衆エージェントの設定へ（書いていないキーは base のまま）
void ReadAgentParams(const sol::object& o, nav::NavAgentParams& p, ai::AgentOptions& opt)
{
    if (o.get_type() != sol::type::table) return;
    sol::table t = o;
    auto num = [&](const char* k, f32& dst) { sol::optional<f32> v = t[k]; if (v) dst = *v; };
    auto flag = [&](const char* k, bool& dst) { sol::object v = t[k]; if (v.get_type() == sol::type::boolean) dst = v.as<bool>(); };
    num("radius", p.radius);
    num("height", p.height);
    num("maxSpeed", p.maxSpeed);
    num("maxAccel", p.maxAccel);
    num("separation", p.separationWeight);
    num("queryRange", p.collisionQueryRange);
    num("optimizeRange", p.pathOptimizationRange);
    num("wallMargin", p.wallMargin);
    num("slowDownRadius", p.slowDownRadius);
    {
        sol::object v = t["avoidance"];
        if (v.get_type() == sol::type::boolean) p.obstacleAvoidance = v.as<bool>();
        else if (v.get_type() == sol::type::number) { p.obstacleAvoidance = true; p.avoidanceQuality = v.as<int>(); }
    }
    flag("anticipateTurns", p.anticipateTurns);
    flag("optimize", p.optimizeVisibility);
    {
        sol::object v = t["separation"];
        if (v.get_type() == sol::type::boolean) { p.separation = v.as<bool>(); }
    }
    flag("faceMovement", opt.faceMovement);
    num("turnRate", opt.turnRate);
    num("yOffset", opt.yOffset);
}

// Lua に渡す通路。ナビメッシュは焼き直されうるので参照は持たず、呼ぶたびにシーンから引く。
struct LuaNavCorridor
{
    nav::NavCorridor c;
};
} // namespace

// ---------------------------------------------------------------------------
// nav グローバル（ナビメッシュ）
//   nav.ready()                          -> bool
//   nav.sample(pos, radius?)             -> Vec3|nil      位置を歩行面へ落とす（そのポリゴン上の最近点）
//   nav.findPath(from, to, radius?)      -> {Vec3,...}    A* + ファネルの折れ線（空なら経路なし）
//   nav.raycast(from, to)                -> bool, Vec3    ★旧 API。true は「壁に当たった」だけ
//   nav.moveAlong(from, to)              -> Vec3          壁で滑らせた移動先（辺から少し内側に着地）
//   ---- ここから追加（polyRef / 4 状態のレイ / 通路）----
//   nav.locate(pos, radius?)             -> ref, Vec3 | nil
//   nav.isValidRef(ref)                  -> bool
//   nav.raycastEx(from, to, ref?)        -> { status, hit, t, point, normal, ref }
//   nav.moveAlongEx(from, to, ref?)      -> Vec3, ref
//   nav.findPathEx(from, to, radius?)    -> { status, points, length, reached }
//   nav.corridor(pos, radius?)           -> NavCorridor|nil
// ★毎フレーム findPath を全員ぶん呼ばないこと。追いかけるなら通路（nav.corridor）か群衆（nav.agent*）を使う。
// ---------------------------------------------------------------------------
void ScriptEngine::RegisterNavBindings()
{
    using namespace DirectX;
    auto& lua = *m_lua;
    sol::table navT = lua.create_named_table("nav");

    auto ext = [this](std::optional<float> radius, float out[3])
    {
        const auto& cfg = m_scene->GetNavConfig();
        const float r = (radius && *radius > 0.0f) ? *radius
                                                   : (std::max)(2.0f, cfg.cellSize * 8.0f);
        out[0] = r;
        out[1] = (std::max)(cfg.agentHeight * 2.0f, 4.0f);
        out[2] = r;
    };
    auto hasNav = [this]() { return m_scene && m_scene->HasNavMesh(); };

    navT.set_function("ready", [hasNav](sol::variadic_args) -> bool { return hasNav(); });

    navT.set_function("sample", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va) -> sol::object
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        if (!hasNav()) return sol::lua_nil;
        const XMFLOAT3 p = a.Vec(0, "sample", "pos");
        float e[3]; ext(a.Num(1), e);
        const float pos[3] = { p.x, p.y, p.z };
        float out[3]{};
        const int poly = m_scene->GetNavMesh().FindNearestPoly(pos, e, out);
        if (poly < 0) return sol::lua_nil;
        return sol::make_object(sv, XMFLOAT3{ out[0], out[1], out[2] });
    });

    navT.set_function("findPath", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va) -> sol::table
    {
        sol::state_view sv(ts);
        sol::table t = sv.create_table();
        NavArgs a(va);
        if (!hasNav()) return t;
        const XMFLOAT3 pa = a.Vec(0, "findPath", "from");
        const XMFLOAT3 pb = a.Vec(1, "findPath", "to");
        float e[3]; ext(a.Num(2), e);
        const float from[3] = { pa.x, pa.y, pa.z };
        const float to[3]   = { pb.x, pb.y, pb.z };
        std::vector<float> path;
        const int n = m_scene->GetNavMesh().FindPath(from, to, e, path);
        for (int i = 0; i < n; ++i)
            t[i + 1] = XMFLOAT3{ path[static_cast<size_t>(i) * 3 + 0],
                                 path[static_cast<size_t>(i) * 3 + 1],
                                 path[static_cast<size_t>(i) * 3 + 2] };
        return t;
    });

    // 旧 API（互換）。★false は「通れた」とは限らない（始点が外でも false）。新規は raycastEx を使う
    navT.set_function("raycast", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va)
        -> std::tuple<bool, sol::object>
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        if (!hasNav()) return { false, sol::lua_nil };
        const XMFLOAT3 pa = a.Vec(0, "raycast", "from");
        const XMFLOAT3 pb = a.Vec(1, "raycast", "to");
        float e[3]; ext(std::nullopt, e);
        const float from[3] = { pa.x, pa.y, pa.z };
        const float to[3]   = { pb.x, pb.y, pb.z };
        float start[3]{};
        const int poly = m_scene->GetNavMesh().FindNearestPoly(from, e, start);
        if (poly < 0) return { false, sol::lua_nil };
        float t = 1.0f, n[3]{}, hit[3]{};
        const bool blocked = m_scene->GetNavMesh().Raycast(start, to, poly, t, n, hit);
        return { blocked, sol::make_object(sv, XMFLOAT3{ hit[0], hit[1], hit[2] }) };
    });

    navT.set_function("moveAlong", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va) -> sol::object
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        const XMFLOAT3 pa = a.Vec(0, "moveAlong", "from");
        const XMFLOAT3 pb = a.Vec(1, "moveAlong", "to");
        if (!hasNav()) return sol::make_object(sv, pa);
        float e[3]; ext(std::nullopt, e);
        const float from[3] = { pa.x, pa.y, pa.z };
        const float to[3]   = { pb.x, pb.y, pb.z };
        float start[3]{};
        const int poly = m_scene->GetNavMesh().FindNearestPoly(from, e, start);
        if (poly < 0) return sol::make_object(sv, pa);
        float out[3]{}; int outPoly = poly;
        m_scene->GetNavMesh().MoveAlongSurface(start, to, poly, out, outPoly);
        return sol::make_object(sv, XMFLOAT3{ out[0], out[1], out[2] });
    });

    // ---- polyRef ----
    navT.set_function("locate", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va)
        -> std::tuple<sol::object, sol::object>
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        if (!hasNav()) return { sol::lua_nil, sol::lua_nil };
        const XMFLOAT3 p = a.Vec(0, "locate", "pos");
        float e[3]; ext(a.Num(1), e);
        const float pos[3] = { p.x, p.y, p.z };
        float out[3]{};
        const auto& nm = m_scene->GetNavMesh();
        const int poly = nm.FindNearestPoly(pos, e, out);
        if (poly < 0) return { sol::lua_nil, sol::lua_nil };
        return { sol::make_object(sv, RefToLua(nm.MakeRef(poly))),
                 sol::make_object(sv, XMFLOAT3{ out[0], out[1], out[2] }) };
    });

    navT.set_function("isValidRef", [this, hasNav](sol::variadic_args va) -> bool
    {
        NavArgs a(va);
        if (!hasNav()) return false;
        return m_scene->GetNavMesh().DecodeRef(a.Ref(0)) >= 0;
    });

    // ---- 4 状態のレイ ----
    navT.set_function("raycastEx", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va) -> sol::table
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        sol::table r = sv.create_table();
        const XMFLOAT3 pa = a.Vec(0, "raycastEx", "from");
        const XMFLOAT3 pb = a.Vec(1, "raycastEx", "to");
        if (!hasNav())
        {
            r["status"] = "startOffMesh"; r["hit"] = false; r["t"] = 0.0f;
            r["point"] = pa; r["normal"] = XMFLOAT3{ 0, 0, 0 }; r["ref"] = 0;
            return r;
        }
        const auto& nm = m_scene->GetNavMesh();
        const float from[3] = { pa.x, pa.y, pa.z };
        const float to[3]   = { pb.x, pb.y, pb.z };
        // ref が渡されて有効ならそのポリゴンから撃つ（位置を探し直さない＝縁で取り違えない）
        int poly = nm.DecodeRef(a.Ref(2));
        float start[3] = { from[0], from[1], from[2] };
        if (poly < 0)
        {
            float e[3]; ext(std::nullopt, e);
            poly = nm.FindNearestPoly(from, e, start);
        }
        const nav::NavRaycastResult res = nm.RaycastEx(start, to, poly);
        r["status"] = nav::NavRayStatusName(res.status);
        r["hit"]    = res.status == nav::NavRayStatus::Hit;
        r["t"]      = res.t;
        r["point"]  = XMFLOAT3{ res.hitPos[0], res.hitPos[1], res.hitPos[2] };
        r["normal"] = XMFLOAT3{ res.hitNormal[0], res.hitNormal[1], res.hitNormal[2] };
        r["ref"]    = RefToLua(nm.MakeRef(res.lastPoly));
        return r;
    });

    navT.set_function("moveAlongEx", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va)
        -> std::tuple<sol::object, sol::object>
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        const XMFLOAT3 pa = a.Vec(0, "moveAlongEx", "from");
        const XMFLOAT3 pb = a.Vec(1, "moveAlongEx", "to");
        if (!hasNav()) return { sol::make_object(sv, pa), sol::make_object(sv, lua_Integer{ 0 }) };
        const auto& nm = m_scene->GetNavMesh();
        const float from[3] = { pa.x, pa.y, pa.z };
        const float to[3]   = { pb.x, pb.y, pb.z };
        int poly = nm.DecodeRef(a.Ref(2));
        float start[3] = { from[0], from[1], from[2] };
        if (poly < 0)
        {
            float e[3]; ext(std::nullopt, e);
            poly = nm.FindNearestPoly(from, e, start);
            if (poly < 0) return { sol::make_object(sv, pa), sol::make_object(sv, lua_Integer{ 0 }) };
        }
        float out[3]{}; int outPoly = poly;
        nm.MoveAlongSurface(start, to, poly, out, outPoly);
        return { sol::make_object(sv, XMFLOAT3{ out[0], out[1], out[2] }),
                 sol::make_object(sv, RefToLua(nm.MakeRef(outPoly))) };
    });

    navT.set_function("findPathEx", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va) -> sol::table
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        sol::table r = sv.create_table();
        sol::table pts = sv.create_table();
        r["points"] = pts;
        r["status"] = "failed"; r["length"] = 0.0f; r["reached"] = false;
        const XMFLOAT3 pa = a.Vec(0, "findPathEx", "from");
        const XMFLOAT3 pb = a.Vec(1, "findPathEx", "to");
        if (!hasNav()) return r;
        const auto& nm = m_scene->GetNavMesh();
        float e[3]; ext(a.Num(2), e);
        const float from[3] = { pa.x, pa.y, pa.z };
        const float to[3]   = { pb.x, pb.y, pb.z };
        float sPos[3], ePos[3];
        const int sPoly = nm.FindNearestPoly(from, e, sPos);
        const int ePoly = nm.FindNearestPoly(to, e, ePos);
        if (sPoly < 0 || ePoly < 0) return r;
        std::vector<int> polys;
        const nav::NavPathStatus st = nm.FindPolyPath(sPoly, ePoly, sPos, ePos, polys);
        if (st == nav::NavPathStatus::Failed) return r;
        float goal[3] = { ePos[0], ePos[1], ePos[2] };
        if (st == nav::NavPathStatus::Partial) nm.ClosestPointOnPoly(polys.back(), ePos, goal);
        std::vector<float> path;
        const int n = nm.FindStraightPath(sPos, goal, polys.data(), static_cast<int>(polys.size()),
                                          path, nullptr, 256);
        float len = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            pts[i + 1] = XMFLOAT3{ path[static_cast<size_t>(i) * 3 + 0], path[static_cast<size_t>(i) * 3 + 1],
                                   path[static_cast<size_t>(i) * 3 + 2] };
            if (i > 0)
            {
                const float dx = path[static_cast<size_t>(i) * 3] - path[static_cast<size_t>(i - 1) * 3];
                const float dz = path[static_cast<size_t>(i) * 3 + 2] - path[static_cast<size_t>(i - 1) * 3 + 2];
                len += std::sqrt(dx * dx + dz * dz);
            }
        }
        r["status"]  = nav::NavPathStatusName(st);
        r["length"]  = len;
        r["reached"] = (st == nav::NavPathStatus::Complete);
        return r;
    });

    // ---- 通路（パス・コリドー）----
    auto corridorNav = [this]() -> const nav::NavMesh*
    {
        return (m_scene && m_scene->HasNavMesh()) ? &m_scene->GetNavMesh() : nullptr;
    };
    auto toXm = [](const float* p) { return XMFLOAT3{ p[0], p[1], p[2] }; };

    lua.new_usertype<LuaNavCorridor>("NavCorridor",
        sol::no_constructor,
        // 目標までの通路を A* で張る → "complete" / "partial"（一番近い所まで）/ "failed"
        "setTarget", [corridorNav, ext](LuaNavCorridor& self, sol::object target, sol::optional<float> radius)
            -> std::string
        {
            const nav::NavMesh* nm = corridorNav();
            if (!nm) return "failed";
            XMFLOAT3 t{};
            if (!LuaToVec3(target, t)) throw sol::error("NavCorridor:setTarget: target には Vec3 を渡すこと");
            float e[3]; ext(radius ? std::optional<float>(*radius) : std::nullopt, e);
            const float tp[3] = { t.x, t.y, t.z };
            return nav::NavPathStatusName(self.c.Plan(*nm, tp, e));
        },
        // 目標を少しだけ動かす（A* をやり直さずに通路の末尾を伸ばす）。壁で止まったら false
        "moveTarget", [corridorNav](LuaNavCorridor& self, sol::object target) -> bool
        {
            const nav::NavMesh* nm = corridorNav();
            XMFLOAT3 t{};
            if (!nm || !LuaToVec3(target, t)) return false;
            const float tp[3] = { t.x, t.y, t.z };
            return self.c.MoveTargetPosition(*nm, tp);
        },
        // 現在地を pos へ滑らせて動かす（通路の中で。壁は抜けない）。戻り値は実際の位置
        "move", [corridorNav, toXm](LuaNavCorridor& self, sol::object pos) -> XMFLOAT3
        {
            const nav::NavMesh* nm = corridorNav();
            XMFLOAT3 p{};
            if (nm && LuaToVec3(pos, p))
            {
                const float np[3] = { p.x, p.y, p.z };
                self.c.MovePosition(*nm, np);
            }
            return toXm(self.c.Pos());
        },
        // 通路に沿って dist メートル進める（角を順にたどる）。戻り値は新しい位置
        "advance", [corridorNav, toXm](LuaNavCorridor& self, float dist, sol::optional<float> margin) -> XMFLOAT3
        {
            const nav::NavMesh* nm = corridorNav();
            if (!nm || dist <= 0.0f) return toXm(self.c.Pos());
            std::vector<float> corners;
            float left = dist;
            for (int it = 0; it < 8 && left > 1e-4f; ++it)
            {
                if (self.c.FindCorners(*nm, corners, nullptr, 2, margin.value_or(0.0f)) <= 0) break;
                const float* p = self.c.Pos();
                const float dx = corners[0] - p[0], dz = corners[2] - p[2];
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d < 1e-4f) break;
                const float st = (std::min)(left, d);
                const float np[3] = { p[0] + dx / d * st, p[1], p[2] + dz / d * st };
                const float before[3] = { p[0], p[1], p[2] };
                self.c.MovePosition(*nm, np);
                const float* q = self.c.Pos();
                const float moved = std::sqrt((q[0] - before[0]) * (q[0] - before[0]) +
                                              (q[2] - before[2]) * (q[2] - before[2]));
                if (moved < st * 0.5f) break;   // 壁に押し返された
                left -= st;
            }
            return toXm(self.c.Pos());
        },
        // 現在地から先の角（最大 n 個、最後は目標）。margin > 0 で壁の角から離して返す
        "corners", [corridorNav](LuaNavCorridor& self, sol::this_state ts, sol::optional<int> n,
                                 sol::optional<float> margin) -> sol::table
        {
            sol::state_view sv(ts);
            sol::table t = sv.create_table();
            const nav::NavMesh* nm = corridorNav();
            if (!nm) return t;
            std::vector<float> corners;
            const int k = self.c.FindCorners(*nm, corners, nullptr, (std::max)(1, n.value_or(4)),
                                             margin.value_or(0.0f));
            for (int i = 0; i < k; ++i)
                t[i + 1] = XMFLOAT3{ corners[static_cast<size_t>(i) * 3], corners[static_cast<size_t>(i) * 3 + 1],
                                     corners[static_cast<size_t>(i) * 3 + 2] };
            return t;
        },
        // 見通せる所まで通路を近道する（障害物の多い広場で遠回りを直す）
        "optimize", [corridorNav](LuaNavCorridor& self, sol::optional<float> range)
        {
            const nav::NavMesh* nm = corridorNav();
            if (!nm) return;
            std::vector<float> corners;
            if (self.c.FindCorners(*nm, corners, nullptr, 2) >= 1)
                self.c.OptimizePathVisibility(*nm, corners.data(), range.value_or(8.0f));
        },
        "reset", [corridorNav, ext](LuaNavCorridor& self, sol::object pos) -> bool
        {
            const nav::NavMesh* nm = corridorNav();
            XMFLOAT3 p{};
            if (!nm || !LuaToVec3(pos, p)) return false;
            float e[3]; ext(std::nullopt, e);
            const float pp[3] = { p.x, p.y, p.z };
            float out[3];
            const int poly = nm->FindNearestPoly(pp, e, out);
            if (poly < 0) return false;
            self.c.Reset(poly, out, nm->Generation());
            return true;
        },
        "position", [toXm](LuaNavCorridor& self) -> XMFLOAT3 { return toXm(self.c.Pos()); },
        "target", [toXm](LuaNavCorridor& self) -> XMFLOAT3 { return toXm(self.c.Target()); },
        "ref", [corridorNav](LuaNavCorridor& self) -> lua_Integer
        {
            const nav::NavMesh* nm = corridorNav();
            return nm ? RefToLua(nm->MakeRef(self.c.FirstPoly())) : 0;
        },
        "status", [](LuaNavCorridor& self) -> std::string
        {
            if (!self.c.HasTarget()) return "none";
            return nav::NavPathStatusName(self.c.Status());
        },
        "isValid", [corridorNav](LuaNavCorridor& self) -> bool
        {
            const nav::NavMesh* nm = corridorNav();
            return nm && self.c.IsValid(*nm);
        },
        "length", [corridorNav](LuaNavCorridor& self) -> float
        {
            const nav::NavMesh* nm = corridorNav();
            return nm ? self.c.PathLength(*nm) : 0.0f;
        },
        "polyCount", [](LuaNavCorridor& self) -> int { return static_cast<int>(self.c.Path().size()); }
    );

    navT.set_function("corridor", [this, ext, hasNav](sol::this_state ts, sol::variadic_args va) -> sol::object
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        if (!hasNav()) return sol::lua_nil;
        const XMFLOAT3 p = a.Vec(0, "corridor", "pos");
        float e[3]; ext(a.Num(1), e);
        const auto& nm = m_scene->GetNavMesh();
        const float pos[3] = { p.x, p.y, p.z };
        float out[3];
        const int poly = nm.FindNearestPoly(pos, e, out);
        if (poly < 0) return sol::lua_nil;
        LuaNavCorridor c;
        c.c.Reset(poly, out, nm.Generation());
        return sol::make_object(sv, std::move(c));
    });

    // ---- 群衆（エンティティを群衆に入れると、エンジンが毎ステップ動かして Transform へ書く）----
    //   nav.agentAdd(entity, params?) -> bool     params: radius / maxSpeed / maxAccel / separation /
    //                                              wallMargin / avoidance(0..3|false) / faceMovement / turnRate / yOffset
    //   nav.agentMoveTo(entity, pos, speed?) -> bool
    //   nav.agentVelocity(entity, vel) -> bool     速度で動かす（通路を使わない。プレイヤー操作の NPC 等）
    //   nav.agentStop(entity) / nav.agentRemove(entity) / nav.agentSet(entity, params)
    //   nav.agentState(entity) -> table|nil
    auto aiOk = [this]() { return m_aiSystem && m_scene && m_scene->HasNavMesh(); };

    navT.set_function("agentAdd", [this, aiOk](sol::variadic_args va) -> bool
    {
        NavArgs a(va);
        if (!aiOk()) return false;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        if (e == entt::null) throw sol::error("nav.agentAdd: entity が見つからない（Entity / 名前 / self を渡すこと）");
        nav::NavAgentParams p;
        ai::AgentOptions o;
        if (const nav::NavCrowdAgent* cur = m_aiSystem->GetAgent(e)) p = cur->params;
        if (const ai::AgentOptions* co = m_aiSystem->GetOptions(e)) o = *co;
        ReadAgentParams(a.At(1), p, o);
        return m_aiSystem->AddAgent(*m_scene, e, p, o);
    });
    navT.set_function("agentRemove", [this](sol::variadic_args va)
    {
        NavArgs a(va);
        if (!m_aiSystem) return;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        if (e != entt::null) m_aiSystem->RemoveAgent(e);
    });
    navT.set_function("agentMoveTo", [this](sol::variadic_args va) -> bool
    {
        NavArgs a(va);
        if (!m_aiSystem) return false;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        if (e == entt::null) return false;
        const XMFLOAT3 p = a.Vec(1, "agentMoveTo", "pos");
        const float pos[3] = { p.x, p.y, p.z };
        return m_aiSystem->MoveTo(e, pos, a.Num(2).value_or(0.0f));
    });
    navT.set_function("agentVelocity", [this](sol::variadic_args va) -> bool
    {
        NavArgs a(va);
        if (!m_aiSystem) return false;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        if (e == entt::null) return false;
        const XMFLOAT3 v = a.Vec(1, "agentVelocity", "vel");
        const float vel[3] = { v.x, 0.0f, v.z };
        return m_aiSystem->MoveVelocity(e, vel);
    });
    navT.set_function("agentStop", [this](sol::variadic_args va) -> bool
    {
        NavArgs a(va);
        if (!m_aiSystem) return false;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        return e != entt::null && m_aiSystem->Stop(e);
    });
    navT.set_function("agentSet", [this](sol::variadic_args va) -> bool
    {
        NavArgs a(va);
        if (!m_aiSystem) return false;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        const nav::NavCrowdAgent* cur = (e != entt::null) ? m_aiSystem->GetAgent(e) : nullptr;
        const ai::AgentOptions* co = (e != entt::null) ? m_aiSystem->GetOptions(e) : nullptr;
        if (!cur || !co) return false;
        nav::NavAgentParams p = cur->params;
        ai::AgentOptions o = *co;
        ReadAgentParams(a.At(1), p, o);
        return m_aiSystem->SetParams(e, p) && m_aiSystem->SetOptions(e, o);
    });
    navT.set_function("agentState", [this](sol::this_state ts, sol::variadic_args va) -> sol::object
    {
        sol::state_view sv(ts);
        NavArgs a(va);
        if (!m_aiSystem) return sol::lua_nil;
        const entt::entity e = ResolveEntityArg(m_scene, a.At(0));
        const nav::NavCrowdAgent* ag = (e != entt::null) ? m_aiSystem->GetAgent(e) : nullptr;
        if (!ag) return sol::lua_nil;
        sol::table t = sv.create_table();
        t["pos"] = XMFLOAT3{ ag->npos[0], ag->npos[1], ag->npos[2] };
        t["vel"] = XMFLOAT3{ ag->vel[0], ag->vel[1], ag->vel[2] };
        t["desiredVel"] = XMFLOAT3{ ag->dvel[0], ag->dvel[1], ag->dvel[2] };
        f32 av[3]{};
        m_aiSystem->ActualVelocity(e, av);
        t["speed"] = std::sqrt(av[0] * av[0] + av[2] * av[2]);   // 実際に進んだ速さ（アニメの足に使う）
        t["state"] = nav::NavMoveStateName(ag->targetState);
        t["partial"] = ag->partial;
        t["distance"] = m_aiSystem->DistanceToGoal(e);
        t["arrived"] = m_aiSystem->Arrived(e, (std::max)(0.2f, ag->params.radius * 0.5f));
        t["target"] = XMFLOAT3{ ag->corridor.Target()[0], ag->corridor.Target()[1], ag->corridor.Target()[2] };
        t["neighbors"] = static_cast<int>(ag->neis.size());
        sol::table cs = sv.create_table();
        for (size_t i = 0; i + 2 < ag->corners.size(); i += 3)
            cs[static_cast<int>(i / 3) + 1] = XMFLOAT3{ ag->corners[i], ag->corners[i + 1], ag->corners[i + 2] };
        t["corners"] = cs;
        return t;
    });
}

// ===========================================================================
// ゲーム AI（Brain）
// ===========================================================================
namespace
{
// Brain の行動 1 つぶんの Lua 関数
struct AiLuaAction
{
    sol::protected_function enter, update, exit;
};
struct AiLuaStore
{
    std::unordered_map<u32, std::vector<AiLuaAction>> fns;
    std::unordered_map<u32, int> errors;   // エンティティごとのエラー回数（ログの連打を止める）
};

// Lua に渡す Brain のハンドル（中身はエンティティ id だけ。状態は AiSystem が持つ）
struct LuaBrain
{
    u32 entity = 0xffffffffu;
};

u32 EntId(entt::entity e) { return static_cast<u32>(entt::to_integral(e)); }
entt::entity EntOf(u32 id) { return static_cast<entt::entity>(id); }

// 黒板の値 ⇔ Lua の値
sol::object BbToLua(sol::state_view sv, entt::registry& reg, const ai::BbValue& v)
{
    switch (v.index())
    {
    case 1: return sol::make_object(sv, std::get<f64>(v));
    case 2: return sol::make_object(sv, std::get<bool>(v));
    case 3: return sol::make_object(sv, std::get<std::string>(v));
    case 4: { const ai::BbVec3& p = std::get<ai::BbVec3>(v); return sol::make_object(sv, XMFLOAT3{ p.x, p.y, p.z }); }
    case 5:
    {
        const entt::entity e = EntOf(std::get<ai::BbEntity>(v).id);
        if (!reg.valid(e)) return sol::make_object(sv, sol::lua_nil);
        return sol::make_object(sv, Entity(e, &reg));
    }
    default: return sol::make_object(sv, sol::lua_nil);
    }
}

ai::BbValue LuaToBb(const sol::object& o)
{
    switch (o.get_type())
    {
    case sol::type::number:  return ai::BbValue{ o.as<f64>() };
    case sol::type::boolean: return ai::BbValue{ o.as<bool>() };
    case sol::type::string:  return ai::BbValue{ o.as<std::string>() };
    default: break;
    }
    if (o.is<Entity>()) return ai::BbValue{ ai::BbEntity{ EntId(o.as<Entity>().GetHandle()) } };
    XMFLOAT3 v{};
    if (LuaToVec3(o, v)) return ai::BbValue{ ai::BbVec3{ v.x, v.y, v.z } };
    return ai::BbValue{};
}

// { input=, curve=, min=, max=, ... } → 考慮事項
ai::Consideration ReadConsideration(const sol::table& t, const std::string& actionName)
{
    ai::Consideration c;
    c.input = t.get_or<std::string>("input", "");
    if (c.input.empty())
        throw sol::error("brain:action('" + actionName + "'): 考慮事項に input（黒板のキー）が無い");
    c.name = t.get_or<std::string>("name", "");
    c.lo = t.get_or("min", 0.0f);
    c.hi = t.get_or("max", 1.0f);
    c.fallback = t.get_or("default", 0.0f);
    const std::string type = t.get_or<std::string>("curve", "linear");
    ai::CurveType ct;
    if (!ai::ParseCurveType(type, ct))
        throw sol::error("brain:action('" + actionName + "'): 未知のカーブ '" + type +
                         "'（linear / quadratic / logistic / step / inverse / smooth）");
    c.curve = ai::ResponseCurve::Make(ct);
    sol::optional<float> m = t["m"], k = t["k"], b = t["b"], cc = t["c"];
    if (m) c.curve.m = *m;
    if (k) c.curve.k = *k;
    if (b) c.curve.b = *b;
    if (cc) c.curve.c = *cc;
    c.curve.invert = t.get_or("invert", false);
    return c;
}

// b:config{...} / ai.brain(self, {...}) で Brain コンポーネントの設定を書く
void ApplyBrainConfig(Brain& br, const sol::table& t)
{
    for (const auto& kv : t)
    {
        if (!kv.first.is<std::string>()) continue;
        const std::string k = kv.first.as<std::string>();
        const sol::object& v = kv.second;
        auto f = [&](f32& dst) { if (v.get_type() == sol::type::number) dst = v.as<f32>(); };
        auto b = [&](bool& dst) { if (v.get_type() == sol::type::boolean) dst = v.as<bool>(); };
        if      (k == "enabled")       b(br.enabled);
        else if (k == "targets")       { if (v.get_type() == sol::type::string) br.targets = v.as<std::string>(); }
        else if (k == "seed")          { if (v.get_type() == sol::type::number) br.seed = v.as<i32>(); }
        else if (k == "thinkInterval") f(br.thinkInterval);
        else if (k == "hysteresis")    f(br.hysteresis);
        else if (k == "minCommitTime") f(br.minCommitTime);
        else if (k == "sightRange")    f(br.sightRange);
        else if (k == "sightFov")      f(br.sightFov);
        else if (k == "nearSense")     f(br.nearSense);
        else if (k == "eyeHeight")     f(br.eyeHeight);
        else if (k == "targetHeight")  f(br.targetHeight);
        else if (k == "confirmTime")   f(br.confirmTime);
        else if (k == "sightInterval") f(br.sightInterval);
        else if (k == "hearingScale")  f(br.hearingScale);
        else if (k == "occlusion")     f(br.occlusion);
        else if (k == "memoryTime")    f(br.memoryTime);
        else if (k == "useCrowd")      b(br.useCrowd);
        else if (k == "agentRadius")   f(br.agentRadius);
        else if (k == "maxSpeed")      f(br.maxSpeed);
        else if (k == "maxAccel")      f(br.maxAccel);
        else if (k == "separation")    f(br.separation);
        else if (k == "wallMargin")    f(br.wallMargin);
        else if (k == "turnRate")      f(br.turnRate);
        else if (k == "debugDraw")     b(br.debugDraw);
        else Logger::Warn("brain:config: 未知のキー '{}'（describe_components の brain を参照）", k);
    }
}

sol::object BrainConfigValue(sol::state_view sv, const Brain& br, const std::string& k)
{
    auto num = [&](f32 v) { return sol::make_object(sv, v); };
    auto bl = [&](bool v) { return sol::make_object(sv, v); };
    if (k == "enabled") return bl(br.enabled);
    if (k == "targets") return sol::make_object(sv, br.targets);
    if (k == "seed") return sol::make_object(sv, br.seed);
    if (k == "thinkInterval") return num(br.thinkInterval);
    if (k == "hysteresis") return num(br.hysteresis);
    if (k == "minCommitTime") return num(br.minCommitTime);
    if (k == "sightRange") return num(br.sightRange);
    if (k == "sightFov") return num(br.sightFov);
    if (k == "nearSense") return num(br.nearSense);
    if (k == "eyeHeight") return num(br.eyeHeight);
    if (k == "targetHeight") return num(br.targetHeight);
    if (k == "confirmTime") return num(br.confirmTime);
    if (k == "sightInterval") return num(br.sightInterval);
    if (k == "hearingScale") return num(br.hearingScale);
    if (k == "occlusion") return num(br.occlusion);
    if (k == "memoryTime") return num(br.memoryTime);
    if (k == "useCrowd") return bl(br.useCrowd);
    if (k == "agentRadius") return num(br.agentRadius);
    if (k == "maxSpeed") return num(br.maxSpeed);
    if (k == "maxAccel") return num(br.maxAccel);
    if (k == "separation") return num(br.separation);
    if (k == "wallMargin") return num(br.wallMargin);
    if (k == "turnRate") return num(br.turnRate);
    if (k == "debugDraw") return bl(br.debugDraw);
    return sol::make_object(sv, sol::lua_nil);
}
} // namespace

void ScriptEngine::SetAiSystem(ai::AiSystem* a)
{
    m_aiSystem = a;
    if (a)
        a->SetActionInvoker([this](entt::entity e, i32 idx, ai::ActionPhase ph, f32 dt)
        {
            return InvokeAiAction(e, idx, static_cast<int>(ph), dt);
        });
}

void ScriptEngine::ClearAiLua()
{
    m_aiLua.reset();
}

int ScriptEngine::InvokeAiAction(entt::entity e, int idx, int phase, float dt)
{
    auto* store = static_cast<AiLuaStore*>(m_aiLua.get());
    if (!store || !m_lua) return -2;
    const u32 id = EntId(e);
    auto it = store->fns.find(id);
    if (it == store->fns.end() || idx < 0 || idx >= static_cast<int>(it->second.size())) return -2;
    // ★関数を写してから呼ぶ（呼んだ先で brain:action が同じ表を書き換えても壊れない）
    const AiLuaAction act = it->second[static_cast<size_t>(idx)];
    const sol::protected_function& fn = (phase == 0) ? act.enter : (phase == 1) ? act.update : act.exit;
    if (!fn.valid()) return 0;
    LuaBrain h{ id };
    sol::protected_function_result r = (phase == 1) ? fn(h, dt) : fn(h);
    if (!r.valid())
    {
        sol::error err = r;
        int& n = store->errors[id];
        if (++n <= 3)
        {
            std::string actName = "?";
            if (const ai::BrainState* st = m_aiSystem ? m_aiSystem->GetBrain(e) : nullptr)
                if (idx < static_cast<int>(st->actions.size())) actName = st->actions[static_cast<size_t>(idx)].name;
            static const char* kPh[] = { "enter", "update", "exit" };
            Logger::Warn("Brain(entity {}) の行動 '{}' の {} でエラー{}: {}", id, actName,
                         kPh[(std::clamp)(phase, 0, 2)], n == 3 ? "（以降は表示しない）" : "", err.what());
        }
        return -1;
    }
    if (phase == 1 && r.return_count() > 0)
    {
        sol::object o = r;
        if (o.get_type() == sol::type::boolean && o.as<bool>()) return 1;
        if (o.get_type() == sol::type::string && o.as<std::string>() == "done") return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ai グローバルと Brain のハンドル
//   local b = ai.brain(self, config?)    -- Brain が無ければ付ける。config で設定を上書き
//   b:action(name, { weight, cooldown, minDuration, considerations = {...}, enter, update, exit })
//   b:set / get / has / unset（黒板）、b:moveTo / stop / setSpeed（群衆で移動）、b:random（シード付き）
//   ai.emitSound(pos, radius, { tag, loudness, source })  -- EventBus の "ai.sound" を発火
//   ai.soundOnEvent(eventName, radius, { tag, loudness }) -- アニメイベント等を音にする（足音）
// ---------------------------------------------------------------------------
void ScriptEngine::RegisterAiBindings()
{
    auto& lua = *m_lua;
    m_aiLua = std::make_shared<AiLuaStore>();
    sol::table aiT = lua.create_named_table("ai");

    auto store = [this]() { return static_cast<AiLuaStore*>(m_aiLua.get()); };
    auto brainOf = [this](const LuaBrain& b) -> ai::BrainState*
    {
        return m_aiSystem ? m_aiSystem->GetBrain(EntOf(b.entity)) : nullptr;
    };
    auto requireBrain = [brainOf](const LuaBrain& b, const char* fn) -> ai::BrainState&
    {
        ai::BrainState* st = brainOf(b);
        if (!st) throw sol::error(std::string("brain:") + fn + ": Brain が無い（Play 中に ai.brain(self) で取ること）");
        return *st;
    };
    auto cfgOf = [this](const LuaBrain& b) -> Brain*
    {
        if (!m_scene) return nullptr;
        auto& reg = m_scene->GetRegistry();
        const entt::entity e = EntOf(b.entity);
        return (reg.valid(e) && reg.all_of<Brain>(e)) ? &reg.get<Brain>(e) : nullptr;
    };

    // ---- ai.brain(self, config?) ----
    aiT.set_function("brain", [this](sol::this_state ts, sol::object target, sol::optional<sol::table> config)
        -> sol::object
    {
        sol::state_view sv(ts);
        if (!m_aiSystem || !m_scene) return sol::make_object(sv, sol::lua_nil);
        const entt::entity e = ResolveEntityArg(m_scene, target);
        if (e == entt::null) throw sol::error("ai.brain: entity が見つからない（self / Entity / 名前を渡すこと）");
        auto& reg = m_scene->GetRegistry();
        Brain& br = reg.get_or_emplace<Brain>(e);
        if (config) ApplyBrainConfig(br, *config);
        if (!m_aiSystem->EnsureBrain(*m_scene, e)) return sol::make_object(sv, sol::lua_nil);
        return sol::make_object(sv, LuaBrain{ EntId(e) });
    });

    // ---- ai.brains() -> { Entity, ... }（id 昇順）----
    aiT.set_function("brains", [this](sol::this_state ts) -> sol::table
    {
        sol::state_view sv(ts);
        sol::table t = sv.create_table();
        if (!m_aiSystem || !m_scene) return t;
        auto& reg = m_scene->GetRegistry();
        int i = 1;
        for (const entt::entity e : m_aiSystem->Brains())
            if (reg.valid(e)) t[i++] = Entity(e, &reg);
        return t;
    });

    // ---- ai.emitSound(pos, radius, opts?) ----
    aiT.set_function("emitSound", [this](sol::object pos, float radius, sol::optional<sol::table> opts)
    {
        if (!m_eventBus && !m_aiSystem) return;
        XMFLOAT3 p{};
        if (!LuaToVec3(pos, p)) throw sol::error("ai.emitSound: pos には Vec3 を渡すこと");
        EngineEvent ev;
        ev.name = "ai.sound";
        ev.set("x", static_cast<double>(p.x));
        ev.set("y", static_cast<double>(p.y));
        ev.set("z", static_cast<double>(p.z));
        ev.set("radius", static_cast<double>(radius));
        if (opts)
        {
            ev.set("loudness", opts->get_or("loudness", 1.0));
            ev.set("tag", opts->get_or<std::string>("tag", ""));
            sol::object src = (*opts)["source"];
            if (src.valid() && src.get_type() != sol::type::lua_nil)
                ev.source = ResolveEntityArg(m_scene, src);
        }
        if (m_eventBus) m_eventBus->Emit(ev);   // AiSystem は "ai.sound" を購読している（他の購読者にも届く）
        else if (m_aiSystem)
        {
            ai::SoundEvent s;
            s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
            s.radius = radius;
            m_aiSystem->EmitSound(s);
        }
    });

    // ---- ai.soundOnEvent(eventName, radius, opts?) ----
    aiT.set_function("soundOnEvent", [this](const std::string& name, float radius, sol::optional<sol::table> opts)
        -> bool
    {
        if (!m_aiSystem || name.empty()) return false;
        const float loud = opts ? opts->get_or("loudness", 1.0f) : 1.0f;
        const std::string tag = opts ? opts->get_or<std::string>("tag", "") : std::string();
        m_aiSystem->AddSoundBridge(name, radius, loud, tag);
        return true;
    });

    // ---- ai.curve(type, x, params?) -> number（カーブの形を試す）----
    aiT.set_function("curve", [](const std::string& type, float x, sol::optional<sol::table> params) -> float
    {
        ai::CurveType ct;
        if (!ai::ParseCurveType(type, ct)) throw sol::error("ai.curve: 未知のカーブ '" + type + "'");
        ai::ResponseCurve c = ai::ResponseCurve::Make(ct);
        if (params)
        {
            sol::optional<float> m = (*params)["m"], k = (*params)["k"], b = (*params)["b"], cc = (*params)["c"];
            if (m) c.m = *m;
            if (k) c.k = *k;
            if (b) c.b = *b;
            if (cc) c.c = *cc;
            c.invert = params->get_or("invert", false);
        }
        return c.Evaluate(x);
    });

    // ---- Brain のハンドル ----
    lua.new_usertype<LuaBrain>("AiBrain",
        sol::no_constructor,
        // 行動の定義（同じ名前なら置き換え）。戻り値 = 行動の番号（1 始まり）
        "action", [this, store](LuaBrain& self, const std::string& name, sol::table def) -> int
        {
            if (!m_aiSystem) return 0;
            ai::ActionDef ad;
            ad.name = name;
            ad.weight = def.get_or("weight", 1.0f);
            ad.cooldown = def.get_or("cooldown", 0.0f);
            ad.minDuration = def.get_or("minDuration", 0.0f);
            sol::object cons = def["considerations"];
            if (cons.get_type() == sol::type::table)
            {
                sol::table ct = cons;
                for (size_t i = 1; i <= ct.size(); ++i)
                {
                    sol::object c = ct[i];
                    if (c.get_type() != sol::type::table)
                        throw sol::error("brain:action('" + name + "'): considerations の要素はテーブル");
                    ad.considerations.push_back(ReadConsideration(c.as<sol::table>(), name));
                }
            }
            const i32 idx = m_aiSystem->DefineAction(EntOf(self.entity), std::move(ad));
            if (idx < 0) throw sol::error("brain:action: Brain が無い（ai.brain(self) で取ったハンドルを使うこと）");
            AiLuaAction fns;
            sol::object en = def["enter"], up = def["update"], ex = def["exit"];
            if (en.get_type() == sol::type::function) fns.enter = en.as<sol::protected_function>();
            if (up.get_type() == sol::type::function) fns.update = up.as<sol::protected_function>();
            if (ex.get_type() == sol::type::function) fns.exit = ex.as<sol::protected_function>();
            auto& v = store()->fns[self.entity];
            if (v.size() <= static_cast<size_t>(idx)) v.resize(static_cast<size_t>(idx) + 1);
            v[static_cast<size_t>(idx)] = std::move(fns);
            return idx + 1;
        },
        "clearActions", [this, store](LuaBrain& self)
        {
            if (m_aiSystem) m_aiSystem->ClearActions(EntOf(self.entity));
            store()->fns.erase(self.entity);
        },
        // ---- 黒板 ----
        "set", [brainOf](LuaBrain& self, const std::string& key, sol::object value)
        {
            if (ai::BrainState* st = brainOf(self)) st->bb.Set(key, LuaToBb(value));
        },
        "get", [this, brainOf](LuaBrain& self, sol::this_state ts, const std::string& key, sol::object def)
            -> sol::object
        {
            sol::state_view sv(ts);
            ai::BrainState* st = brainOf(self);
            const ai::BbValue* v = st ? st->bb.Find(key) : nullptr;
            if (!v) return def;
            return BbToLua(sv, m_scene->GetRegistry(), *v);
        },
        "has", [brainOf](LuaBrain& self, const std::string& key) -> bool
        {
            ai::BrainState* st = brainOf(self);
            return st && st->bb.Has(key);
        },
        "unset", [brainOf](LuaBrain& self, const std::string& key)
        {
            if (ai::BrainState* st = brainOf(self)) st->bb.Erase(key);
        },
        // ---- 行動 ----
        "current", [brainOf](LuaBrain& self, sol::this_state ts) -> sol::object
        {
            sol::state_view sv(ts);
            ai::BrainState* st = brainOf(self);
            if (!st || st->current < 0) return sol::make_object(sv, sol::lua_nil);
            return sol::make_object(sv, st->actions[static_cast<size_t>(st->current)].name);
        },
        "timeInAction", [this, brainOf](LuaBrain& self) -> double
        {
            ai::BrainState* st = brainOf(self);
            return (st && st->current >= 0 && m_aiSystem) ? m_aiSystem->Time() - st->enteredAt : 0.0;
        },
        "force", [this](LuaBrain& self, const std::string& name) -> bool
        {
            return m_aiSystem && m_aiSystem->ForceAction(EntOf(self.entity), name);
        },
        "think", [this](LuaBrain& self)
        {
            if (m_aiSystem) m_aiSystem->RequestThink(EntOf(self.entity));
        },
        // 最後の評価の内訳 { chosen, reason, time, actions = { {name, score, final, bonus, weight, cooldown,
        //   considerations = { {name, input, value, x, score, missing} } } } }
        "scores", [brainOf](LuaBrain& self, sol::this_state ts) -> sol::table
        {
            sol::state_view sv(ts);
            sol::table t = sv.create_table();
            ai::BrainState* st = brainOf(self);
            if (!st) return t;
            const ai::Decision& d = st->last;
            if (d.chosen >= 0 && d.chosen < static_cast<i32>(st->actions.size()))
                t["chosen"] = st->actions[static_cast<size_t>(d.chosen)].name;
            t["reason"] = ai::DecisionReasonName(d.reason);
            t["time"] = d.time;
            sol::table acts = sv.create_table();
            int i = 1;
            for (const ai::ActionEval& ae : d.actions)
            {
                sol::table a = sv.create_table();
                a["name"] = ae.name; a["score"] = ae.score; a["final"] = ae.final; a["bonus"] = ae.bonus;
                a["weight"] = ae.weight; a["cooldown"] = ae.cooldown;
                sol::table cs = sv.create_table();
                int j = 1;
                for (const ai::ConsiderationEval& ce : ae.considerations)
                {
                    sol::table c = sv.create_table();
                    c["name"] = ce.name; c["input"] = ce.input; c["value"] = ce.raw;
                    c["x"] = ce.x; c["score"] = ce.score; c["missing"] = ce.missing;
                    cs[j++] = c;
                }
                a["considerations"] = cs;
                acts[i++] = a;
            }
            t["actions"] = acts;
            return t;
        },
        // ---- 移動（群衆）----
        "moveTo", [this](LuaBrain& self, sol::object pos, sol::optional<float> speed) -> bool
        {
            XMFLOAT3 p{};
            if (!m_aiSystem || !LuaToVec3(pos, p)) return false;
            const float pp[3] = { p.x, p.y, p.z };
            if (speed && *speed > 0.0f) m_aiSystem->SetBrainSpeed(EntOf(self.entity), *speed);
            return m_aiSystem->MoveTo(EntOf(self.entity), pp, speed.value_or(0.0f));
        },
        "stop", [this](LuaBrain& self) -> bool
        {
            return m_aiSystem && m_aiSystem->Stop(EntOf(self.entity));
        },
        "setSpeed", [this](LuaBrain& self, float speed)
        {
            if (m_aiSystem) m_aiSystem->SetBrainSpeed(EntOf(self.entity), speed);
        },
        "arrived", [this](LuaBrain& self, sol::optional<float> tol) -> bool
        {
            return m_aiSystem && m_aiSystem->Arrived(EntOf(self.entity), tol.value_or(0.5f));
        },
        "moveState", [this](LuaBrain& self) -> std::string
        {
            const nav::NavCrowdAgent* ag = m_aiSystem ? m_aiSystem->GetAgent(EntOf(self.entity)) : nullptr;
            if (!ag) return "none";
            if (ag->targetState == nav::NavMoveState::Valid && ag->partial) return "partial";
            return nav::NavMoveStateName(ag->targetState);
        },
        "distanceToGoal", [this](LuaBrain& self) -> float
        {
            return m_aiSystem ? m_aiSystem->DistanceToGoal(EntOf(self.entity)) : 0.0f;
        },
        "speed", [this](LuaBrain& self) -> float
        {
            f32 v[3]{};
            if (!m_aiSystem || !m_aiSystem->ActualVelocity(EntOf(self.entity), v)) return 0.0f;
            return std::sqrt(v[0] * v[0] + v[2] * v[2]);
        },
        "position", [this](LuaBrain& self) -> XMFLOAT3
        {
            auto& reg = m_scene->GetRegistry();
            const entt::entity e = EntOf(self.entity);
            return (reg.valid(e) && reg.all_of<Transform>(e)) ? reg.get<Transform>(e).position : XMFLOAT3{};
        },
        // ---- 乱数（Brain ごとのシード付き＝決定論）----
        "random", [requireBrain](LuaBrain& self) -> double
        {
            return requireBrain(self, "random").rng.Uniform();
        },
        "randomRange", [requireBrain](LuaBrain& self, double lo, double hi) -> double
        {
            return requireBrain(self, "randomRange").rng.Range(lo, hi);
        },
        "randomInt", [requireBrain](LuaBrain& self, lua_Integer lo, lua_Integer hi) -> lua_Integer
        {
            return static_cast<lua_Integer>(requireBrain(self, "randomInt").rng.Int(lo, hi));
        },
        // center のまわり radius 以内のナビ上の点（見つからなければ nil）
        "randomPoint", [this, requireBrain](LuaBrain& self, sol::this_state ts, sol::object center, float radius)
            -> sol::object
        {
            sol::state_view sv(ts);
            ai::BrainState& st = requireBrain(self, "randomPoint");
            XMFLOAT3 c{};
            if (!LuaToVec3(center, c) || !m_scene || !m_scene->HasNavMesh()) return sol::make_object(sv, sol::lua_nil);
            const auto& nm = m_scene->GetNavMesh();
            const float ext[3] = { 2.0f, (std::max)(4.0f, m_scene->GetNavConfig().agentHeight * 2.0f), 2.0f };
            for (int i = 0; i < 12; ++i)
            {
                const double a = st.rng.Uniform() * 6.283185307179586;
                const double r = std::sqrt(st.rng.Uniform()) * radius;
                const float p[3] = { c.x + static_cast<float>(std::cos(a) * r), c.y,
                                     c.z + static_cast<float>(std::sin(a) * r) };
                float out[3];
                if (nm.FindNearestPoly(p, ext, out) >= 0)
                    return sol::make_object(sv, XMFLOAT3{ out[0], out[1], out[2] });
            }
            return sol::make_object(sv, sol::lua_nil);
        },
        // ---- 知覚 ----
        "canSee", [brainOf](LuaBrain& self) -> bool
        {
            ai::BrainState* st = brainOf(self);
            return st && st->bb.Bool("target.seen", false);
        },
        "target", [this, brainOf](LuaBrain& self, sol::this_state ts) -> sol::object
        {
            sol::state_view sv(ts);
            ai::BrainState* st = brainOf(self);
            const ai::BbValue* v = st ? st->bb.Find("target") : nullptr;
            if (!v) return sol::make_object(sv, sol::lua_nil);
            return BbToLua(sv, m_scene->GetRegistry(), *v);
        },
        "awareness", [brainOf](LuaBrain& self) -> double
        {
            ai::BrainState* st = brainOf(self);
            return st ? st->bb.Number("target.awareness", 0.0) : 0.0;
        },
        "lastKnown", [brainOf](LuaBrain& self, sol::this_state ts) -> sol::object
        {
            sol::state_view sv(ts);
            ai::BrainState* st = brainOf(self);
            ai::BbVec3 p;
            if (!st || !st->bb.Vec3("target.lastKnown", p)) return sol::make_object(sv, sol::lua_nil);
            return sol::make_object(sv, XMFLOAT3{ p.x, p.y, p.z });
        },
        "lastSeenAge", [brainOf](LuaBrain& self) -> double
        {
            ai::BrainState* st = brainOf(self);
            return st ? st->bb.Number("target.lastSeenAge", 1e9) : 1e9;
        },
        // 直近に聞いた音 { pos, loudness, age, tag, occluded } | nil
        "heard", [this, brainOf](LuaBrain& self, sol::this_state ts) -> sol::object
        {
            sol::state_view sv(ts);
            ai::BrainState* st = brainOf(self);
            if (!st || st->heard.empty() || !m_aiSystem) return sol::make_object(sv, sol::lua_nil);
            const ai::HeardSound& h = st->heard.back();
            sol::table t = sv.create_table();
            t["pos"] = XMFLOAT3{ h.pos[0], h.pos[1], h.pos[2] };
            t["loudness"] = h.loudness;
            t["age"] = m_aiSystem->Time() - h.time;
            t["tag"] = h.tag;
            t["occluded"] = h.occluded;
            return t;
        },
        "remember", [this](LuaBrain& self, sol::object pos) -> bool
        {
            XMFLOAT3 p{};
            if (!m_aiSystem || !LuaToVec3(pos, p)) return false;
            const float pp[3] = { p.x, p.y, p.z };
            return m_aiSystem->Remember(EntOf(self.entity), pp);
        },
        "forget", [this](LuaBrain& self)
        {
            if (m_aiSystem) m_aiSystem->Forget(EntOf(self.entity));
        },
        // 自分の位置で音を出す（自分には聞こえない）
        "sound", [this](LuaBrain& self, float radius, sol::optional<std::string> tag)
        {
            if (!m_aiSystem || !m_scene) return;
            auto& reg = m_scene->GetRegistry();
            const entt::entity e = EntOf(self.entity);
            if (!reg.valid(e) || !reg.all_of<Transform>(e)) return;
            const auto& p = reg.get<Transform>(e).position;
            EngineEvent ev;
            ev.name = "ai.sound";
            ev.source = e;
            ev.set("x", static_cast<double>(p.x));
            ev.set("y", static_cast<double>(p.y));
            ev.set("z", static_cast<double>(p.z));
            ev.set("radius", static_cast<double>(radius));
            ev.set("tag", tag.value_or(std::string()));
            if (m_eventBus) m_eventBus->Emit(ev);
        },
        // ---- 設定（Brain コンポーネント）----
        "config", [cfgOf](LuaBrain& self, sol::table t)
        {
            if (Brain* br = cfgOf(self)) ApplyBrainConfig(*br, t);
        },
        "getConfig", [cfgOf](LuaBrain& self, sol::this_state ts, const std::string& key) -> sol::object
        {
            sol::state_view sv(ts);
            Brain* br = cfgOf(self);
            if (!br) return sol::make_object(sv, sol::lua_nil);
            return BrainConfigValue(sv, *br, key);
        },
        "entity", [this](LuaBrain& self) -> Entity { return Entity(EntOf(self.entity), &m_scene->GetRegistry()); },
        "id", [](LuaBrain& self) -> lua_Integer { return static_cast<lua_Integer>(self.entity); }
    );
}

} // namespace dx12e
