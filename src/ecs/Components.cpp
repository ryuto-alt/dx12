#include "ecs/Components.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/AnimGraphRuntime.h"

using namespace DirectX;

namespace dx12e
{

XMMATRIX Transform::GetWorldMatrix() const
{
    XMMATRIX s = XMMatrixScaling(scale.x, scale.y, scale.z);

    XMMATRIX r;
    if (useQuaternion)
    {
        XMVECTOR q = XMLoadFloat4(&quaternion);
        r = XMMatrixRotationQuaternion(q);
    }
    else
    {
        // Euler degrees -> radians, YXZ order (Yaw -> Pitch -> Roll)
        r = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(rotation.x),   // pitch
            XMConvertToRadians(rotation.y),    // yaw
            XMConvertToRadians(rotation.z));   // roll
    }

    XMMATRIX t = XMMatrixTranslation(position.x, position.y, position.z);

    return s * r * t;
}

XMMATRIX ComputeWorldMatrix(const entt::registry& reg, entt::entity e)
{
    XMMATRIX world = XMMatrixIdentity();
    entt::entity cur = e;
    int depth = 0;  // 循環ガード
    while (cur != entt::null && reg.valid(cur) && reg.all_of<Transform>(cur)
           && depth++ < 64)
    {
        const auto& t = reg.get<Transform>(cur);
        world = world * t.GetWorldMatrix();
        cur = t.parent;
    }
    return world;
}

SkeletalAnimation::~SkeletalAnimation() = default;
SkeletalAnimation::SkeletalAnimation(SkeletalAnimation&&) noexcept = default;
SkeletalAnimation& SkeletalAnimation::operator=(SkeletalAnimation&&) noexcept = default;

// AnimGraphRuntimeState は Components.h では前方宣言なので、
// 特殊メンバは全部ここ（完全型が見える TU）で定義する。
AnimatorController::AnimatorController() = default;
AnimatorController::~AnimatorController() = default;
AnimatorController::AnimatorController(AnimatorController&&) noexcept = default;
AnimatorController& AnimatorController::operator=(AnimatorController&&) noexcept = default;

// コピーは「永続フィールドだけ」。_state / _loaded は既定のままにして、
// コピー先が次の Update で .animfsm を読み直すようにする。
AnimatorController::AnimatorController(const AnimatorController& other)
    : graphPath(other.graphPath)
    , playOnStart(other.playOnStart)
    , speed(other.speed)
    , applyRootMotion(other.applyRootMotion)
    , eventChannel(other.eventChannel)
{
}

AnimatorController& AnimatorController::operator=(const AnimatorController& other)
{
    if (this != &other)
    {
        graphPath       = other.graphPath;
        playOnStart     = other.playOnStart;
        speed           = other.speed;
        applyRootMotion = other.applyRootMotion;
        eventChannel    = other.eventChannel;
        _state.reset();
        _loaded = false;
        _failed = false;
        _loadedPath.clear();
    }
    return *this;
}

void RewriteEntityNameRefs(entt::registry& reg, const std::string& oldName,
                           const std::string& newName)
{
    if (oldName.empty() || oldName == newName) return;

    for (auto [e, ls] : reg.view<LuaScript>().each())
    {
        (void)e;
        for (auto& p : ls.props)
            if (p.type == ScriptPropType::Entity && p.str == oldName)
                p.str = newName;
    }

    for (auto [e, tr] : reg.view<Trigger>().each())
    {
        (void)e;
        // filter が空 = 「Player」の暗黙指定。空を newName で埋めない。
        if (!tr.filter.empty() && tr.filter == oldName) tr.filter = newName;
        for (auto& a : tr.actions)
            if (a.target == oldName) a.target = newName;
    }
}

uint64_t ParseEntityGuidHex(const std::string& s)
{
    if (s.empty() || s.size() > 16) return 0;
    uint64_t v = 0;
    for (char c : s)
    {
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;   // 壊れた guid は「無い」扱い（名前フォールバックへ落ちる）
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v;
}

std::string FormatEntityGuidHex(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

entt::entity FindEntityByGuid(const entt::registry& reg, uint64_t guid)
{
    if (guid == 0) return entt::null;
    for (auto [e, g] : reg.view<const EntityGuid>().each())
        if (g.value == guid) return e;
    return entt::null;
}

entt::entity ResolveEntityRef(const entt::registry& reg, uint64_t guid, const std::string& name)
{
    if (const entt::entity e = FindEntityByGuid(reg, guid); e != entt::null) return e;
    // guid が 0（旧データ）か、指す先が消えている。名前で拾えるならそちらを使う。
    if (name.empty()) return entt::null;
    for (auto [e, n] : reg.view<const NameTag>().each())
        if (n.name == name) return e;
    return entt::null;
}

void PromoteTriggerRefsToGuid(entt::registry& reg)
{
    // 名前 → entity の索引を 1 回だけ作る（トリガーごとに全走査すると O(n*m) になる）。
    // 同名が複数ある場合の勝者は、実行時の名前引きと同じ「後勝ち」に揃える
    // （entt の view はプール末尾→先頭で回るため、最後に挿入されたものが勝つ）。
    std::unordered_map<std::string, entt::entity> byName;
    for (auto [e, n] : reg.view<const NameTag>().each())
        byName[n.name] = e;

    const auto guidOf = [&](const std::string& name) -> uint64_t {
        if (name.empty()) return 0;
        auto it = byName.find(name);
        if (it == byName.end()) return 0;              // 参照切れ。診断に拾わせる
        const auto* g = reg.try_get<EntityGuid>(it->second);
        return g ? g->value : 0;                        // まだ guid が無い（保存前）
    };

    for (auto [e, tr] : reg.view<Trigger>().each())
    {
        (void)e;
        if (tr.filterGuid == 0) tr.filterGuid = guidOf(tr.filter);
        for (auto& a : tr.actions)
            if (a.targetGuid == 0) a.targetGuid = guidOf(a.target);
    }
}

NodeAnimationComp::~NodeAnimationComp() = default;
NodeAnimationComp::NodeAnimationComp(NodeAnimationComp&&) noexcept = default;
NodeAnimationComp& NodeAnimationComp::operator=(NodeAnimationComp&&) noexcept = default;

} // namespace dx12e
