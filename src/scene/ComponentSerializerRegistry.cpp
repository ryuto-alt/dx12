#include "scene/ComponentSerializerRegistry.h"

#include <cstddef>
#include <cstring>

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

std::vector<ComponentSerializer>& MutableList()
{
    static std::vector<ComponentSerializer> list = [] {
        std::vector<ComponentSerializer> v;
        v.reserve(kReserve);
        RegisterAllComponentSerializers(v);
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
