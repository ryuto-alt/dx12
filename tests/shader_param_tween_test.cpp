// カスタムシェーダーの「名前付きパラメーター」の解決とトゥイーンのテスト。
//
// なに:
//   Trigger の SetShaderParam / AnimShaderParam が依存する 2 つの土台を GPU 抜きで検証する。
//     1) 名前 → 書き込み先スロットの解決（scene/ShaderParamTween.h）
//        メッシュ用（MeshRenderer の自由枠 float 8 個）と
//        画面用（CameraComponent::screenShaderParams の float 4 個）で行き先が変わる。
//     2) トゥイーンの進行（開始値の即時反映・終端の厳密一致・再発火での差し替え・対象消滅）
//
// なぜ:
//   ここは「オフセット → 添字」の算術と「どのコンポーネントへ書くか」の場合分けで、
//   1 つずれても静かに隣のスロットを壊す（別のパラメーターが勝手に動く）。しかも症状が
//   出るのは Play 中の見た目だけなので、目視では気づけない。
//   リフレクションは DXC が要るが、ここが検証したいのは【リフレクション結果の使い方】なので、
//   Param を手で組んでストアへ積む＝コンパイラもデバイスも要らない。
//
// 実行: ctest --output-on-failure  （失敗があれば終了コード 1）

#include "scene/ShaderParamTween.h"
#include "resource/ShaderParams.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // MeshRenderer が Mesh* を持つ（完全型が要る）

#include <entt/entt.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f * (1.0f + std::fabs(a) + std::fabs(b));
}
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_F(a, b)  CHECK(feq((a), (b)))

namespace
{

shaderparams::Param MakeParam(const char* name, u32 offset, shaderparams::Space space,
                              shaderparams::Kind kind = shaderparams::Kind::Float)
{
    shaderparams::Param p;
    p.name     = name;
    p.typeName = "float";
    p.offset   = offset;
    p.space    = space;
    p.kind     = kind;
    return p;
}

// リフレクションの代わりに、既知のレイアウトを手でストアへ積む。
// キーは shaderdiag::NormalizeKey 済みの相対パス（小文字・'/' 区切り）。
void SeedMeshShader(const char* key)
{
    std::vector<shaderparams::Param> ps;
    // float _Glow;  float3 _Tint;  float4 shaderParams;  の並び（8 個ぶん）
    ps.push_back(MakeParam("_Glow", shaderparams::kMeshFreeBegin + 0, shaderparams::Space::MeshObject));
    ps.push_back(MakeParam("_Tint", shaderparams::kMeshFreeBegin + 4, shaderparams::Space::MeshObject,
                           shaderparams::Kind::Color3));
    ps.push_back(MakeParam("_Last", shaderparams::kMeshFreeBegin + 28, shaderparams::Space::MeshObject));
    shaderparams::ShaderInfo info;
    info.params = std::move(ps);
    shaderparams::Set(key, std::move(info));
}

void SeedScreenShader(const char* key)
{
    std::vector<shaderparams::Param> ps;
    ps.push_back(MakeParam("_Flash", shaderparams::kScreenFreeBegin + 0, shaderparams::Space::Screen));
    ps.push_back(MakeParam("_Tail",  shaderparams::kScreenFreeBegin + 12, shaderparams::Space::Screen));
    shaderparams::ShaderInfo info;
    info.params = std::move(ps);
    shaderparams::Set(key, std::move(info));
}

// ---- 1) 名前 → スロットの解決 ----
void TestResolution()
{
    entt::registry reg;
    SeedMeshShader("fx/glow.hlsl");
    SeedScreenShader("fx/flash.hlsl");

    const entt::entity mesh = reg.create();
    auto& mr = reg.emplace<MeshRenderer>(mesh);
    mr.shaderPath = "fx/Glow.hlsl";   // ★大文字混じり。NormalizeKey で引けること自体の確認

    const std::vector<shaderparams::Param> listed = ListShaderParams(reg, mesh);
    CHECK(listed.size() == 3);

    shaderparams::Param p;
    CHECK(FindShaderParam(reg, mesh, "_Glow", p));
    CHECK(p.Index() == 0);
    CHECK(p.space == shaderparams::Space::MeshObject);

    CHECK(FindShaderParam(reg, mesh, "_Tint", p));
    CHECK(p.Index() == 1);
    CHECK(FindShaderParam(reg, mesh, "_Last", p));
    CHECK(p.Index() == 7);   // 自由枠の最後（8 個目）
    CHECK(!FindShaderParam(reg, mesh, "_NoSuchName", p));

    // 書き込み先が正しいスロットか。★_Glow は effectValue、_Last は shaderParams.w に載る。
    CHECK(FindShaderParam(reg, mesh, "_Glow", p));
    CHECK(WriteShaderParam(reg, mesh, p, 0.25f));
    CHECK_F(mr.effectValue, 0.25f);
    CHECK_F(mr.shaderParams.w, 0.0f);

    CHECK(FindShaderParam(reg, mesh, "_Last", p));
    CHECK(WriteShaderParam(reg, mesh, p, 0.75f));
    CHECK_F(mr.shaderParams.w, 0.75f);
    CHECK_F(mr.effectValue, 0.25f);   // 隣を壊していない

    // _Tint は旧 _pad の先頭（shaderParamsB.x）
    CHECK(FindShaderParam(reg, mesh, "_Tint", p));
    CHECK(WriteShaderParam(reg, mesh, p, 0.5f));
    CHECK_F(mr.shaderParamsB.x, 0.5f);

    // ---- 画面シェーダーは CameraComponent へ行く ----
    const entt::entity cam = reg.create();
    auto& cc = reg.emplace<CameraComponent>(cam);
    cc.screenShaderPath = "fx/flash.hlsl";

    CHECK(ListShaderParams(reg, cam).size() == 2);
    CHECK(FindShaderParam(reg, cam, "_Flash", p));
    CHECK(p.space == shaderparams::Space::Screen);
    CHECK(p.Index() == 0);
    CHECK(WriteShaderParam(reg, cam, p, 1.0f));
    CHECK_F(cc.screenShaderParams.x, 1.0f);

    CHECK(FindShaderParam(reg, cam, "_Tail", p));
    CHECK(p.Index() == 3);
    CHECK(WriteShaderParam(reg, cam, p, 0.3f));
    CHECK_F(cc.screenShaderParams.w, 0.3f);

    // シェーダー未割当のエンティティからは何も引けない
    const entt::entity bare = reg.create();
    reg.emplace<MeshRenderer>(bare);
    CHECK(ListShaderParams(reg, bare).empty());
    CHECK(!FindShaderParam(reg, bare, "_Glow", p));
}

// ---- 2) トゥイーンの進行 ----
void TestTween()
{
    entt::registry reg;
    SeedMeshShader("fx/glow.hlsl");

    const entt::entity e = reg.create();
    auto& mr = reg.emplace<MeshRenderer>(e);
    mr.shaderPath = "fx/glow.hlsl";

    ShaderParamTweens tw;

    // 発火した瞬間に開始値が入る（1 フレーム遅れて光り始めない）
    CHECK(tw.Start(reg, e, "_Glow", 1.0f, 0.0f, 1.0f, ShaderTweenEase::Linear));
    CHECK_F(mr.effectValue, 1.0f);
    CHECK(tw.ActiveCount() == 1);

    tw.Update(reg, 0.25f);
    CHECK_F(mr.effectValue, 0.75f);
    tw.Update(reg, 0.25f);
    CHECK_F(mr.effectValue, 0.5f);

    // ★終端はイージングの誤差を残さず目標値そのものになること
    tw.Update(reg, 0.6f);
    CHECK(mr.effectValue == 0.0f);
    CHECK(tw.ActiveCount() == 0);

    // 秒数 0 は即代入と同じ（積まれない）
    CHECK(tw.Start(reg, e, "_Glow", 1.0f, 0.4f, 0.0f, ShaderTweenEase::Linear));
    CHECK_F(mr.effectValue, 0.4f);
    CHECK(tw.ActiveCount() == 0);

    // 同じスロットへの再発火は差し替え（二重に動かない）
    CHECK(tw.Start(reg, e, "_Glow", 1.0f, 0.0f, 1.0f, ShaderTweenEase::Linear));
    tw.Update(reg, 0.5f);
    CHECK_F(mr.effectValue, 0.5f);
    CHECK(tw.Start(reg, e, "_Glow", 1.0f, 0.0f, 1.0f, ShaderTweenEase::Linear));
    CHECK(tw.ActiveCount() == 1);
    CHECK_F(mr.effectValue, 1.0f);   // 前のは打ち切られ、開始値から引き直し

    // 別スロットは独立して並走する
    CHECK(tw.Start(reg, e, "_Last", 0.0f, 1.0f, 1.0f, ShaderTweenEase::Linear));
    CHECK(tw.ActiveCount() == 2);
    tw.Update(reg, 0.5f);
    CHECK_F(mr.effectValue, 0.5f);
    CHECK_F(mr.shaderParams.w, 0.5f);

    // 名前が引けなければ何も積まない
    CHECK(!tw.Start(reg, e, "_NoSuchName", 0.0f, 1.0f, 1.0f, ShaderTweenEase::Linear));
    CHECK(tw.ActiveCount() == 2);

    // ★Stay（毎フレーム発火）: 同じ指示で積み直さず走り続けること。
    //   これが無いと毎フレーム進行がリセットされ、値が開始値に張り付いて目標へ着かない。
    {
        ShaderParamTweens stay;
        CHECK(stay.Start(reg, e, "_Glow", 0.0f, 1.0f, 1.0f, ShaderTweenEase::Linear, true));
        for (int i = 0; i < 4; ++i)
        {
            // 毎フレーム同じ指示が飛んでくる状況を再現する
            CHECK(stay.Start(reg, e, "_Glow", 0.0f, 1.0f, 1.0f, ShaderTweenEase::Linear, true));
            stay.Update(reg, 0.125f);
        }
        CHECK(stay.ActiveCount() == 1);
        CHECK_F(mr.effectValue, 0.5f);   // 0.125 * 4 ぶん進んでいる（張り付いていない）

        // 指示が変わったら積み直す
        CHECK(stay.Start(reg, e, "_Glow", 0.0f, 0.25f, 1.0f, ShaderTweenEase::Linear, true));
        CHECK(stay.ActiveCount() == 1);
        CHECK_F(mr.effectValue, 0.0f);   // 新しい開始値から
    }
    mr.effectValue = 0.5f;   // 上のブロックで書き換えたので元の並走テストの状態へ戻す

    // 即代入は進行中を打ち切る
    CHECK(tw.SetNow(reg, e, "_Glow", 0.125f));
    CHECK(tw.ActiveCount() == 1);
    CHECK_F(mr.effectValue, 0.125f);
    tw.Update(reg, 1.0f);
    CHECK_F(mr.effectValue, 0.125f);   // 打ち切ったので動かない

    // ★対象が消えても落ちずに畳まれること（Destroy アクションと同居しうる）
    CHECK(tw.Start(reg, e, "_Glow", 0.0f, 1.0f, 1.0f, ShaderTweenEase::Linear));
    CHECK(tw.ActiveCount() == 1);
    reg.destroy(e);
    tw.Update(reg, 0.5f);
    CHECK(tw.ActiveCount() == 0);
}

// ---- 3) イージング ----
void TestEasing()
{
    entt::registry reg;
    SeedMeshShader("fx/glow.hlsl");

    auto midpointOf = [&](ShaderTweenEase ease)
    {
        const entt::entity e = reg.create();
        auto& mr = reg.emplace<MeshRenderer>(e);
        mr.shaderPath = "fx/glow.hlsl";

        ShaderParamTweens tw;
        tw.Start(reg, e, "_Glow", 0.0f, 1.0f, 1.0f, ease);
        tw.Update(reg, 0.5f);
        return mr.effectValue;
    };

    CHECK_F(midpointOf(ShaderTweenEase::Linear), 0.5f);
    CHECK_F(midpointOf(ShaderTweenEase::Out),    0.75f);   // 先に進む＝引きが速い
    CHECK_F(midpointOf(ShaderTweenEase::In),     0.25f);   // 後から効く
    CHECK_F(midpointOf(ShaderTweenEase::InOut),  0.5f);    // 中点は対称
}

} // namespace

int main()
{
    TestResolution();
    TestTween();
    TestEasing();

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
