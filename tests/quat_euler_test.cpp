// QuaternionToEulerDegrees の往復テスト。
//
// ★何を守っているか: シーン JSON は position/rotation/scale しか持たず、
//   quaternion / useQuaternion は実行時専有。MCP で quaternion だけ書くと
//   保存時に古い euler が書かれて姿勢が戻る（無音のデータ損失）ので、
//   書いた側で euler を揃える。その変換が **GetWorldMatrix と同じ規約**
//   （XMMatrixRotationRollPitchYaw(pitch=x, yaw=y, roll=z)）で解けていることを固定する。
//   規約を取り違えると「保存すると少しだけ姿勢がずれる」という一番気づきにくい形で出る。
//
// 判定は euler の一致ではなく**回転そのものの一致**で行う（euler は表現が一意でないため）。
// クォータニオン q と -q は同じ回転なので |dot| を見る。
#include "ecs/Components.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;
using namespace DirectX;

namespace
{
int g_checks = 0;
int g_failures = 0;

// q を euler へ落として組み直したものが、元と同じ回転か
void ExpectRoundTrip(float pitchDeg, float yawDeg, float rollDeg, const char* label)
{
    ++g_checks;
    const XMVECTOR q0 = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(pitchDeg), XMConvertToRadians(yawDeg), XMConvertToRadians(rollDeg));
    XMFLOAT4 q0f{};
    XMStoreFloat4(&q0f, q0);

    const XMFLOAT3 e = QuaternionToEulerDegrees(q0f);
    const XMVECTOR q1 = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(e.x), XMConvertToRadians(e.y), XMConvertToRadians(e.z));

    const float d = std::fabs(XMVectorGetX(XMQuaternionDot(q0, q1)));
    if (d > 0.9995f) return;
    ++g_failures;
    std::printf("FAIL %-22s in=(%.1f,%.1f,%.1f) out=(%.1f,%.1f,%.1f) |dot|=%.5f\n",
                label, pitchDeg, yawDeg, rollDeg, e.x, e.y, e.z, d);
}
}

int main()
{
    ExpectRoundTrip(0, 0, 0, "identity");
    ExpectRoundTrip(30, 0, 0, "pitch only");
    ExpectRoundTrip(0, 45, 0, "yaw only");
    ExpectRoundTrip(0, 0, 60, "roll only");
    ExpectRoundTrip(30, 45, 60, "mixed");
    ExpectRoundTrip(-30, -45, -60, "mixed negative");
    ExpectRoundTrip(10, 170, -20, "yaw near 180");
    ExpectRoundTrip(80, 20, 35, "pitch near lock");
    ExpectRoundTrip(-80, -20, -35, "pitch near lock neg");
    // ★ジンバルロック。euler の表現は変わるが**回転は同じ**であることを見る
    ExpectRoundTrip(90, 40, 25, "gimbal lock +90");
    ExpectRoundTrip(-90, 40, 25, "gimbal lock -90");

    // 決定的な疑似乱数で広く舐める（規約違いは特定の象限だけで出ることがある）
    unsigned s = 12345u;
    const auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; };
    for (int i = 0; i < 500; ++i)
    {
        const float p = rnd() * 178.0f - 89.0f;    // ロック点はピンポイントで上に入れてある
        const float y = rnd() * 360.0f - 180.0f;
        const float r = rnd() * 360.0f - 180.0f;
        ExpectRoundTrip(p, y, r, "random");
    }

    if (g_failures != 0)
    {
        std::printf("quat_euler: %d checks, %d failure(s)\n", g_checks, g_failures);
        return 1;
    }
    std::printf("quat_euler: %d checks, all passed\n", g_checks);
    return 0;
}
