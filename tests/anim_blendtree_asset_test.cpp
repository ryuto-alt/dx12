// .animfsm のブレンドツリー宣言（1D / 2D）が正しくパースされるかの検証。
//
// 重み計算の式そのものは blend_tree_test.cpp が見ている（Johansen の論文の
// Gradient Band Interpolation）。ここで見るのは「JSON から 2D を宣言できるか」と
// 「壊れた宣言をどう畳むか」＝配線の側。
//
// AnimGraphAsset.cpp（nlohmann を使う）をリンクするので、tests 単体構成では
// ビルドされない（CMakeLists の if(TARGET Animation) ガード）。
#include "animation/AnimGraphAsset.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// ---------------------------------------------------------------------------
// 2D ブレンドツリーの配線（パーサ）。式そのものは blend_tree_test.cpp が見ている。
// ここで見るのは「.animfsm から 2D を宣言できるか」と「壊れた宣言の扱い」。
// ---------------------------------------------------------------------------
// レイヤー0から名前でステートを引く（FindAnimState は index を返すのでラップする）
static const AnimStateDef* StateOf(const AnimGraphAsset& g, const char* name)
{
    if (g.layers.empty()) return nullptr;
    const i32 idx = FindAnimState(g.layers[0], name);
    if (idx < 0 || idx >= static_cast<i32>(g.layers[0].states.size())) return nullptr;
    return &g.layers[0].states[static_cast<size_t>(idx)];
}

static AnimGraphAsset ParseGraph(const char* jsonText, bool expectOk = true)
{
    AnimGraphAsset g;
    std::string err;
    const std::vector<uint8_t> bytes(jsonText, jsonText + std::strlen(jsonText));
    const bool ok = ParseAnimGraphAsset(bytes, g, err);
    CHECK(ok == expectOk);
    return g;
}

static void TestBlendTree2DParsed()
{
    const char* js = R"({
      "clips": [],
      "layers": [{ "name": "Base", "defaultState": "Strafe", "states": [
        { "name": "Strafe", "blendTree": {
            "type": "2d", "param": "mx", "paramY": "mz",
            "samples": [ {"value":0,"valueY":0,"clip":"Idle"},
                         {"value":0,"valueY":1,"clip":"Fwd"},
                         {"value":1,"valueY":0,"clip":"Right"} ] } }
      ], "transitions": [] }]
    })";
    AnimGraphAsset g = ParseGraph(js);
    CHECK(!g.layers.empty());
    if (g.layers.empty()) return;
    const AnimStateDef* st = StateOf(g, "Strafe");
    CHECK(st != nullptr);
    if (!st) return;
    CHECK(st->blendTree.type == AnimBlendTreeType::TwoD);
    CHECK(st->blendTree.param  == "mx");
    CHECK(st->blendTree.paramY == "mz");
    CHECK(st->blendTree.samples.size() == 3);
    // 2D は記述順を保つ（1D のような value 昇順ソートを掛けない）
    CHECK(st->blendTree.samples[1].clip == "Fwd");
    CHECK(st->blendTree.samples[1].valueY == 1.0f);
}

static void TestBlendTree2DPolarAndSpeedMatch()
{
    const char* js = R"({
      "clips": [],
      "layers": [{ "name": "Base", "defaultState": "S", "states": [
        { "name": "S", "blendTree": {
            "type": "2dPolar", "param": "mx", "paramY": "mz", "polarAlpha": 3.5,
            "speedMatch": true,
            "samples": [ {"value":0,"valueY":0,"clip":"A"}, {"value":1,"valueY":1,"clip":"B"} ] } }
      ], "transitions": [] }]
    })";
    AnimGraphAsset g = ParseGraph(js);
    const AnimStateDef* st = StateOf(g, "S");
    CHECK(st != nullptr);
    if (!st) return;
    CHECK(st->blendTree.type == AnimBlendTreeType::TwoDPolar);
    CHECK(st->blendTree.polarAlpha == 3.5f);
    // speedMatch は 1D 専用。2D で書かれても無視する（どの軸が速度か決まらないため）
    CHECK(st->blendTree.speedMatch == false);
}

static void TestBlendTree2DWithoutParamYFallsBackTo1D()
{
    // paramY が無いと全サンプルが y=0 の直線に並び、1D と区別が付かない結果になる。
    // 黙って変な絵を出すより 1D として扱う。
    const char* js = R"({
      "clips": [],
      "layers": [{ "name": "Base", "defaultState": "S", "states": [
        { "name": "S", "blendTree": {
            "type": "2d", "param": "mx",
            "samples": [ {"value":0,"clip":"A"}, {"value":1,"clip":"B"} ] } }
      ], "transitions": [] }]
    })";
    AnimGraphAsset g = ParseGraph(js);
    const AnimStateDef* st = StateOf(g, "S");
    CHECK(st != nullptr);
    if (st) CHECK(st->blendTree.type == AnimBlendTreeType::OneD);
}

static void TestBlendTreeUnknownTypeIgnored()
{
    const char* js = R"({
      "clips": [],
      "layers": [{ "name": "Base", "defaultState": "S", "states": [
        { "name": "S", "clip": "A", "blendTree": {
            "type": "3d", "param": "mx",
            "samples": [ {"value":0,"clip":"A"} ] } }
      ], "transitions": [] }]
    })";
    AnimGraphAsset g = ParseGraph(js);
    const AnimStateDef* st = StateOf(g, "S");
    CHECK(st != nullptr);
    // 未知の type はツリー無し扱い＝単一クリップのステートとして動く
    if (st) CHECK(st->blendTree.type == AnimBlendTreeType::None);
}

int main()
{
    TestBlendTree2DParsed();
    TestBlendTree2DPolarAndSpeedMatch();
    TestBlendTree2DWithoutParamYFallsBackTo1D();
    TestBlendTreeUnknownTypeIgnored();

    std::printf("AnimBlendTreeAssetTests: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
