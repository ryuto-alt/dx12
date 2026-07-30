#pragma once

#include "scene/Scene.h"
#include "core/Types.h"

#include <cstddef>
#include <string>
#include <type_traits>

namespace dx12e
{

// シーン設定（ポスト/SSAO/SSR/SSGI/TAA/影/RT/DDGI/フォグ/空/デカール）の指紋。
//
// なぜ要るか: これらを書き換えるエディタの窓は 20 箇所以上あり、どれも Undo を積まない。
// 1 箇所ずつフックすると必ず取りこぼす（取りこぼし = 未保存の変更が黙って消える）ので、
// 設定そのものを比較して未保存判定に使う。
//
// 中身の意味は問わない。「前と違うか」だけ分かればよいので FNV-1a。
inline u64 SceneSettingsFingerprint(const Scene& scene)
{
    u64 v = 1469598103934665603ull;
    auto mix = [&v](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { v ^= b[i]; v *= 1099511628211ull; }
    };
    // ponytail: trivially copyable なものは構造体ごとバイト列で混ぜる。パディングまで
    // 見るので理論上は偽陽性がありうるが、偽陽性＝「保存しますか」と余計に聞くだけ。
    // 偽陰性（変更を見落として黙って捨てる）だけは作らない。
    auto pod = [&mix](const auto& s) {
        static_assert(std::is_trivially_copyable_v<std::decay_t<decltype(s)>>,
                      "trivially copyable でない設定はここでは扱えない（下の post/skybox のように個別に混ぜる）");
        mix(&s, sizeof(s));
    };

    // PostProcessSettings は std::string(lutPath) を持つので、既存の名前表(X マクロ)を
    // 回してフィールド単位で混ぜる。フィールドを足しても表を直せば自動で追随する。
    {
        const PostProcessSettings& pp = scene.GetPostSettings();
#define DX12E_PPH_B(n) mix(&pp.n, sizeof(pp.n));
#define DX12E_PPH_F(n) mix(&pp.n, sizeof(pp.n));
#define DX12E_PPH_I(n) mix(&pp.n, sizeof(pp.n));
#define DX12E_PPH_V(n) mix(&pp.n, sizeof(pp.n));
#define DX12E_PPH_S(n) mix(pp.n.data(), pp.n.size());
        DX12E_POST_FIELDS(DX12E_PPH_B, DX12E_PPH_F, DX12E_PPH_I, DX12E_PPH_V, DX12E_PPH_S)
#undef DX12E_PPH_B
#undef DX12E_PPH_F
#undef DX12E_PPH_I
#undef DX12E_PPH_V
#undef DX12E_PPH_S
    }

    pod(scene.GetSSAOSettings());
    pod(scene.GetContactShadowSettings());
    pod(scene.GetShadowPcssSettings());
    pod(scene.GetSsrSettings());
    pod(scene.GetSsgiSettings());
    pod(scene.GetRtSettings());
    pod(scene.GetDdgiSettings());
    pod(scene.GetTaaSettings());
    pod(scene.GetVolumetricFogSettings());

    // SkyboxSettings は std::string を持つので個別に。
    const auto& sky = scene.GetSkyboxSettings();
    mix(sky.envMapPath.data(), sky.envMapPath.size());
    mix(&sky.iblIntensity, sizeof(sky.iblIntensity));
    mix(&sky.skyboxIntensity, sizeof(sky.skyboxIntensity));
    mix(&sky.drawSkybox, sizeof(sky.drawSkybox));

    const std::string& decal = scene.GetDecalAtlasPath();
    mix(decal.data(), decal.size());

    // ★「このシーンで影を描く」も混ぜる。シリアライズはされているのにここに無かったので、
    //   トグルしても未保存扱いにならず、タイトルの * も保存確認ダイアログも出ず、
    //   自動保存も走らないまま**次にシーンを開くと元に戻っていた**
    //   （このチェックボックスは Undo も積まないので、指紋だけが検知経路）。
    const bool shadows = scene.GetShadowsEnabled();
    mix(&shadows, sizeof(shadows));
    return v;
}

} // namespace dx12e
