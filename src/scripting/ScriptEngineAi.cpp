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

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <optional>
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
}

} // namespace dx12e
