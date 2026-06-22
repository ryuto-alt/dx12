// entt::meta によるコア部品フィールド反映の検証（Phase 2 土台）。
//
// RegisterCoreComponentMeta() 後、entt::resolve<T>().data() で各フィールドの name() を
// 取得できること＝Inspector 自動 UI / 汎用シリアライズの単一ソースが成立することを確認する。
//
// 実行: ctest --output-on-failure

#include "ecs/ComponentMeta.h"
#include "ecs/Components.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

template <typename T>
static std::vector<std::string> FieldNames()
{
    std::vector<std::string> names;
    // entt 3.16: meta_type::data() は std::pair<id_type, meta_data> の範囲を返す。
    for (auto&& entry : entt::resolve<T>().data())
    {
        if (const char* n = entry.second.name())
            names.emplace_back(n);
    }
    return names;
}

static bool Has(const std::vector<std::string>& v, const char* s)
{
    return std::find(v.begin(), v.end(), std::string(s)) != v.end();
}

// T が meta 登録され、期待フィールドが全て反映されているか。
template <typename T>
static void Expect(std::initializer_list<const char*> fields)
{
    CHECK(static_cast<bool>(entt::resolve<T>()));   // 型が解決できる
    const auto names = FieldNames<T>();
    for (const char* f : fields)
        CHECK(Has(names, f));
}

int main()
{
    RegisterCoreComponentMeta();

    Expect<DirectX::XMFLOAT3>({ "x", "y", "z" });
    Expect<Transform>({ "position", "rotation", "scale" });
    Expect<NameTag>({ "name" });
    Expect<PointLight>({ "color", "intensity", "range" });
    Expect<DirectionalLight>({ "direction", "color", "intensity", "ambient" });
    Expect<SpotLight>({ "color", "intensity", "range", "direction", "innerConeDeg", "outerConeDeg" });
    Expect<CameraComponent>({ "fovDegrees", "nearClip", "farClip", "isActive", "projection", "orthoSize" });
    Expect<RigidBody>({ "motionType", "mass", "restitution", "friction", "linearDamping", "angularDamping", "useGravity" });
    Expect<BoxCollider>({ "halfExtents", "offset" });
    Expect<SphereCollider>({ "radius", "offset" });
    Expect<CapsuleCollider>({ "radius", "halfHeight", "offset" });
    Expect<AudioSource>({ "clipPath", "volume", "loop", "spatial", "playOnStart", "minDistance", "maxDistance" });
    Expect<Tag>({ "tags" });
    Expect<Sprite2D>({ "texturePath", "layer", "size", "uvMin", "uvMax", "color", "worldSpace" });

    // 反映は冪等（2回呼んでも壊れない）
    RegisterCoreComponentMeta();
    CHECK(FieldNames<PointLight>().size() == 3);

    std::printf("component_meta: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
