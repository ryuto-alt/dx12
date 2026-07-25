// editor/LightMath.h（シーンビューのライティング編集で使う純関数）の単体テスト。
// ヘッダオンリー・純 DirectXMath（ImGui も entt も GPU も要らない）。実行: ctest --output-on-failure
//
// ここで守りたい回帰:
//   ・方位/高度 ⇔ direction の往復（符号を1個間違えると太陽ドラッグが逆に回る）
//   ・時刻カーブが Lua の Lighting.sample と同じ値を返す（エディタと Lua で絵が食い違わない）
//   ・コーン半径 ⇔ 半頂角の往復（スポットのハンドルを掴んだ位置と角度がズレない）
//   ・ワールド→スクリーン投影（ハンドルの当たり判定の土台）
//   ・レイ幾何（平面交差 / 直線最近点 / 球）— range と向きのドラッグ変換

#include "editor/LightMath.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace dx12e::lightmath;
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

namespace
{

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

float Length3(const XMFLOAT3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

int main()
{
    // ---------------------------------------------------------------
    // 1) 方位 / 高度 ⇔ direction
    // ---------------------------------------------------------------
    {
        // 真上の太陽 → 光は真下へ進む
        const SunAngles zenith = DirectionToSunAngles(XMFLOAT3{0.0f, -1.0f, 0.0f});
        CHECK(Near(zenith.elevationDeg, 90.0f, 1e-3f));

        // 太陽が +X 側（光は -X へ進む）→ 方位 90 度・高度 0
        const SunAngles east = DirectionToSunAngles(XMFLOAT3{-1.0f, 0.0f, 0.0f});
        CHECK(Near(east.azimuthDeg, 90.0f, 1e-3f));
        CHECK(Near(east.elevationDeg, 0.0f, 1e-3f));

        // 太陽が +Z 側 → 方位 0 度
        const SunAngles north = DirectionToSunAngles(XMFLOAT3{0.0f, 0.0f, -1.0f});
        CHECK(Near(north.azimuthDeg, 0.0f, 1e-3f));

        // 0 ベクトル / 非有限は既定値へ落ちる（ドラッグ中に NaN を撒かない）
        const SunAngles degenerate = DirectionToSunAngles(XMFLOAT3{0.0f, 0.0f, 0.0f});
        CHECK(std::isfinite(degenerate.azimuthDeg) && std::isfinite(degenerate.elevationDeg));

        // 往復（高度 ±90 ちょうどは方位が定義できないので避ける）
        const float azs[] = {-170.0f, -90.0f, -12.5f, 0.0f, 45.0f, 179.0f};
        const float els[] = {-80.0f, -33.0f, 0.0f, 17.0f, 88.0f};
        for (float az : azs)
        {
            for (float el : els)
            {
                SunAngles in;
                in.azimuthDeg   = az;
                in.elevationDeg = el;
                const XMFLOAT3 dir = SunAnglesToDirection(in);
                CHECK(Near(Length3(dir), 1.0f, 1e-4f));
                const SunAngles out = DirectionToSunAngles(dir);
                CHECK(Near(out.azimuthDeg, az, 0.05f));
                CHECK(Near(out.elevationDeg, el, 0.05f));
            }
        }
    }

    // ---------------------------------------------------------------
    // 2) 角度の畳み込み / ドラッグ変換
    // ---------------------------------------------------------------
    {
        CHECK(Near(WrapDeg180(370.0f), 10.0f, 1e-3f));
        CHECK(Near(WrapDeg180(-190.0f), 170.0f, 1e-3f));
        CHECK(Near(std::fabs(WrapDeg180(180.0f)), 180.0f, 1e-3f));
        CHECK(Near(WrapDeg180(0.0f), 0.0f, 1e-3f));
        CHECK(Near(WrapDeg180(std::nanf("")), 0.0f, 1e-3f));   // NaN は 0 へ

        SunAngles cur;
        cur.azimuthDeg   = 0.0f;
        cur.elevationDeg = 45.0f;

        // 右へ動かすと方位が進む
        const SunAngles right = ApplySunDrag(cur, 100.0f, 0.0f, 0.5f);
        CHECK(Near(right.azimuthDeg, 50.0f, 1e-3f));
        CHECK(Near(right.elevationDeg, 45.0f, 1e-3f));

        // 下へ動かすと太陽が沈む（高度が下がる）
        const SunAngles down = ApplySunDrag(cur, 0.0f, 100.0f, 0.5f);
        CHECK(down.elevationDeg < cur.elevationDeg);

        // 高度は ±89 でクランプ（±90 だと方位が定義できず離した瞬間に飛ぶ）
        const SunAngles up = ApplySunDrag(cur, 0.0f, -100000.0f, 0.5f);
        CHECK(Near(up.elevationDeg, 89.0f, 1e-3f));
        const SunAngles deep = ApplySunDrag(cur, 0.0f, 100000.0f, 0.5f);
        CHECK(Near(deep.elevationDeg, -89.0f, 1e-3f));

        // 方位は必ず -180..180 に収まる
        const SunAngles wrapped = ApplySunDrag(cur, 100000.0f, 0.0f, 0.5f);
        CHECK(wrapped.azimuthDeg > -180.5f && wrapped.azimuthDeg <= 180.5f);
    }

    // ---------------------------------------------------------------
    // 3) 時刻カーブ（Lua の Lighting.sample と同じ定数・同じ式）
    // ---------------------------------------------------------------
    {
        // 正午: 太陽ほぼ天頂、昼の色 / 強度 / 環境光
        const TimeOfDaySample noon = SampleTimeOfDay(12.0f);
        CHECK(Near(noon.intensity, 3.0f, 1e-3f));          // Lighting.dayIntensity
        CHECK(Near(noon.ambient,   0.30f, 1e-3f));         // Lighting.dayAmbient
        CHECK(Near(noon.color.x, 1.00f, 1e-3f));
        CHECK(Near(noon.color.y, 0.97f, 1e-3f));
        CHECK(Near(noon.color.z, 0.92f, 1e-3f));
        CHECK(noon.direction.y < -0.9f);                   // 上から下へ照らす
        CHECK(Near(Length3(noon.direction), 1.0f, 1e-4f));

        // 深夜: 月明かり
        const TimeOfDaySample midnight = SampleTimeOfDay(0.0f);
        CHECK(Near(midnight.intensity, 0.35f, 1e-3f));     // Lighting.nightIntensity
        CHECK(Near(midnight.ambient,   0.05f, 1e-3f));     // Lighting.nightAmbient
        CHECK(midnight.direction.y < -0.9f);

        // 日の出（地平線）: 強度 0・環境光は duskAmbient に揃う
        const TimeOfDaySample dawn = SampleTimeOfDay(6.0f);
        CHECK(Near(dawn.intensity, 0.0f, 1e-3f));
        CHECK(Near(dawn.ambient,   0.12f, 1e-3f));         // Lighting.duskAmbient

        // 24 時 = 0 時、負の時刻も 0..24 へ畳む
        const TimeOfDaySample h24 = SampleTimeOfDay(24.0f);
        CHECK(Near(h24.intensity, midnight.intensity, 1e-4f));
        const TimeOfDaySample hneg = SampleTimeOfDay(-24.0f);
        CHECK(Near(hneg.intensity, midnight.intensity, 1e-4f));

        // 昼夜の継ぎ目（6 時）で強度と環境光が飛ばない＝薄明が連続している
        const TimeOfDaySample before = SampleTimeOfDay(5.99f);
        const TimeOfDaySample after  = SampleTimeOfDay(6.01f);
        CHECK(Near(before.intensity, after.intensity, 0.01f));
        CHECK(Near(before.ambient,   after.ambient,   0.01f));

        // 全時刻で「単位ベクトル・強度と環境光が定義域内」
        for (int i = 0; i <= 96; ++i)
        {
            const TimeOfDaySample s = SampleTimeOfDay(static_cast<float>(i) * 0.25f);
            CHECK(Near(Length3(s.direction), 1.0f, 1e-3f));
            CHECK(s.intensity >= 0.0f && s.intensity <= 3.0f + 1e-3f);
            CHECK(s.ambient >= 0.05f - 1e-3f && s.ambient <= 0.30f + 1e-3f);
        }
    }

    // ---------------------------------------------------------------
    // 4) 色温度
    // ---------------------------------------------------------------
    {
        const XMFLOAT3 candle = KelvinToRGB(1900.0f);
        CHECK(candle.x > candle.z);            // 低ケルビンは赤寄り
        const XMFLOAT3 sky = KelvinToRGB(10000.0f);
        CHECK(sky.z > sky.x);                  // 高ケルビンは青寄り
        const XMFLOAT3 neutral = KelvinToRGB(6600.0f);
        CHECK(neutral.x > 0.95f && neutral.y > 0.95f && neutral.z > 0.95f);

        for (int k = 1000; k <= 20000; k += 500)
        {
            const XMFLOAT3 c = KelvinToRGB(static_cast<float>(k));
            CHECK(c.x >= 0.0f && c.x <= 1.0f);
            CHECK(c.y >= 0.0f && c.y <= 1.0f);
            CHECK(c.z >= 0.0f && c.z <= 1.0f);
        }
    }

    // ---------------------------------------------------------------
    // 5) コーンの半径 ⇔ 半頂角、円周上の点
    // ---------------------------------------------------------------
    {
        const float r = ConeRadiusAt(10.0f, 30.0f);
        CHECK(Near(r, 10.0f * std::tan(XMConvertToRadians(30.0f)), 1e-4f));
        CHECK(Near(ConeHalfAngleFromRadius(r, 10.0f), 30.0f, 1e-3f));

        // 距離が変わっても角度は保たれる（range ハンドルを引いてもコーン角が動かない）
        const float r2 = ConeRadiusAt(37.0f, 12.5f);
        CHECK(Near(ConeHalfAngleFromRadius(r2, 37.0f), 12.5f, 1e-3f));

        // 距離 0 でも壊れない
        CHECK(std::isfinite(ConeHalfAngleFromRadius(1.0f, 0.0f)));

        // 真下を向いたコーンの底円上の点: 高さは -10、軸からの距離は tan(30)*10
        const XMFLOAT3 apex{0.0f, 0.0f, 0.0f};
        const XMFLOAT3 dir{0.0f, -1.0f, 0.0f};
        for (int i = 0; i < 8; ++i)
        {
            const float phase = XM_2PI * static_cast<float>(i) / 8.0f;
            const XMFLOAT3 p = ConeRimPoint(apex, dir, 10.0f, 30.0f, phase);
            CHECK(Near(p.y, -10.0f, 1e-3f));
            CHECK(Near(std::sqrt(p.x * p.x + p.z * p.z), r, 1e-3f));
        }

        // 基底は必ず直交（dir が上下でも破綻しない）
        XMFLOAT3 u{}, v{};
        BuildBasis(XMFLOAT3{0.0f, 1.0f, 0.0f}, u, v);
        CHECK(Near(u.x * v.x + u.y * v.y + u.z * v.z, 0.0f, 1e-4f));
        CHECK(Near(Length3(u), 1.0f, 1e-4f));
        CHECK(Near(Length3(v), 1.0f, 1e-4f));
    }

    // ---------------------------------------------------------------
    // 6) ワールド → スクリーン
    // ---------------------------------------------------------------
    {
        const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -5.0f, 1.0f),
                                               XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                               XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 4.0f / 3.0f, 0.1f, 100.0f);
        XMFLOAT4X4 vp{};
        XMStoreFloat4x4(&vp, XMMatrixMultiply(view, proj));

        // 注視点はビューポートの中央へ落ちる
        XMFLOAT2 s{};
        CHECK(WorldToScreen(vp, XMFLOAT3{0.0f, 0.0f, 0.0f}, 100.0f, 50.0f, 800.0f, 600.0f, s));
        CHECK(Near(s.x, 100.0f + 400.0f, 0.05f));
        CHECK(Near(s.y, 50.0f + 300.0f, 0.05f));

        // 右上のものは右上（+X は画面右、+Y は画面上＝スクリーン Y は小さくなる）
        XMFLOAT2 ru{};
        CHECK(WorldToScreen(vp, XMFLOAT3{1.0f, 1.0f, 0.0f}, 0.0f, 0.0f, 800.0f, 600.0f, ru));
        CHECK(ru.x > 400.0f);
        CHECK(ru.y < 300.0f);

        // カメラ後方は false（戻り値を無視すると画面の変な所にハンドルが出る）
        XMFLOAT2 behind{};
        CHECK(!WorldToScreen(vp, XMFLOAT3{0.0f, 0.0f, -20.0f}, 0.0f, 0.0f, 800.0f, 600.0f, behind));

        CHECK(Near(ScreenDistance(XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3.0f, 4.0f}), 5.0f, 1e-4f));
    }

    // ---------------------------------------------------------------
    // 7) レイ幾何（コーン角 / range / 向きのドラッグ変換の土台）
    // ---------------------------------------------------------------
    {
        // レイと平面
        float t = 0.0f;
        CHECK(RayPlane(XMFLOAT3{0.0f, 0.0f, -5.0f}, XMFLOAT3{0.0f, 0.0f, 1.0f},
                       XMFLOAT3{0.0f, 0.0f, 0.0f}, XMFLOAT3{0.0f, 0.0f, -1.0f}, t));
        CHECK(Near(t, 5.0f, 1e-4f));
        // 平面と平行 → false
        CHECK(!RayPlane(XMFLOAT3{0.0f, 1.0f, 0.0f}, XMFLOAT3{1.0f, 0.0f, 0.0f},
                        XMFLOAT3{0.0f, 0.0f, 0.0f}, XMFLOAT3{0.0f, 1.0f, 0.0f}, t));

        // 直線上でレイに最も近い点（range ハンドル）
        float lt = 0.0f;
        CHECK(ClosestParamOnLineToRay(XMFLOAT3{0.0f, 0.0f, 0.0f}, XMFLOAT3{1.0f, 0.0f, 0.0f},
                                      XMFLOAT3{3.0f, 5.0f, 0.0f}, XMFLOAT3{0.0f, -1.0f, 0.0f}, lt));
        CHECK(Near(lt, 3.0f, 1e-3f));
        // 直線とレイが平行なら false（発散防止）
        CHECK(!ClosestParamOnLineToRay(XMFLOAT3{0.0f, 0.0f, 0.0f}, XMFLOAT3{1.0f, 0.0f, 0.0f},
                                       XMFLOAT3{0.0f, 5.0f, 0.0f}, XMFLOAT3{1.0f, 0.0f, 0.0f}, lt));

        // 球へのレイ（向きハンドル）: 手前側の交点が採用される
        const XMFLOAT3 hit = DirectionFromRayOnSphere(
            XMFLOAT3{0.0f, 0.0f, 0.0f}, 2.0f,
            XMFLOAT3{0.0f, 0.0f, -10.0f}, XMFLOAT3{0.0f, 0.0f, 1.0f});
        CHECK(Near(hit.x, 0.0f, 1e-4f));
        CHECK(Near(hit.y, 0.0f, 1e-4f));
        CHECK(Near(hit.z, -1.0f, 1e-3f));

        // 外れても最近接点へフォールバック（ハンドルが画面外へ逃げても追従する）
        const XMFLOAT3 miss = DirectionFromRayOnSphere(
            XMFLOAT3{0.0f, 0.0f, 0.0f}, 2.0f,
            XMFLOAT3{0.0f, 10.0f, -10.0f}, XMFLOAT3{0.0f, 0.0f, 1.0f});
        CHECK(Near(Length3(miss), 1.0f, 1e-3f));
        CHECK(miss.y > 0.9f);
    }

    std::printf("light_math: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
