#include "scripting/ScriptAudioBindings.h"

#include "audio/AudioSystem.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4244 4267 4996)
// ★ScriptEngine.cpp と同じ設定にすること（sol2 の設定マクロが TU ごとに違うと ODR 違反になる）
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#pragma warning(pop)

#include <DirectXMath.h>
#include <string>
#include <tuple>

namespace dx12e
{

namespace
{
// opts.pos を読む。Vec3 でも {x=,y=,z=} でも {1,2,3} でもよい。無ければ false。
bool ReadPos(const sol::table& opts, float out[3])
{
    sol::object p = opts["pos"];
    if (p.is<DirectX::XMFLOAT3>())
    {
        const auto v = p.as<DirectX::XMFLOAT3>();
        out[0] = v.x; out[1] = v.y; out[2] = v.z;
        return true;
    }
    if (p.is<sol::table>())
    {
        sol::table t = p.as<sol::table>();
        out[0] = t.get_or("x", t.get_or(1, 0.0f));
        out[1] = t.get_or("y", t.get_or(2, 0.0f));
        out[2] = t.get_or("z", t.get_or(3, 0.0f));
        return true;
    }
    return false;
}
} // namespace

void RegisterAudioBindings(sol::state& lua)
{
    lua.new_usertype<AudioSystem>("AudioSystem",
        // メンバ関数ポインタ直バインドだと C++ 側のデフォルト引数(loop)が効かず
        // 1引数呼びでエラーになるので、sol::optional で loop を省略可にする
        "playBGM",         [](AudioSystem& a, const std::string& path, sol::optional<bool> loop) {
                               a.PlayBGM(path, loop.value_or(true));
                           },
        "stopBGM",         &AudioSystem::StopBGM,
        "pauseBGM",        &AudioSystem::PauseBGM,
        "resumeBGM",       &AudioSystem::ResumeBGM,
        "seekBGM",         &AudioSystem::SeekBGM,
        "setBGMRate",      &AudioSystem::SetBGMRate,
        "setListener",     &AudioSystem::SetListenerPos,
        // ★vol を受け取って渡す。以前は引数を取らず C++ 既定の 1.0 が常に勝っていたので、
        //   `audio:playSFX("hit.wav", false, 0.2)` はエラーも警告も無しに全音量で鳴っていた
        //   （sol2 は余った引数を黙って捨てる）。すぐ下の playSpatial には vol があるので、
        //   2D だけ音量を下げられない状態だった。
        "playSFX",         [](AudioSystem& a, const std::string& path, sol::optional<bool> loop,
                              sol::optional<float> vol) {
                               a.PlaySFX(path, loop.value_or(false), vol.value_or(1.0f));
                           },
        "playSpatial",     [](AudioSystem& a, const std::string& path, float x, float y, float z,
                              float minD, float maxD, sol::optional<float> vol, sol::optional<bool> loop) {
                               a.PlaySFXSpatial(path, x, y, z, minD, maxD,
                                                vol.value_or(1.0f), loop.value_or(false));
                           },
        "stopAllSFX",      &AudioSystem::StopAllSFX,
        // ---- 鳴っている 1 本を掴んで操作する（環境音・機械の唸り・スピンダウン用）----
        // ★playSFX / playSpatial は「撃ちっぱなし」なので、ループ再生した音を止められない。
        //   止めるのに stopAllSFX しか無いと、他の音まで巻き添えで消える。
        //   下の 3 つは ID を返す版の再生と、その ID への操作。
        //   ID は世代つきなので、鳴り終わってスロットが使い回された後の ID は無視される
        //   （古い ID で他人の音を止めてしまう事故が起きない）。
        "playSFXId",       [](AudioSystem& a, const std::string& path, sol::optional<bool> loop,
                              sol::optional<float> vol) {
                               return a.PlaySFXTracked(path, loop.value_or(false), vol.value_or(1.0f));
                           },
        "playSpatialId",   [](AudioSystem& a, const std::string& path, float x, float y, float z,
                              float minD, float maxD, sol::optional<float> vol, sol::optional<bool> loop) {
                               return a.PlaySFXSpatial(path, x, y, z, minD, maxD,
                                                       vol.value_or(1.0f), loop.value_or(false));
                           },
        // ---- 汎用の再生口（バス指定つき）。オプションは全部省略可 ----
        //   audio:play("audio/se/door.wav", { bus="sfx", volume=0.8, loop=false, priority=128,
        //                                      pitch=1, pos=Vec3.new(x,y,z), minDistance=1, maxDistance=30 })
        //   pos を渡すと 3D 空間音、渡さなければ 2D。戻り値は ID（失敗 -1）。
        //   priority は 0..255（大きいほど大事）。上限に達したら低い方から仮想化/停止される。
        "play",            [](AudioSystem& a, const std::string& path, sol::optional<sol::table> opts) {
                               AudioSystem::PlayParams p;
                               p.path = path;
                               if (opts)
                               {
                                   const sol::table& o = *opts;
                                   p.bus         = o.get_or("bus", std::string());
                                   p.volume      = o.get_or("volume", 1.0f);
                                   p.pitch       = o.get_or("pitch", 1.0f);
                                   p.loop        = o.get_or("loop", false);
                                   p.priority    = o.get_or("priority", AudioSystem::kDefaultPriority);
                                   p.minDistance = o.get_or("minDistance", 1.0f);
                                   p.maxDistance = o.get_or("maxDistance", 30.0f);
                                   p.reverb      = o.get_or("reverb", 1.0f);
                                   p.spatial     = ReadPos(o, p.pos);
                               }
                               return a.Play(p);
                           },
        "moveVoice",       &AudioSystem::UpdateSpatialEmitter,
        // fade 秒で音量を 0 まで下げてから止める（省略/0 = 即停止）
        "stopVoice",       [](AudioSystem& a, int id, sol::optional<float> fade) {
                               a.StopVoice(id, fade.value_or(0.0f));
                           },
        "setVoicePriority", &AudioSystem::SetVoicePriority,
        "setVoiceVolume",  &AudioSystem::SetVoiceVolume,
        "setVoicePitch",   &AudioSystem::SetVoicePitch,
        "isVoicePlaying",  &AudioSystem::IsVoicePlaying,
        "setMasterVolume",  &AudioSystem::SetMasterVolume,
        "setBGMVolume",     &AudioSystem::SetBGMVolume,
        "setSFXVolume",     &AudioSystem::SetSFXVolume,
        "getMasterVolume",  &AudioSystem::GetMasterVolume,
        "getBGMVolume",     &AudioSystem::GetBGMVolume,
        "getSFXVolume",     &AudioSystem::GetSFXVolume,
        // ---- バス（サブミックス）----
        // 既定は master ← music / sfx / ambience / voice / ui。createBus で足せる。
        // setMasterVolume/setBGMVolume/setSFXVolume は master/music/sfx の別名。
        "createBus",        [](AudioSystem& a, const std::string& name, sol::optional<std::string> parent) {
                                return a.CreateBus(name, parent.value_or(std::string()));
                            },
        "setBusVolume",     &AudioSystem::SetBusVolume,
        "getBusVolume",     &AudioSystem::GetBusVolume,
        "setBusMute",       &AudioSystem::SetBusMute,
        "isBusMuted",       &AudioSystem::IsBusMuted,
        "setBusLowpass",    &AudioSystem::SetBusLowpass,
        "getBusLowpass",    &AudioSystem::GetBusLowpass,
        // ---- 同時発音数 ----
        // 実ボイス（本当に XAudio2 で鳴らす本数）の上限。超えた分は優先度の低い順に
        // 仮想ボイス（音は出さず位置だけ進める）へ落ちる。既定 32。
        "setMaxVoices",     &AudioSystem::SetMaxVoices,
        "getMaxVoices",     &AudioSystem::GetMaxVoices,
        "setBusVoiceLimit", &AudioSystem::SetBusVoiceLimit,
        "getBusVoiceLimit", &AudioSystem::GetBusVoiceLimit,
        "getVoiceCount",    [](AudioSystem& a) {
                                u32 real = 0, virt = 0;
                                a.GetVoiceCounts(real, virt);
                                return std::make_tuple(real, virt);
                            },
        // ---- スナップショット ----
        //   audio:defineSnapshot("chase", { music = {volume=1.0}, ambience = {volume=0.3},
        //                                   sfx = {lowpass=1200} })
        //   audio:setSnapshot("chase", 1.5)   -- 1.5 秒かけて遷移
        // 値はバスのユーザー音量に掛ける補正（volume は線形 0..4、db= でも書ける。lowpass は Hz、0 = 無し）。
        "defineSnapshot",   [](AudioSystem& a, const std::string& name, sol::table buses) {
                                std::vector<AudioSystem::SnapshotBus> list;
                                for (auto& kv : buses)
                                {
                                    if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
                                    sol::table t = kv.second.as<sol::table>();
                                    AudioSystem::SnapshotBus sb;
                                    sb.bus = kv.first.as<std::string>();
                                    sb.mod.gain = t.get_or("volume", 1.0f);
                                    if (sol::optional<float> db = t["db"]) sb.mod.gain = audio::DbToLinear(*db);
                                    sb.mod.lowpassHz = t.get_or("lowpass", 0.0f);
                                    list.push_back(std::move(sb));
                                }
                                return a.DefineSnapshot(name, std::move(list));
                            },
        "setSnapshot",      [](AudioSystem& a, const std::string& name, sol::optional<float> sec) {
                                return a.SetSnapshot(name, sec.value_or(0.5f));
                            },
        "getSnapshot",      &AudioSystem::GetSnapshot,
        "getSnapshots",     [](AudioSystem& a, sol::this_state ts) {
                                sol::state_view lv(ts);
                                sol::table t = lv.create_table();
                                int i = 1;
                                for (const auto& n : a.GetSnapshotNames()) t[i++] = n;
                                return t;
                            },
        // ---- リバーブ ----
        // ゾーン（AudioReverbZone）の外で使う既定の響き。wet 0..1。未知のプリセットは false。
        "setReverb",        [](AudioSystem& a, const std::string& preset, sol::optional<float> wet) {
                                return a.SetDefaultReverb(preset, wet.value_or(0.35f));
                            },
        "getReverbPresets", [](AudioSystem& /*a*/, sol::this_state ts) {
                                sol::state_view lv(ts);
                                sol::table t = lv.create_table();
                                std::size_t n = 0;
                                const audio::ReverbPreset* p = audio::ReverbPresetTable(n);
                                for (std::size_t i = 0; i < n; ++i) t[i + 1] = std::string(p[i].name);
                                return t;
                            },
        "setBusReverbSend", &AudioSystem::SetBusReverbSend,
        "getBusReverbSend", &AudioSystem::GetBusReverbSend,
        "getBuses",         [](AudioSystem& a, sol::this_state ts) {
                                sol::state_view lv(ts);
                                sol::table t = lv.create_table();
                                int i = 1;
                                for (const auto& b : a.GetBuses()) t[i++] = b.name;
                                return t;
                            },
        // 「今なにが鳴っているか」。playBGM は同じパスでも頭出しするので、
        // シーンをまたいで同じ曲を流し続けたいときはこれで判定して呼ばない。
        "getCurrentBGM",    &AudioSystem::GetCurrentBGM,
        "isBGMPlaying",     &AudioSystem::IsBGMPlaying,
        "getBGMList",       &AudioSystem::GetBGMList,
        "getSFXList",       &AudioSystem::GetSFXList,
        "rescan",           &AudioSystem::ScanAudioFiles
    );
}

} // namespace dx12e
