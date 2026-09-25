// ===========================================================================
// MCP: 音（AI が音を「観測」する口）
// ---------------------------------------------------------------------------
// ★なぜ要るか: AI はスクショで絵は見られるが、音は一切観測できなかった。
//   「足音が鳴っていない」「BGM が小さすぎる」「壁越しなのにこもっていない」「上限で
//   大事な音が消えている」を、鳴らした本人（AI）が確かめる手段が無かった。
//   audio_state は数値だけを返す（言葉への言い換えは TS 側の担当）。
//
// 返すもの（全部そのまま数値。dB は dBFS、-120 = 無音）:
//   device    … 音声デバイスの有無・チャンネル数・サンプルレート
//   listener  … リスナー位置
//   limits    … 実ボイスの上限と本数・仮想ボイスの本数・ボイスを作り直した累計時間
//   buses[]   … バスごとの音量・ミュート・ローパス・スナップショット補正・メーター（ピーク/RMS）
//   voices[]  … 鳴っている論理ボイス（音名・バス・音量・距離・遮蔽・仮想化中か・優先度・位置）
//   bgm / snapshot / reverb / streams
// 即時応答（遅延なし）。Editor / Play どちらでも呼べる。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;

namespace
{
double R(double v, double scale = 1000.0) { return std::round(v * scale) / scale; }
double Db(float v) { return R(v, 100.0); }

nlohmann::json ReverbParamsJson(const audio::ReverbParams& p)
{
    return {
        {"roomMb",           R(p.room, 1.0)},
        {"roomHfMb",         R(p.roomHF, 1.0)},
        {"decayTime",        R(p.decayTime)},
        {"decayHfRatio",     R(p.decayHFRatio)},
        {"reflectionsMb",    R(p.reflections, 1.0)},
        {"reflectionsDelay", R(p.reflectionsDelay, 10000.0)},
        {"reverbMb",         R(p.reverb, 1.0)},
        {"reverbDelay",      R(p.reverbDelay, 10000.0)},
        {"diffusion",        R(p.diffusion, 10.0)},
        {"density",          R(p.density, 10.0)},
    };
}
} // namespace

void Application::RegisterMcpAudioMethods()
{
    using json = nlohmann::json;

    McpDefine("audio_state", "", DX12E_MCP_HANDLER
        {
            if (!m_audioSystem)
                throw McpError(McpErr::Internal, "audio system is not created",
                               "エンジンの起動が終わっていない。dx12_ping が通るまで待ってから呼ぶこと");
            AudioSystem& a = *m_audioSystem;

            json r;
            r["device"] = {
                {"ready",      a.IsDeviceReady()},
                {"status",     a.GetDeviceStatus()},
                {"channels",   a.GetOutputChannels()},
                {"sampleRate", a.GetOutputSampleRate()},
            };
            {
                float lx = 0, ly = 0, lz = 0;
                a.GetListenerPos(lx, ly, lz);
                r["listener"] = {{"position", {R(lx), R(ly), R(lz)}}};
            }
            {
                u32 real = 0, virt = 0;
                a.GetVoiceCounts(real, virt);
                r["limits"] = {
                    {"maxVoices",        a.GetMaxVoices()},
                    {"logicalCapacity",  AudioSystem::kMaxLogicalVoices},
                    {"realVoices",       real},
                    {"virtualVoices",    virt},
                    {"voiceChurnCount",  a.GetVoiceChurnCount()},
                    {"voiceChurnMs",     R(a.GetVoiceChurnMs())},
                };
            }

            json buses = json::array();
            for (const auto& b : a.GetBuses())
            {
                buses.push_back({
                    {"name",            b.name},
                    {"parent",          b.parent},
                    {"volume",          R(b.volume)},
                    {"muted",           b.muted},
                    {"lowpassHz",       R(b.lowpassHz, 1.0)},
                    {"snapshotGain",    R(b.snapshotGain)},
                    {"snapshotLowpassHz", R(b.snapshotLowpass, 1.0)},
                    {"effectiveGain",   R(b.effectiveGain)},
                    {"chainGain",       R(b.chainGain)},
                    {"reverbSend",      R(b.reverbSend)},
                    {"reverbReturn",    b.reverbReturn},
                    {"voiceLimit",      b.voiceLimit},
                    {"realVoices",      b.realVoices},
                    {"virtualVoices",   b.virtualVoices},
                    {"peakDb",          Db(b.peakDb)},
                    {"rmsDb",           Db(b.rmsDb)},
                    {"peakNowDb",       Db(b.peakNowDb)},
                    {"builtin",         b.builtin},
                });
            }
            r["buses"] = std::move(buses);

            json voices = json::array();
            for (const auto& v : a.GetVoices())
            {
                json jv = {
                    {"id",            v.id},
                    {"clip",          v.path},
                    {"bus",           v.bus},
                    {"priority",      v.priority},
                    {"volume",        R(v.volume)},
                    {"fade",          R(v.fade)},
                    {"pitch",         R(v.pitch)},
                    {"audibility",    R(v.audibility, 100000.0)},
                    {"gainDb",        Db(audio::LinearToDb(v.audibility))},
                    {"spatial",       v.spatial},
                    {"distance",      R(v.distance)},
                    {"distanceGain",  R(v.distanceGain)},
                    {"occlusion",     R(v.occlusion)},
                    {"reverbSend",    R(v.reverbSend)},
                    {"loop",          v.loop},
                    {"virtual",       v.isVirtual},
                    {"virtualReason", v.virtualReason},
                    {"bgm",           v.bgm},
                    {"paused",        v.paused},
                    {"stream",        v.stream},
                    {"positionSec",   R(v.positionSec)},
                    {"lengthSec",     R(v.lengthSec)},
                };
                if (v.stream)
                {
                    jv["bufferedChunks"] = v.bufferedChunks;
                    jv["underruns"]      = v.underruns;
                }
                if (v.loopStartSec > 0.0f || v.loopEndSec > 0.0f)
                    jv["loopPointsSec"] = {R(v.loopStartSec), R(v.loopEndSec)};
                voices.push_back(std::move(jv));
            }
            r["voices"] = std::move(voices);

            r["bgm"] = {{"path", a.GetCurrentBGM()}, {"playing", a.IsBGMPlaying()}};

            {
                const auto s = a.GetSnapshotState();
                r["snapshot"] = {
                    {"current",  s.current},
                    {"target",   s.target},
                    {"progress", R(s.progress)},
                    {"duration", R(s.duration)},
                    {"defined",  s.defined},
                };
            }
            {
                const auto rv = a.GetReverbState();
                json zones = json::array();
                for (const auto& z : rv.zones)
                    zones.push_back({{"name", z.name}, {"preset", z.preset}, {"weight", R(z.weight)},
                                     {"wet", R(z.wet)}, {"priority", z.priority}});
                r["reverb"] = {
                    {"available",     rv.available},
                    {"defaultPreset", rv.defaultPreset},
                    {"defaultWet",    R(rv.defaultWet)},
                    {"dominantZone",  rv.dominant},
                    {"targetWet",     R(rv.targetWet)},
                    {"currentWet",    R(rv.currentWet)},
                    {"zones",         std::move(zones)},
                    {"params",        ReverbParamsJson(rv.current)},
                };
            }
            {
                const auto st = a.GetStreamStats();
                r["streams"] = {
                    {"count",          st.count},
                    {"memoryBytes",    st.memoryBytes},
                    {"underruns",      st.underruns},
                    {"decodeMs",       R(st.decodeMs)},
                    {"chunksDecoded",  st.chunksDecoded},
                    {"watchdogPumps",  st.watchdogPumps},
                    {"audioSecondsDecoded", R(st.audioSecondsDecoded)},
                    // 1 秒ぶんの音をデコードするのに掛かった CPU 時間（ms）。小さいほど軽い
                    {"decodeMsPerAudioSecond",
                     st.audioSecondsDecoded > 0.0 ? R(st.decodeMs / st.audioSecondsDecoded) : 0.0},
                };
            }
            resp["ok"] = true;
            resp["result"] = std::move(r);
        });
}

} // namespace dx12e
