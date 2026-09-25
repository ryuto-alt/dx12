// 知覚層（dx12_perceive）の集計ロジックのテスト。
// renderer/PerceptionStats.{h,cpp} は GPU を一切触らない純関数なので、合成した画素配列で
// 指標の定義（占有率・外接矩形・周囲との輝度比・遮蔽・灯りの向き・空の割合）を固定する。
//
// ★ここが守っているもの: TS 側（perceive.ts）の言葉の境界はこの定義を前提にしている。
//   定義が黙って変わると「小さい」「暗い」「影」の判定が全部ずれるが、絵を見ないと気付けない。
//
// 実行: ctest --output-on-failure -R PerceptionStats

#include "renderer/PerceptionStats.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::perception;

namespace
{
int g_fail = 0, g_checks = 0;

void Check(bool cond, const std::string& label)
{
    ++g_checks;
    if (cond) return;
    ++g_fail;
    std::printf("  NG  %s\n", label.c_str());
}
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// 合成フレーム。背景は id=0・灰色。
struct Canvas
{
    u32 w, h;
    std::vector<u32>   ids;
    std::vector<u8>    rgba;
    std::vector<float> posDist, normal;
    Canvas(u32 W, u32 H, u8 bg) : w(W), h(H), ids(W * H, 0), rgba(W * H * 4), posDist(W * H * 4, 0.0f),
                                  normal(W * H * 4, 0.0f)
    {
        for (u32 i = 0; i < W * H; ++i) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = bg; rgba[i * 4 + 3] = 255; }
    }
    // [x0,x1) x [y0,y1) を id と色で塗る。位置は z=0 の平面上（x,y をそのままワールドへ）、
    // 法線は +z（カメラは +z 側にいる想定）、距離は dist。
    void Fill(u32 x0, u32 y0, u32 x1, u32 y1, u32 id, u8 r, u8 g, u8 b, float dist = 5.0f)
    {
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x)
            {
                const u32 p = y * w + x;
                ids[p] = id;
                rgba[p * 4] = r; rgba[p * 4 + 1] = g; rgba[p * 4 + 2] = b;
                posDist[p * 4 + 0] = static_cast<float>(x);
                posDist[p * 4 + 1] = static_cast<float>(y);
                posDist[p * 4 + 2] = 0.0f;
                posDist[p * 4 + 3] = dist;
                normal[p * 4 + 0] = 0.0f; normal[p * 4 + 1] = 0.0f; normal[p * 4 + 2] = 1.0f;
            }
    }
    PixelFrame Frame(bool withGeometry = true) const
    {
        PixelFrame f;
        f.width = w; f.height = h;
        f.ids = ids.data(); f.rgba = rgba.data();
        if (withGeometry) { f.posDist = posDist.data(); f.normal = normal.data(); }
        return f;
    }
};

// clip = [x y z 1] * M の行ベクトル規約で、ワールドの x,y をそのまま NDC にする（w=1）。
CameraInfo IdentityCamera()
{
    CameraInfo c;
    for (int i = 0; i < 16; ++i) c.viewProj[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    return c;
}

std::vector<EntityMeta> Meta(std::initializer_list<const char*> names)
{
    std::vector<EntityMeta> m(1);   // [0] は「何も無い」
    for (const char* nm : names) { EntityMeta e; e.name = nm; m.push_back(e); }
    return m;
}

void TestPixelHelpers()
{
    Check(Near(LumaOf(255, 255, 255), 1.0f), "白の Y' は 1");
    Check(Near(LumaOf(0, 0, 0), 0.0f), "黒の Y' は 0");
    Check(Near(LumaOf(0, 255, 0), 0.7152f), "緑の Y' は Rec.709 の係数 0.7152");
    Check(Near(SaturationOf(255, 0, 0), 1.0f), "純色の彩度は 1");
    Check(Near(SaturationOf(128, 128, 128), 0.0f), "灰色の彩度は 0");
    Check(Near(SaturationOf(0, 0, 0), 0.0f), "真っ黒の彩度は 0（0 除算しない）");
}

void TestShareBboxContrast()
{
    // 20x10。背景は明るい灰(200)。破片(id=1)は暗い(20) 4x4 を (8,3)〜(12,7) に置く。
    Canvas c(20, 10, 200);
    c.Fill(8, 3, 12, 7, 1, 20, 20, 20, 4.0f);
    const auto r = Analyze(c.Frame(), Meta({"shard"}), {}, {}, IdentityCamera(), Options{});

    Check(r.top.size() == 1, "見えているのは 1 体");
    const Stats& s = r.top[0];
    Check(s.name == "shard", "名前が引ける");
    Check(s.pixels == 16, "画素数 16");
    Check(Near(s.share, 16.0f / 200.0f), "share = 16/200");
    Check(Near(s.bbox[0], 8.0f / 20) && Near(s.bbox[1], 3.0f / 10) &&
          Near(s.bbox[2], 12.0f / 20) && Near(s.bbox[3], 7.0f / 10), "bbox は正規化した外接矩形（右端は +1 した排他端）");
    Check(Near(s.center[0], 10.0f / 20) && Near(s.center[1], 5.0f / 10), "重心は画素中心で取る");
    Check(Near(s.luma, 20.0f / 255.0f), "luma は Y' の平均");
    Check(Near(s.lumaStd, 0.0f), "一様な面の lumaStd は 0");
    Check(s.lumaRing.has_value() && Near(*s.lumaRing, 200.0f / 255.0f), "周囲リングは背景の Y'");
    const float expect = (20.0f / 255 + kContrastEps) / (200.0f / 255 + kContrastEps);
    Check(s.contrast.has_value() && Near(*s.contrast, expect), "contrast = (luma+0.05)/(ring+0.05)");
    Check(*s.contrast < 0.25f, "暗い破片は contrast が 1 より十分小さい（＝周囲より暗い）");
    Check(s.distance.has_value() && Near(*s.distance, 4.0f) && Near(*s.distanceMin, 4.0f), "距離は可視画素の平均と最小");
    // margin = max(2, round(0.15*4)) = 2 → 8x8 の枠 − 4x4 の矩形 = 48 画素
    Check(s.ringPixels == 48, "リングは外接矩形を 2 画素広げた枠（48 画素） 実測=" + std::to_string(s.ringPixels));
}

void TestRingExcludesSelfAndScreenEdge()
{
    // 画面の左上角に貼り付いた物: リングは画面内の分だけ。
    Canvas c(10, 10, 100);
    c.Fill(0, 0, 3, 3, 1, 250, 250, 250);
    c.Fill(3, 0, 4, 1, 1, 250, 250, 250);
    const auto r = Analyze(c.Frame(), Meta({"corner"}), {}, {}, IdentityCamera(), Options{});
    const Stats& s = r.top[0];
    // 外接矩形 [0..3]x[0..2]、margin 2 → 枠 [0..5]x[0..4]（30 画素）− 矩形 12 = 18
    Check(s.ringPixels == 18, "画面外と矩形内は数えない（18 画素） 実測=" + std::to_string(s.ringPixels));
    Check(s.contrast.has_value() && *s.contrast > 2.0f, "明るい物は contrast > 1");

    // L 字の物: 外接矩形の内側にある【自分でない】画素（明るい 250）はリングに入れない。
    // 入れてしまうと、細長い物・斜めの物ほど周囲の輝度が自分の矩形の中身で汚れる。
    Canvas l(12, 12, 100);
    l.Fill(2, 2, 6, 3, 1, 30, 30, 30);
    l.Fill(2, 3, 3, 6, 1, 30, 30, 30);
    for (u32 y = 3; y < 6; ++y)
        for (u32 x = 3; x < 6; ++x) { const u32 p = y * 12 + x; l.rgba[p * 4] = l.rgba[p * 4 + 1] = l.rgba[p * 4 + 2] = 250; }
    const Stats sl = Analyze(l.Frame(), Meta({"L"}), {}, {}, IdentityCamera(), Options{}).top[0];
    Check(sl.lumaRing.has_value() && Near(*sl.lumaRing, 100.0f / 255.0f), "外接矩形の内側の他人の画素はリングに入れない");

    // 隣に並んだ別の物（兄弟）はリングに入る＝「周囲とのコントラスト」は隣との比になる
    Canvas n(12, 6, 100);
    n.Fill(2, 1, 5, 5, 1, 30, 30, 30);
    n.Fill(5, 0, 12, 6, 2, 220, 220, 220);
    const auto rn = Analyze(n.Frame(), Meta({"a", "b"}), {}, {}, IdentityCamera(), Options{});
    for (const auto& s2 : rn.top)
        if (s2.id == 1) Check(s2.lumaRing && Near(*s2.lumaRing, 148.0f / 255.0f), "隣の明るい物がリングに入る（(12x220+18x100)/30 = 148）");
}

void TestSceneRegionsAndHistogram()
{
    // 上半分は何も描かれていない(空)、下半分は床(id=1)。左上の 1 画素だけ真っ白、残りの空は黒。
    Canvas c(8, 8, 0);
    c.Fill(0, 4, 8, 8, 1, 128, 128, 128, 10.0f);
    c.rgba[0] = c.rgba[1] = c.rgba[2] = 255;
    const auto r = Analyze(c.Frame(), Meta({"floor"}), {}, {}, IdentityCamera(), Options{});
    Check(Near(r.scene.empty, 0.5f), "空の割合 = 0.5");
    Check(Near(r.scene.top.empty, 1.0f) && Near(r.scene.bottom.empty, 0.0f), "上半分は全部空・下半分は全部描かれている");
    Check(Near(r.scene.left.empty, 0.5f) && Near(r.scene.right.empty, 0.5f), "左右は半々");
    Check(!r.scene.top.distance.has_value(), "空しか無い領域は距離なし");
    Check(r.scene.bottom.distance.has_value() && Near(*r.scene.bottom.distance, 10.0f), "下半分の平均距離");
    Check(r.scene.farthest.has_value() && Near(*r.scene.farthest, 10.0f), "最も遠い可視面");
    Check(Near(r.scene.crushed, 31.0f / 64.0f), "黒潰れ = 空の黒 31 画素");
    Check(Near(r.scene.clipped, 1.0f / 64.0f), "白飛び = 1 画素");
    Check(Near(r.scene.lumaP5, 0.0f) && Near(r.scene.lumaP95, 128.0f / 255.0f), "p5 は黒、p95 は床の灰");
    Check(r.scene.visibleEntities == 1, "見えているエンティティ数");
}

void TestTopOrderingDeterministic()
{
    Canvas c(10, 4, 50);
    c.Fill(0, 0, 2, 2, 3, 90, 90, 90);   // 4 画素
    c.Fill(2, 0, 4, 2, 1, 90, 90, 90);   // 4 画素（同数）
    c.Fill(4, 0, 10, 4, 2, 90, 90, 90);  // 24 画素
    Options o; o.top = 2;
    const auto r = Analyze(c.Frame(), Meta({"a", "b", "c"}), {}, {}, IdentityCamera(), o);
    Check(r.top.size() == 2, "top で件数を絞れる");
    Check(r.top[0].id == 2 && r.top[1].id == 1, "多い順・同数なら ID の小さい順");
}

void TestGroupAndOcclusion()
{
    // 対象 = id 1 と 2（親子）。遮蔽物が無ければ 5x4=20 画素映るはずが、id 3 が 5 画素隠している。
    Canvas c(10, 6, 120);
    c.Fill(0, 0, 5, 4, 1, 60, 60, 60);
    c.Fill(3, 2, 5, 4, 2, 60, 60, 60);    // 子（1 の一部を上書き）
    c.Fill(0, 0, 5, 1, 3, 200, 200, 200); // 手前の遮蔽物（上 1 行 = 5 画素）
    std::vector<u8> iso(10 * 6, 0);
    for (u32 y = 0; y < 4; ++y) for (u32 x = 0; x < 5; ++x) iso[y * 10 + x] = 1;
    Group g; g.name = "piece"; g.ids = {1, 2}; g.isolatedMask = iso.data();
    const auto r = Analyze(c.Frame(), Meta({"piece_a", "piece_b", "wall"}), {g}, {}, IdentityCamera(), Options{});
    Check(r.targets.size() == 1, "対象が 1 件返る");
    const Stats& s = r.targets[0];
    Check(s.transparentMembers == 0, "不透明だけのグループは transparentMembers = 0");
    Check(s.pixels == 15, "グループの画素 = 構成員の和集合（15）");
    Check(s.members == 2, "構成員数");
    Check(s.isolatedPixels.has_value() && *s.isolatedPixels == 20, "遮蔽なしの被覆 20");
    Check(s.occlusion.has_value() && Near(*s.occlusion, 0.25f), "occlusion = 1 - 15/20");
    // グループのリングにはグループ自身の画素（id 1/2）を入れない
    Check(s.lumaRing.has_value() && *s.lumaRing > 100.0f / 255.0f, "リングは自分以外の画素だけ");
}

void TestTransparentFlag()
{
    Canvas c(4, 4, 90);
    c.Fill(0, 0, 2, 2, 1, 200, 200, 200);
    c.Fill(2, 2, 4, 4, 2, 200, 200, 200);
    auto meta = Meta({"ghost", "solid"});
    meta[1].transparent = true;
    Group g; g.name = "both"; g.ids = {1, 2};
    const auto r = Analyze(c.Frame(), meta, {g}, {}, IdentityCamera(), Options{});
    Check(r.targets[0].transparentMembers == 1, "グループ内の半透明の数");
    bool ghostFlag = false, solidFlag = true;
    for (const auto& s : r.top)
    {
        if (s.id == 1) ghostFlag = (s.transparentMembers == 1);
        if (s.id == 2) solidFlag = (s.transparentMembers == 1);
    }
    Check(ghostFlag && !solidFlag, "単体の半透明フラグ");
}

void TestProjection()
{
    const CameraInfo cam = IdentityCamera();
    bool fiv = false;
    std::optional<std::array<float, 2>> ext;
    ProjectAabb(cam.viewProj, Vec3{-0.5f, -0.5f, 0}, Vec3{0.5f, 0.5f, 0}, fiv, ext);
    Check(fiv && ext && Near((*ext)[0], 0.5f) && Near((*ext)[1], 0.5f), "画面の半分の箱は全部入る・幅 0.5");
    ProjectAabb(cam.viewProj, Vec3{-2, -0.5f, 0}, Vec3{2, 0.5f, 0}, fiv, ext);
    Check(!fiv && ext && Near((*ext)[0], 2.0f), "横に 2 倍はみ出す箱は入らない・幅 2.0");
    // w = z にする射影（z<=0 はカメラの後ろ）
    CameraInfo persp = cam;
    persp.viewProj[15] = 0.0f; persp.viewProj[2 * 4 + 3] = 1.0f;
    ProjectAabb(persp.viewProj, Vec3{-1, -1, -1}, Vec3{1, 1, 1}, fiv, ext);
    Check(!fiv && !ext, "カメラの後ろに回り込む箱は投影できない（extent 無し）");
}

void TestLitFacing()
{
    // 平面(z=0, 法線 +z = カメラ側)。灯りが手前(+z)なら照らされている、奥(-z)なら影。
    Canvas c(6, 6, 100);
    c.Fill(1, 1, 5, 5, 1, 80, 80, 80);
    auto meta = Meta({"shard"});
    meta[1].hasAabb = true; meta[1].aabbMin = {1, 1, 0}; meta[1].aabbMax = {5, 5, 0};

    Light front; front.type = Light::Point; front.name = "front_lamp"; front.position = {3, 3, 2};
    front.radiance = 5.0f; front.range = 20.0f;
    Light behind = front; behind.name = "back_lamp"; behind.position = {3, 3, -2};

    auto one = [&](std::vector<Light> ls) {
        return Analyze(c.Frame(), meta, {}, ls, IdentityCamera(), Options{}).top[0];
    };
    const Stats lit = one({front});
    Check(lit.litFacing.has_value() && Near(*lit.litFacing, 1.0f), "灯りが手前なら litFacing = 1");
    Check(lit.mainLight == "front_lamp" && lit.mainLightFacing && Near(*lit.mainLightFacing, 1.0f), "主光源と向き");

    const Stats dark = one({behind});
    Check(dark.litFacing.has_value() && Near(*dark.litFacing, 0.0f), "灯りが裏なら litFacing = 0（見えている面は影）");
    Check(dark.mainLight == "back_lamp", "裏の灯りが主光源として名指しされる");

    const Stats both = one({front, behind});
    Check(both.litFacing && Near(*both.litFacing, 0.5f, 1e-3f), "同じ強さで表裏なら 0.5");

    Light far = front; far.range = 1.0f;   // 届かない
    const Stats none = one({far});
    Check(none.unlit && !none.litFacing, "どの灯りも届かなければ unlit・litFacing 無し");

    // 平行光: direction は光が進む向き。-z へ進む光は +z 向きの面を正面から照らす
    Light sun; sun.type = Light::Directional; sun.name = "sun"; sun.direction = {0, 0, -1}; sun.radiance = 1.0f;
    Check(one({sun}).litFacing && Near(*one({sun}).litFacing, 1.0f), "平行光（正面）");
    sun.direction = {0, 0, 1};
    Check(one({sun}).litFacing && Near(*one({sun}).litFacing, 0.0f), "平行光（背面）");

    // スポット: コーンの外は届かない
    Light spot = front; spot.type = Light::Spot; spot.name = "spot";
    spot.direction = {0, 0, -1}; spot.cosInner = std::cos(0.2f); spot.cosOuter = std::cos(0.4f);
    Check(one({spot}).litFacing.has_value(), "スポットが面を向いていれば届く");
    spot.direction = {1, 0, 0};
    Check(one({spot}).unlit, "スポットが横を向いていれば届かない");

    // 裏面: カメラ(+z 側)から見て法線が +z なら表、-z に裏返すと裏面が見えている＝灯りが手前でも暗い
    {
        CameraInfo cam = IdentityCamera();
        cam.position = {3, 3, 10};
        const Stats front1 = Analyze(c.Frame(), meta, {}, {front}, cam, Options{}).top[0];
        Check(front1.backFacing && Near(*front1.backFacing, 0.0f), "表が見えていれば backFacing = 0");
        Canvas flipped = c;
        for (size_t i = 0; i < flipped.normal.size(); i += 4) flipped.normal[i + 2] = -flipped.normal[i + 2];
        const Stats back1 = Analyze(flipped.Frame(), meta, {}, {front}, cam, Options{}).top[0];
        Check(back1.backFacing && Near(*back1.backFacing, 1.0f), "裏面が見えていれば backFacing = 1");
        Check(back1.litFacing && Near(*back1.litFacing, 0.0f), "裏面は手前の灯りでも litFacing = 0（フォワードと同じ理屈）");
    }

    // 位置と法線が無いフレームでは灯りの指標を出さない（嘘の 0 を返さない）
    const Stats noGeo = Analyze(c.Frame(false), meta, {}, {front}, IdentityCamera(), Options{}).top[0];
    Check(!noGeo.litFacing && !noGeo.unlit && !noGeo.distance && !noGeo.backFacing, "位置・法線が無ければ灯りと距離は無し");
}

void TestAccumulateLightMatchesShader()
{
    // Lighting.hlsli の減衰 saturate(1-d/range)^2 と同じか（d=5, range=10 → 0.25）
    Light L; L.type = Light::Point; L.position = {0, 0, 5}; L.radiance = 2.0f; L.range = 10.0f;
    float fr = 0, bk = 0;
    const float c = AccumulateLight(L, Vec3{0, 0, 0}, Vec3{0, 0, 1}, fr, bk);
    Check(Near(c, 2.0f * 0.25f) && Near(fr, 0.5f) && Near(bk, 0.0f), "点光源の減衰はシェーダと同じ式");
}
} // namespace

int main()
{
    std::printf("知覚層の集計（PerceptionStats）\n");
    TestPixelHelpers();
    TestShareBboxContrast();
    TestRingExcludesSelfAndScreenEdge();
    TestSceneRegionsAndHistogram();
    TestTopOrderingDeterministic();
    TestGroupAndOcclusion();
    TestTransparentFlag();
    TestProjection();
    TestLitFacing();
    TestAccumulateLightMatchesShader();
    std::printf("%s: %d checks / %d failures\n", g_fail == 0 ? "OK" : "NG", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
