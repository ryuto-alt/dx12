// IES (LM-63) の解析と縦角プロファイルへの焼き込みのテスト。
//   renderer/IesProfile.h はヘッダオンリー・標準ライブラリのみ。GPU 不要。
//   (A) 軸対称の最小 IES が読めて、最大 1.0 に正規化される
//   (B) 範囲外（ダウンライトの 90° より外）は 0、内側は線形補間
//   (C) 水平角が複数あるときは水平方向に平均される
//   (D) 壊れた入力（TILT 無し / データ不足 / 全部 0）は ok=false で落ちない
//   (E) 同梱の assets/engine/light/*.ies が読める（相対パスは作業ディレクトリ不問の ASSETS_DIR 経由）
#include "renderer/IesProfile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace dx12e;

namespace
{
int g_failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) { ++g_failures; std::printf("  [FAIL] %s\n", what); }
}
bool feq(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

const char* kSimple =
    "IESNA:LM-63-2002\n[TEST] t\n[LUMINAIRE] x\nTILT=NONE\n"
    "1 -1 1 5 1 1 2 0 0 0\n1.0 1.0 10.0\n"
    "0 10 20 30 90\n0\n"
    "1000 800 400 100 0\n";

const char* kTwoH =
    "IESNA:LM-63-1995\nTILT=NONE\n"
    "1 -1 1 3 2 1 2 0 0 0\n1 1 10\n"
    "0 45 90\n0 90\n"
    "100 100 100\n"      // h=0
    "300 100 0\n";       // h=90
}

int main()
{
    {
        const IesProfile p = ParseIes(kSimple);
        Check(p.ok, "simple: ok");
        Check(feq(p.maxCandela, 1000.0f), "simple: max");
        Check(feq(p.Eval(0.0f), 1.0f), "simple: eval 0 = 1.0 (normalized)");
        Check(feq(p.Eval(10.0f), 0.8f), "simple: eval 10");
        Check(feq(p.Eval(15.0f), 0.6f), "simple: linear interp 15");
        Check(feq(p.Eval(60.0f), 0.0f + (100.0f + (0.0f - 100.0f) * (30.0f / 60.0f)) / 1000.0f), "simple: interp 30..90");
        Check(feq(p.Eval(120.0f), 0.0f), "simple: beyond last angle = 0");
        float baked[181];
        p.Bake(baked, 181);
        Check(feq(baked[0], 1.0f) && feq(baked[10], 0.8f) && feq(baked[180], 0.0f), "bake: n=181 maps 1deg per sample");
    }
    {
        const IesProfile p = ParseIes(kTwoH);
        Check(p.ok, "twoH: ok");
        // 0°: (100+300)/2 = 200 が最大
        Check(feq(p.maxCandela, 200.0f), "twoH: averaged max");
        Check(feq(p.Eval(45.0f), 100.0f / 200.0f), "twoH: averaged 45deg");
        Check(feq(p.Eval(90.0f), 50.0f / 200.0f), "twoH: averaged 90deg");
    }
    {
        Check(!ParseIes("IESNA\nno tilt here\n").ok, "bad: no TILT");
        Check(!ParseIes("TILT=NONE\n1 -1 1 3 1 1 2 0 0 0\n1 1 10\n0 10\n").ok, "bad: truncated");
        const IesProfile z = ParseIes("TILT=NONE\n1 -1 1 2 1 1 2 0 0 0\n1 1 10\n0 90\n0\n0 0\n");
        Check(!z.ok, "bad: all zero");
        Check(!z.error.empty(), "bad: error message");
        // ok=false のプロファイルは Eval が 1.0（効果なし）を返す
        Check(feq(IesProfile{}.Eval(30.0f), 1.0f), "default profile is neutral");
    }
#ifdef DX12_TEST_ASSETS_DIR
    for (const char* name : {"flashlight.ies", "downlight.ies"})
    {
        std::ifstream f(std::string(DX12_TEST_ASSETS_DIR) + "engine/light/" + name, std::ios::binary);
        Check(static_cast<bool>(f), name);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        const IesProfile p = ParseIes(ss.str());
        Check(p.ok, name);
        Check(feq(p.Eval(0.0f), p.candela.front() / p.maxCandela), name);
        Check(p.Eval(0.0f) > p.Eval(40.0f), name);
    }
#endif
    if (g_failures == 0) std::printf("IesProfileTests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
