// 地形（ハイトフィールド + スカルプトブラシ）の単体テスト。
// HeightField.cpp / TerrainBrush.cpp は純ロジック（GPU も entt も vfs も不要）なので
// テスト側で直接ビルドしている。実行: ctest --output-on-failure
//
// ここで守りたい不変条件:
//   ① バイリニア高さサンプルが格子点/中点で期待どおり（座標規約が中心原点であること含む）
//   ② ブラシ適用後の高さ変化が期待どおり（中心>縁、範囲外は不変、Shift 反転、Flatten の収束）
//   ③ .hf のラウンドトリップ（保存→読み込みで 1 サンプルもズレない / 壊れた入力を弾く）
//   ④ 平坦地形へのレイキャスト交点が正しい（ブラシの当たり位置＝ここが狂うと全部狂う）

#include "terrain/HeightField.h"
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainSplatMap.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <vector>

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

namespace
{
bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
}

int main()
{
    // ================= ① サンプリング =================
    {
        // 解像度は 4 の倍数へ丸められる（Jolt HeightFieldShape のブロックサイズ制約）。
        CHECK(HeightField::NormalizeResolution(17) == 16);
        CHECK(HeightField::NormalizeResolution(128) == 128);
        CHECK(HeightField::NormalizeResolution(2) == 16);      // 下限クランプ
        CHECK(HeightField::NormalizeResolution(99999) == 512); // 上限クランプ
        CHECK(HeightField::NormalizeResolution(512) == 512);

        HeightField hf;
        hf.Create(32, 31.0f, 0.0f);            // cell = 31/(32-1) = 1.0
        CHECK(hf.IsValid());
        CHECK(hf.Resolution() == 32);
        CHECK(Near(hf.CellSize(), 1.0f));
        CHECK(Near(hf.HalfSize(), 15.5f));

        // 中心原点規約: グリッド(0,0) は (-half,-half)、最後のサンプルは (+half,+half)
        CHECK(Near(hf.GridToLocalX(0), -15.5f));
        CHECK(Near(hf.GridToLocalZ(31), 15.5f));

        // 高さ 10 / 20 の 2 点を打ってバイリニアを確認
        hf.SetAt(10, 10, 10.0f);
        hf.SetAt(11, 10, 20.0f);
        const float x10 = hf.GridToLocalX(10);
        const float z10 = hf.GridToLocalZ(10);

        CHECK(Near(hf.SampleHeight(x10, z10), 10.0f));            // 格子点そのもの
        CHECK(Near(hf.SampleHeight(x10 + 1.0f, z10), 20.0f));     // 隣の格子点
        CHECK(Near(hf.SampleHeight(x10 + 0.5f, z10), 15.0f));     // X 方向の中点
        CHECK(Near(hf.SampleHeight(x10 + 0.25f, z10), 12.5f));    // 1/4 点
        CHECK(Near(hf.SampleHeight(x10, z10 + 0.5f), 5.0f));      // Z 方向の中点（隣は 0）
        // 遠く離れた場所は影響を受けない
        CHECK(Near(hf.SampleHeight(0.0f, 0.0f), 0.0f));
        // 範囲外は端をクランプ（例外にも NaN にもしない）
        CHECK(Near(hf.SampleHeight(-9999.0f, -9999.0f), hf.At(0, 0)));

        // 平坦地形の法線は真上
        HeightField flat;
        flat.Create(16, 15.0f, 3.0f);
        const XMFLOAT3 n = flat.SampleNormal(0.0f, 0.0f);
        CHECK(Near(n.x, 0.0f) && Near(n.y, 1.0f) && Near(n.z, 0.0f));

        // 一定勾配（x が増えるほど高い）なら法線は -X 側へ傾く
        HeightField slope;
        slope.Create(16, 15.0f, 0.0f);
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x)
                slope.SetAt(x, z, static_cast<float>(x));
        const XMFLOAT3 sn = slope.SampleNormal(0.0f, 0.0f);
        CHECK(sn.x < -0.4f);          // 上り方向と逆へ倒れる
        CHECK(Near(sn.z, 0.0f));
        CHECK(sn.y > 0.0f);
    }

    // ================= ② ブラシ =================
    {
        // 重み: 中心 1、縁 0、falloff で単調
        CHECK(Near(TerrainBrushWeight(0.0f, 0.5f), 1.0f));
        CHECK(Near(TerrainBrushWeight(1.0f, 0.5f), 0.0f));
        CHECK(Near(TerrainBrushWeight(1.5f, 0.5f), 0.0f));
        CHECK(Near(TerrainBrushWeight(0.5f, 0.0f), 1.0f));   // falloff=0 は円柱
        CHECK(TerrainBrushWeight(0.5f, 1.0f) < 0.999f);      // falloff=1 は縁で落ちる
        CHECK(TerrainBrushWeight(0.25f, 1.0f) > TerrainBrushWeight(0.75f, 1.0f));

        HeightField hf;
        hf.Create(32, 31.0f, 0.0f);

        TerrainBrushParams p;
        p.type     = TerrainBrushType::Raise;
        p.radius   = 4.0f;
        p.strength = 10.0f;
        p.falloff  = 0.0f;   // 円柱（縁まで一定）＝上がる量が strength*dt ちょうどになる

        // 原点を中心に 0.1 秒ぶん盛る
        const HeightField::Rect r = ApplyTerrainBrush(hf, p, 0.0f, 0.0f, 0.1f, /*invert=*/false);
        CHECK(r.Valid());

        CHECK(Near(hf.SampleHeight(0.0f, 0.0f), 10.0f * 0.1f));   // w=1 → strength*dt
        CHECK(Near(hf.SampleHeight(2.0f, 0.0f), 10.0f * 0.1f));   // 半径内は一様
        CHECK(Near(hf.SampleHeight(10.0f, 0.0f), 0.0f));          // 半径外は一切動かない

        // dirty 矩形は円をちゃんと含む（グリッド座標 15.5 ± 4 → [11,20] を内包）
        CHECK(r.x0 <= 11 && r.x1 >= 20);
        CHECK(r.z0 <= 11 && r.z1 >= 20);

        // Shift 相当（invert）で同じ量だけ戻る
        ApplyTerrainBrush(hf, p, 0.0f, 0.0f, 0.1f, /*invert=*/true);
        CHECK(Near(hf.SampleHeight(0.0f, 0.0f), 0.0f, 1e-4f));

        // falloff=1 なら中心が一番高く、縁に向かって落ちる
        HeightField soft;
        soft.Create(32, 31.0f, 0.0f);
        TerrainBrushParams softP = p;
        softP.falloff = 1.0f;
        ApplyTerrainBrush(soft, softP, 0.0f, 0.0f, 0.1f, false);
        const float softCenter = soft.SampleHeight(0.0f, 0.0f);
        CHECK(softCenter > 0.0f);
        CHECK(softCenter <= 10.0f * 0.1f + 1e-4f);
        CHECK(soft.SampleHeight(3.0f, 0.0f) > 0.0f);
        CHECK(soft.SampleHeight(3.0f, 0.0f) < softCenter);
        CHECK(Near(soft.SampleHeight(10.0f, 0.0f), 0.0f));

        // Lower は下がる
        HeightField down;
        down.Create(32, 31.0f, 0.0f);
        TerrainBrushParams lp = p;
        lp.type = TerrainBrushType::Lower;
        ApplyTerrainBrush(down, lp, 0.0f, 0.0f, 0.1f, false);
        CHECK(down.SampleHeight(0.0f, 0.0f) < 0.0f);

        // 高さクランプが効く（何度叩いても maxHeight を超えない）
        HeightField clamped;
        clamped.Create(32, 31.0f, 0.0f);
        TerrainBrushParams cp = p;
        cp.maxHeight = 2.0f;
        for (int i = 0; i < 50; ++i)
            ApplyTerrainBrush(clamped, cp, 0.0f, 0.0f, 0.1f, false);
        CHECK(clamped.SampleHeight(0.0f, 0.0f) <= 2.0f + 1e-4f);
        CHECK(clamped.SampleHeight(0.0f, 0.0f) > 1.9f);

        // Flatten は target へ収束する
        HeightField flatten;
        flatten.Create(32, 31.0f, 8.0f);
        TerrainBrushParams fp;
        fp.type          = TerrainBrushType::Flatten;
        fp.radius        = 4.0f;
        fp.strength      = 10.0f;
        fp.falloff       = 0.0f;      // 縁まで一定に効かせる
        fp.flattenTarget = 1.0f;
        for (int i = 0; i < 60; ++i)
            ApplyTerrainBrush(flatten, fp, 0.0f, 0.0f, 0.1f, false);
        CHECK(Near(flatten.SampleHeight(0.0f, 0.0f), 1.0f, 0.05f));
        CHECK(Near(flatten.SampleHeight(10.0f, 0.0f), 8.0f));   // 範囲外は元のまま

        // Smooth はギザギザを平らへ寄せる（中央の突起が下がる）
        HeightField spike;
        spike.Create(32, 31.0f, 0.0f);
        spike.SetAt(16, 16, 10.0f);
        const float before = spike.At(16, 16);
        TerrainBrushParams sp;
        sp.type     = TerrainBrushType::Smooth;
        sp.radius   = 4.0f;
        sp.strength = 10.0f;
        sp.falloff  = 0.0f;
        ApplyTerrainBrush(spike, sp, spike.GridToLocalX(16), spike.GridToLocalZ(16), 0.1f, false);
        CHECK(spike.At(16, 16) < before);
        CHECK(spike.At(16, 16) > 0.0f);

        // ミラー: X ミラー ON なら反対側にも同じ山ができる
        HeightField mir;
        mir.Create(32, 31.0f, 0.0f);
        TerrainBrushParams mp = p;
        mp.mirrorX = true;
        ApplyTerrainBrush(mir, mp, 6.0f, 0.0f, 0.1f, false);
        CHECK(Near(mir.SampleHeight(6.0f, 0.0f), mir.SampleHeight(-6.0f, 0.0f), 1e-4f));
        CHECK(mir.SampleHeight(-6.0f, 0.0f) > 0.1f);

        // fBm は決定的（同じ引数なら同じ値）で、範囲は概ね -1..1
        const float a = TerrainFbm(12.0f, -7.0f, 0.05f, 5, 0.0f, 42u);
        const float b = TerrainFbm(12.0f, -7.0f, 0.05f, 5, 0.0f, 42u);
        CHECK(Near(a, b, 1e-7f));
        CHECK(a >= -1.0001f && a <= 1.0001f);
        CHECK(!Near(a, TerrainFbm(12.0f, -7.0f, 0.05f, 5, 0.0f, 43u), 1e-6f));  // seed で変わる

        // 熱浸食: 安息角を超えた崖が緩む（頂上が下がり、裾が上がる）
        HeightField cliff;
        cliff.Create(32, 31.0f, 0.0f);
        for (int z = 0; z < 32; ++z)
            for (int x = 16; x < 32; ++x)
                cliff.SetAt(x, z, 20.0f);
        const float topBefore  = cliff.At(20, 16);
        const float footBefore = cliff.At(15, 16);
        ApplyThermalErosion(cliff, 30.0f, 12, HeightField::Rect{});
        CHECK(cliff.At(16, 16) < 20.0f);           // 崖の肩が削れる
        CHECK(cliff.At(15, 16) > footBefore);      // 削れた土砂が裾に積もる
        CHECK(cliff.At(20, 16) <= topBefore + 1e-3f);  // 平らな頂上は増えない

        // 生成: プリセットで起伏が付く
        HeightField gen;
        gen.Create(64, 200.0f, 0.0f);
        GenerateTerrain(gen, MakeTerrainPreset(TerrainPreset::Mountains, 200.0f, 7u));
        float gmin = 0.0f, gmax = 0.0f;
        gen.MinMaxHeight(gmin, gmax);
        CHECK(gmax - gmin > 1.0f);                 // 平坦のままではない
    }

    // ================= ③ .hf ラウンドトリップ =================
    {
        HeightField src;
        src.Create(64, 123.5f, 0.0f);
        for (int z = 0; z < 64; ++z)
            for (int x = 0; x < 64; ++x)
                src.SetAt(x, z, static_cast<float>(x) * 0.25f - static_cast<float>(z) * 0.125f);

        const std::vector<dx12e::u8> bytes = src.Encode();
        CHECK(bytes.size() == HeightField::kHeaderSize + 64u * 64u * sizeof(float));

        HeightField dst;
        CHECK(dst.Decode(bytes));
        CHECK(dst.Resolution() == src.Resolution());
        CHECK(Near(dst.WorldSize(), src.WorldSize()));
        bool same = true;
        for (int z = 0; z < 64 && same; ++z)
            for (int x = 0; x < 64 && same; ++x)
                if (dst.At(x, z) != src.At(x, z)) same = false;
        CHECK(same);   // f32 をそのまま書くので誤差ゼロで一致すること

        // 壊れた入力は弾く（黙って変な地形を作らない）
        HeightField bad;
        CHECK(!bad.Decode(nullptr, 0));
        std::vector<dx12e::u8> truncated = bytes;
        truncated.resize(HeightField::kHeaderSize + 8);
        CHECK(!bad.Decode(truncated));
        std::vector<dx12e::u8> wrongMagic = bytes;
        wrongMagic[0] = 0x00;
        CHECK(!bad.Decode(wrongMagic));
    }

    // ================= ④ レイキャスト =================
    {
        HeightField flat;
        flat.Create(64, 63.0f, 5.0f);   // 高さ 5 の平坦地形、cell = 1、範囲 ±31.5

        float t = 0.0f;
        // 真上から真下へ
        CHECK(flat.Raycast(XMFLOAT3{3.0f, 25.0f, -7.0f}, XMFLOAT3{0.0f, -1.0f, 0.0f}, 1000.0f, t));
        CHECK(Near(t, 20.0f, 0.01f));   // 25 - 5 = 20

        // 斜め 45 度（xz 距離と y 落差が等しい）→ 距離は sqrt(2)*落差
        float t2 = 0.0f;
        CHECK(flat.Raycast(XMFLOAT3{0.0f, 15.0f, 0.0f}, XMFLOAT3{1.0f, -1.0f, 0.0f}, 1000.0f, t2));
        CHECK(Near(t2, 10.0f * 1.41421356f, 0.05f));

        // dir は非正規化でも同じ距離が返る（内部で正規化する契約）
        float t3 = 0.0f;
        CHECK(flat.Raycast(XMFLOAT3{3.0f, 25.0f, -7.0f}, XMFLOAT3{0.0f, -9.0f, 0.0f}, 1000.0f, t3));
        CHECK(Near(t3, 20.0f, 0.01f));

        // 上向きは当たらない
        float t4 = 0.0f;
        CHECK(!flat.Raycast(XMFLOAT3{0.0f, 25.0f, 0.0f}, XMFLOAT3{0.0f, 1.0f, 0.0f}, 1000.0f, t4));

        // フットプリントの外を通るレイは当たらない
        float t5 = 0.0f;
        CHECK(!flat.Raycast(XMFLOAT3{500.0f, 25.0f, 0.0f}, XMFLOAT3{0.0f, -1.0f, 0.0f}, 1000.0f, t5));

        // maxDistance が足りなければ当たらない
        float t6 = 0.0f;
        CHECK(!flat.Raycast(XMFLOAT3{0.0f, 25.0f, 0.0f}, XMFLOAT3{0.0f, -1.0f, 0.0f}, 5.0f, t6));

        // 山を彫った地形でも「交点の高さ = その XZ の地形高さ」になる
        HeightField bumpy;
        bumpy.Create(64, 63.0f, 0.0f);
        TerrainBrushParams p;
        p.type = TerrainBrushType::Raise;
        p.radius = 10.0f;
        p.strength = 60.0f;
        ApplyTerrainBrush(bumpy, p, 2.0f, 3.0f, 0.2f, false);

        float th = 0.0f;
        const XMFLOAT3 org{4.0f, 60.0f, 1.0f};
        const XMFLOAT3 dir{0.0f, -1.0f, 0.0f};
        CHECK(bumpy.Raycast(org, dir, 1000.0f, th));
        const float hitY = org.y - th;
        CHECK(Near(hitY, bumpy.SampleHeight(org.x, org.z), 0.01f));
        CHECK(hitY > 0.0f);   // 実際に盛り上がっている場所を撃っている
    }

    // ================= ⑤ スプラット重み（テクスチャレイヤー）=================
    {
        TerrainSplatMap sp;
        sp.Create(128);
        CHECK(sp.IsValid());
        CHECK(sp.Size() == 128);
        // 2 のべき乗へ丸まる
        {
            TerrainSplatMap odd; odd.Create(100);
            CHECK(odd.Size() == 128);
        }
        // 初期状態は全面レイヤー 0
        CHECK(sp.Pixels()[0] == 255 && sp.Pixels()[1] == 0
              && sp.Pixels()[2] == 0 && sp.Pixels()[3] == 0);

        // --- 円ブラシ: 中心は塗った層が支配的になり、合計は 255 のまま ---
        const float worldSize = 128.0f;
        const u32 v0 = sp.Version();
        const HeightField::Rect r = sp.PaintCircle(worldSize, 0.0f, 0.0f, 10.0f,
                                                   /*layer=*/2, /*strength=*/1.0f, /*falloff=*/0.0f);
        CHECK(r.Valid());
        CHECK(sp.Version() > v0);            // GPU アップロード判定用の版が上がる
        {
            const i32 cx = sp.LocalToTexelX(0.0f, worldSize);
            const i32 cz = sp.LocalToTexelZ(0.0f, worldSize);
            const size_t i = (static_cast<size_t>(cz) * sp.Size() + static_cast<size_t>(cx)) * 4;
            CHECK(sp.Pixels()[i + 2] == 255);          // レイヤー 2 が総取り
            CHECK(sp.Pixels()[i + 0] == 0);
            const int sum = sp.Pixels()[i] + sp.Pixels()[i + 1] + sp.Pixels()[i + 2] + sp.Pixels()[i + 3];
            CHECK(sum >= 254 && sum <= 256);           // 合計 255（丸め誤差 ±1）
        }
        // 円の外は不変
        {
            const size_t i = 0;   // 左上端（中心から 64m 以上離れている）
            CHECK(sp.Pixels()[i + 0] == 255 && sp.Pixels()[i + 2] == 0);
        }

        // --- .splat のラウンドトリップ（1 バイトもズレない）---
        const std::vector<u8> bytes = sp.Encode();
        CHECK(bytes.size() == TerrainSplatMap::kHeaderSize
                            + static_cast<size_t>(sp.Size()) * sp.Size() * 4);
        TerrainSplatMap back;
        CHECK(back.Decode(bytes));
        CHECK(back.Size() == sp.Size());
        CHECK(back.Pixels() == sp.Pixels());
        // 壊れた入力を弾く
        CHECK(!back.Decode(nullptr, 0));
        {
            std::vector<u8> broken = bytes;
            broken[0] ^= 0xFF;                       // マジック破壊
            TerrainSplatMap bad;
            CHECK(!bad.Decode(broken));
        }
        {
            std::vector<u8> truncated(bytes.begin(), bytes.begin() + TerrainSplatMap::kHeaderSize + 16);
            TerrainSplatMap bad;
            CHECK(!bad.Decode(truncated));           // 画素が足りない
        }

        // --- ミップチェーン（遠景のジャギ防止。GPU 側で作れないので CPU で焼く）---
        const auto mips = sp.BuildMipChain();
        CHECK(mips.size() == TerrainSplatMap::MipCount(sp.Size()));
        CHECK(mips.front().size() == sp.Pixels().size());
        CHECK(mips.back().size() == 4);              // 最終 mip は 1x1

        // --- 自動ペイント: 急斜面は岩(2)、平地は草(0) ---
        HeightField slope;
        slope.Create(64, 64.0f, 0.0f);
        {
            // x が増えるほど高くなる強い斜面を作る（右端で 60m）
            for (i32 z = 0; z < 64; ++z)
                for (i32 x = 0; x < 64; ++x)
                    slope.SetAt(x, z, (x < 32) ? 0.0f : static_cast<f32>(x - 32) * 2.0f);
        }
        TerrainSplatMap auto0;
        auto0.Create(64);
        TerrainAutoPaintParams ap;
        ap.noiseStrength = 0.0f;   // 判定を決定的にする
        auto0.AutoPaintFromHeightField(slope, ap);
        {
            // 左半分は完全に平ら → 草が支配的
            const i32 fx = auto0.LocalToTexelX(-24.0f, 64.0f);
            const i32 fz = auto0.LocalToTexelZ(0.0f, 64.0f);
            const size_t i = (static_cast<size_t>(fz) * auto0.Size() + static_cast<size_t>(fx)) * 4;
            CHECK(auto0.Pixels()[i + 0] > 200);
            // 右半分は急斜面 → 岩か雪（＝草ではない）
            const i32 sx = auto0.LocalToTexelX(20.0f, 64.0f);
            const size_t j = (static_cast<size_t>(fz) * auto0.Size() + static_cast<size_t>(sx)) * 4;
            CHECK(auto0.Pixels()[j + 0] < 128);
        }
    }

    std::printf("terrain_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
