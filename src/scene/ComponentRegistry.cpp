// RuntimeComponentRegistry の実装（Phase 1）。
//
// typeName -> RuntimeComponentInfo の単一テーブル。SceneSerializer はこの ForEach 走査で
// コア部品を直列化/復元し、型ごとの if(reg.all_of<T>()) 連鎖を畳んでいく。
//
// このTUは nlohmann::json の「定義」を必要としない（ハンドラは std::function に格納され、
// json は参照でしか現れない）。json_fwd だけで足りるが、Scene ライブラリは nlohmann を
// 既にリンクしているため問題ない。

#include "engine/ecs/ComponentRegistry.h"

#include <utility>

namespace dx12e
{

RuntimeComponentRegistry& RuntimeComponentRegistry::Get()
{
    static RuntimeComponentRegistry instance;
    return instance;
}

void RuntimeComponentRegistry::Register(RuntimeComponentInfo info)
{
    // 同 typeName の再登録は置換（多重初期化に対して冪等にする）。
    for (auto& existing : m_infos)
    {
        if (existing.typeName == info.typeName)
        {
            existing = std::move(info);
            return;
        }
    }
    m_infos.push_back(std::move(info));
}

const RuntimeComponentInfo* RuntimeComponentRegistry::Find(const std::string& typeName) const
{
    for (const auto& info : m_infos)
    {
        if (info.typeName == typeName)
            return &info;
    }
    return nullptr;
}

void RuntimeComponentRegistry::ForEach(const std::function<void(const RuntimeComponentInfo&)>& fn) const
{
    for (const auto& info : m_infos)
        fn(info);
}

} // namespace dx12e
