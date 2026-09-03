#include "scene/ShaderParamTween.h"

#include "ecs/Components.h"
#include "resource/ShaderDiagnostics.h"

#include <algorithm>

namespace dx12e
{
namespace
{

// エンティティが使っているシェーダーのキー（正規化済み relPath）を、枠ごとに引く。
// 空文字なら「その枠のシェーダーは割り当てられていない」。
std::string MeshShaderKey(const entt::registry& reg, entt::entity e)
{
    if (!reg.valid(e)) return {};
    const auto* mr = reg.try_get<MeshRenderer>(e);
    if (!mr || mr->shaderPath.empty()) return {};
    return shaderdiag::NormalizeKey(mr->shaderPath);
}

std::string ScreenShaderKey(const entt::registry& reg, entt::entity e)
{
    if (!reg.valid(e)) return {};
    const auto* cc = reg.try_get<CameraComponent>(e);
    if (!cc || cc->screenShaderPath.empty()) return {};
    return shaderdiag::NormalizeKey(cc->screenShaderPath);
}

f32 ApplyEase(f32 t, ShaderTweenEase ease)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (ease)
    {
    case ShaderTweenEase::Out:   return 1.0f - (1.0f - t) * (1.0f - t);
    case ShaderTweenEase::In:    return t * t;
    case ShaderTweenEase::InOut: return (t < 0.5f) ? (2.0f * t * t)
                                                   : (1.0f - 0.5f * (2.0f - 2.0f * t) * (2.0f - 2.0f * t));
    case ShaderTweenEase::Linear:
    default:                     return t;
    }
}

// 書き込み先の float を毎回引き直す。
// ★ポインタを Item に持たせないこと。entt はコンポーネントを再配置するので、
//   トゥイーンの途中で別のエンティティを生成/削除すると容易にぶら下がる。
f32* SlotPtr(entt::registry& reg, entt::entity e, shaderparams::Space space, u32 index)
{
    if (!reg.valid(e)) return nullptr;

    if (space == shaderparams::Space::MeshObject)
    {
        if (index >= shaderparams::kMeshFreeFloats) return nullptr;
        auto* mr = reg.try_get<MeshRenderer>(e);
        return mr ? (mr->CustomParamBase() + index) : nullptr;
    }

    if (index >= shaderparams::kScreenFreeFloats) return nullptr;
    auto* cc = reg.try_get<CameraComponent>(e);
    return cc ? (&cc->screenShaderParams.x + index) : nullptr;
}

} // namespace

const char* ShaderTweenEaseName(int e)
{
    switch (static_cast<ShaderTweenEase>(e))
    {
    case ShaderTweenEase::Out:   return "減速 (out)";
    case ShaderTweenEase::In:    return "加速 (in)";
    case ShaderTweenEase::InOut: return "両端ゆるめ (inOut)";
    case ShaderTweenEase::Linear:
    default:                     return "等速 (linear)";
    }
}

std::vector<shaderparams::Param> ListShaderParams(const entt::registry& reg, entt::entity e)
{
    std::vector<shaderparams::Param> out;

    const std::string meshKey = MeshShaderKey(reg, e);
    if (!meshKey.empty())
    {
        std::vector<shaderparams::Param> mesh =
            shaderparams::GetIn(meshKey, shaderparams::Space::MeshObject);
        out.insert(out.end(), mesh.begin(), mesh.end());
    }

    const std::string screenKey = ScreenShaderKey(reg, e);
    if (!screenKey.empty())
    {
        std::vector<shaderparams::Param> screen =
            shaderparams::GetIn(screenKey, shaderparams::Space::Screen);
        out.insert(out.end(), screen.begin(), screen.end());
    }
    return out;
}

bool FindShaderParam(const entt::registry& reg, entt::entity e,
                     const std::string& name, shaderparams::Param& out)
{
    if (name.empty()) return false;
    for (const shaderparams::Param& p : ListShaderParams(reg, e))
    {
        if (p.name != name) continue;
        out = p;
        return true;
    }
    return false;
}

bool WriteShaderParam(entt::registry& reg, entt::entity e,
                      const shaderparams::Param& p, f32 value)
{
    f32* ptr = SlotPtr(reg, e, p.space, p.Index());
    if (!ptr) return false;
    *ptr = value;
    return true;
}

void ShaderParamTweens::CancelSlot(entt::entity target, shaderparams::Space space, u32 index)
{
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                 [&](const Item& it)
                                 {
                                     return it.target == target && it.space == space
                                         && it.index == index;
                                 }),
                  m_items.end());
}

bool ShaderParamTweens::Start(entt::registry& reg, entt::entity target, const std::string& name,
                              f32 from, f32 to, f32 duration, ShaderTweenEase ease,
                              bool keepIfIdentical)
{
    shaderparams::Param p;
    if (!FindShaderParam(reg, target, name, p)) return false;
    if (!p.IsAnimatable())                      return false;

    if (keepIfIdentical)
    {
        // Stay から毎フレーム同じ指示が来ているだけ。走っているものをそのまま進ませる。
        for (const Item& it : m_items)
        {
            if (it.target != target || it.space != p.space || it.index != p.Index()) continue;
            if (it.from == from && it.to == to && it.duration == duration && it.ease == ease)
                return true;
        }
    }

    CancelSlot(target, p.space, p.Index());

    // 開始値をまず書く。0 秒指定は「即代入」と同じ意味にする。
    if (!WriteShaderParam(reg, target, p, from)) return false;
    if (duration <= 0.0f)
        return WriteShaderParam(reg, target, p, to);

    Item it;
    it.target   = target;
    it.space    = p.space;
    it.index    = p.Index();
    it.from     = from;
    it.to       = to;
    it.duration = duration;
    it.ease     = ease;
    m_items.push_back(it);
    return true;
}

bool ShaderParamTweens::SetNow(entt::registry& reg, entt::entity target,
                               const std::string& name, f32 value)
{
    shaderparams::Param p;
    if (!FindShaderParam(reg, target, name, p)) return false;

    CancelSlot(target, p.space, p.Index());
    return WriteShaderParam(reg, target, p, value);
}

void ShaderParamTweens::Update(entt::registry& reg, f32 dt)
{
    if (m_items.empty()) return;

    for (size_t i = 0; i < m_items.size();)
    {
        Item& it = m_items[i];

        f32* ptr = SlotPtr(reg, it.target, it.space, it.index);
        if (!ptr)
        {
            // 対象が消された / シェーダーの割り当てが外れた。黙って畳む。
            m_items.erase(m_items.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }

        it.elapsed += dt;
        const f32 t = (it.duration > 0.0f) ? (it.elapsed / it.duration) : 1.0f;
        if (t >= 1.0f)
        {
            *ptr = it.to;   // ★最後は必ず目標値そのものを書く（誤差で 0.999 が残らない）
            m_items.erase(m_items.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }

        *ptr = it.from + (it.to - it.from) * ApplyEase(t, it.ease);
        ++i;
    }
}

} // namespace dx12e
