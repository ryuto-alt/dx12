#pragma once

// 新規シーン（new_scene / Ctrl+N）の既定値を作る純関数。ヘッダオンリー（entt / GPU 非依存）。
// ApplicationRender.cpp の新規シーン生成と tests/new_scene_defaults_test.cpp が共有する。

#include <DirectXMath.h>

#include "editor/LightMath.h"

namespace dx12e::newscene
{

// 新規シーンの太陽の既定: 方位 220°・高度 38°（影が斜めに落ちる）、色温度 5500K。
inline constexpr float kSunAzimuthDeg   = 220.0f;
inline constexpr float kSunElevationDeg = 38.0f;
inline constexpr float kSunKelvin       = 5500.0f;

struct SunDefaults
{
    DirectX::XMFLOAT3 direction{};   // 光が進む向き（正規化済み）
    DirectX::XMFLOAT3 rotation{};    // Transform の回転（度）。direction と同じ向きを指す（前方 +Z を回した結果）
    DirectX::XMFLOAT3 color{};
};

inline SunDefaults MakeSunDefaults()
{
    namespace lm = dx12e::lightmath;
    SunDefaults d;
    const lm::SunAngles a{kSunAzimuthDeg, kSunElevationDeg};
    d.direction = lm::SunAnglesToDirection(a);
    d.color     = lm::KelvinToRGB(kSunKelvin);
    // 前方 (0,0,1) に pitch(X)=高度、yaw(Y)=方位+180 を掛けると direction に一致する
    d.rotation  = {a.elevationDeg, lm::WrapDeg180(a.azimuthDeg + 180.0f), 0.0f};
    return d;
}

}  // namespace dx12e::newscene
