// scene/NewSceneDefaults.h（新規シーンの太陽の既定）の単体テスト。ヘッダオンリー・純 DirectXMath。
// 守りたい回帰: 高度 20〜60° / 影が斜め / Transform 回転と direction が同じ向き / 色温度 5500K。

#include "scene/NewSceneDefaults.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main()
{
    const newscene::SunDefaults d = newscene::MakeSunDefaults();

    // 正規化済み
    const float len = std::sqrt(d.direction.x * d.direction.x + d.direction.y * d.direction.y +
                                d.direction.z * d.direction.z);
    CHECK(std::fabs(len - 1.0f) < 1e-4f);

    // 高度が 20〜60°（往復で確かめる）
    const lightmath::SunAngles a = lightmath::DirectionToSunAngles(d.direction);
    CHECK(a.elevationDeg >= 20.0f && a.elevationDeg <= 60.0f);
    CHECK(std::fabs(a.elevationDeg - 38.0f) < 0.01f);
    CHECK(std::fabs(lightmath::WrapDeg180(a.azimuthDeg - 220.0f)) < 0.01f);

    // 影が斜め: 真下でも水平でもない（水平成分あり、下向き成分あり）
    CHECK(d.direction.y < -0.3f);
    CHECK(std::fabs(d.direction.x) > 0.1f || std::fabs(d.direction.z) > 0.1f);

    // Transform 回転で前方 +Z を回した向きが direction と一致する
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(d.rotation.x), XMConvertToRadians(d.rotation.y), XMConvertToRadians(d.rotation.z));
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q));
    CHECK(std::fabs(fwd.x - d.direction.x) < 1e-4f);
    CHECK(std::fabs(fwd.y - d.direction.y) < 1e-4f);
    CHECK(std::fabs(fwd.z - d.direction.z) < 1e-4f);

    // 色温度 5500K（暖色寄り: r >= g >= b）
    const XMFLOAT3 k = lightmath::KelvinToRGB(5500.0f);
    CHECK(d.color.x == k.x && d.color.y == k.y && d.color.z == k.z);
    CHECK(d.color.x >= d.color.y && d.color.y >= d.color.z);

    std::printf("new_scene_defaults_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
