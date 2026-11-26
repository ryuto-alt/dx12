#include "scene/ComponentSerializerRegistry.h"

#include <cstddef>
#include <cstring>
#include <set>
#include <string>

#include "core/Logger.h"

namespace dx12e
{

// 1 パス目: 各登録関数の宣言。2 パス目: 呼び出し。
#define UNO_COMPONENT_SERIALIZER(Fn) void Fn(std::vector<ComponentSerializer>& out);
#include "scene/ComponentSerializerList.inc"
#undef UNO_COMPONENT_SERIALIZER

void RegisterAllComponentSerializers(std::vector<ComponentSerializer>& out)
{
    (void)out;   // .inc が空の間は未使用
#define UNO_COMPONENT_SERIALIZER(Fn) Fn(out);
#include "scene/ComponentSerializerList.inc"
#undef UNO_COMPONENT_SERIALIZER
}

namespace
{
constexpr size_t kReserve = 256;   // 参照（McpUndo のスロットが握る）を再確保で無効にしないための余裕
size_t g_baseCount = 0;

// 必須の関数が欠けた / typeName が重複した登録は警告して落とす（保存と読み込みが null を呼ばないように）。
// remove は Undo の「前は無かった」に要るので必須。
void ValidateSerializers(std::vector<ComponentSerializer>& v)
{
    std::set<std::string> seen;
    for (size_t i = 0; i < v.size();)
    {
        const ComponentSerializer& cs = v[i];
        const bool ok = cs.typeName && cs.has && cs.save && cs.load && cs.remove && seen.insert(cs.typeName).second;
        if (ok) { ++i; continue; }
        Logger::Warn("[ComponentRegistry] 不正な登録を無視しました（関数の欠け / typeName の重複）: {}", cs.typeName ? cs.typeName : "(null)");
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

std::vector<ComponentSerializer>& MutableList()
{
    static std::vector<ComponentSerializer> list = [] {
        std::vector<ComponentSerializer> v;
        v.reserve(kReserve);
        RegisterAllComponentSerializers(v);
        ValidateSerializers(v);
        g_baseCount = v.size();
        return v;
    }();
    return list;
}
} // namespace

const std::vector<ComponentSerializer>& ComponentSerializers()
{
    return MutableList();
}

void TestOnlyAppendComponentSerializer(const ComponentSerializer& cs)
{
    auto& l = MutableList();
    if (l.size() < kReserve) l.push_back(cs);
}

void TestOnlyResetComponentSerializers()
{
    MutableList().resize(g_baseCount);
}

const ComponentSerializer* FindComponentSerializer(const char* typeName)
{
    if (!typeName) return nullptr;
    for (const ComponentSerializer& cs : ComponentSerializers())
        if (cs.typeName && std::strcmp(cs.typeName, typeName) == 0) return &cs;
    return nullptr;
}

} // namespace dx12e
