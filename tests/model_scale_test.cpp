// モデルのインポートサイズ回帰テスト。
//
// 守っているもの: ModelLoader が FBX の UnitScaleFactor(既定 cm)をメートルへ正規化すること
// (= aiProcess_GlobalScale)。これを外すと Blender/Maya/3ds Max の既定書き出し FBX が
// **100 倍**で読み込まれる。glTF/OBJ は元から等倍なので、同じフラグで壊れないことも一緒に見張る。
//
// 素材(tests/data/)は Blender 5.2 で書き出した 1m 立方体:
//   cube1m_blender_default.fbx  既定設定(ノード変換に 100 倍が乗る cm ファイル)
//   cube2m_nodescale.fbx        同上 + オブジェクトスケール2 → 2m
//   cube1m_skinned.fbx          アーマチュア + アニメーション付き 1m
//   cube1m.glb / cube1m.obj     等倍の対照群
#include "resource/ModelLoader.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
int g_failures = 0;

void ExpectSize(const char* file, float expected)
{
    const std::string path = std::string(DX12E_TEST_DATA_DIR) + "/" + file;
    const dx12e::ModelProbeInfo info = dx12e::ModelLoader::Probe(path);
    if (!info.ok)
    {
        std::printf("[FAIL] %s: probe failed (%s)\n", file, info.error.c_str());
        ++g_failures;
        return;
    }

    const float sx = info.aabbMax[0] - info.aabbMin[0];
    const float sy = info.aabbMax[1] - info.aabbMin[1];
    const float sz = info.aabbMax[2] - info.aabbMin[2];
    const float tol = expected * 0.01f;   // 1%

    if (std::fabs(sx - expected) > tol || std::fabs(sy - expected) > tol ||
        std::fabs(sz - expected) > tol)
    {
        std::printf("[FAIL] %s: expected %.3fm cube, got (%.3f, %.3f, %.3f)  ratio=%.2fx\n",
                    file, expected, sx, sy, sz, sx / expected);
        ++g_failures;
        return;
    }
    std::printf("[ ok ] %s: %.3f x %.3f x %.3f m\n", file, sx, sy, sz);
}
} // namespace

int main()
{
    ExpectSize("cube1m_blender_default.fbx", 1.0f);   // 修正前は 100.0 になっていた
    ExpectSize("cube2m_nodescale.fbx",       2.0f);   // 修正前は 200.0
    ExpectSize("cube1m_skinned.fbx",         1.0f);   // スキン付き(実描画も 1m になる)
    ExpectSize("cube1m.glb",                 1.0f);   // 単位正規化が no-op であることの確認
    ExpectSize("cube1m.obj",                 1.0f);   // 同上

    if (g_failures != 0)
    {
        std::printf("model_scale_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("model_scale_test: all passed\n");
    return 0;
}
