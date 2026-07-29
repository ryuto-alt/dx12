#include "core/PlaySession.h"

#include "core/Logger.h"
#include "input/InputSystem.h"
#include "renderer/Camera.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dx12e
{
namespace {

using json = nlohmann::json;

// VK → 名前。ApplicationInternal.h の ParseMcpVk と同じ語彙にしてある
// （記録された名前をそのまま dx12_key_press へ投げ返せる = 手で当たりを付けた再現ができる）。
std::string VkName(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    switch (vk)
    {
        case VK_SPACE:   return "SPACE";
        case VK_SHIFT:   return "SHIFT";
        case VK_CONTROL: return "CTRL";
        case VK_MENU:    return "ALT";
        case VK_TAB:     return "TAB";
        case VK_ESCAPE:  return "ESC";
        case VK_RETURN:  return "ENTER";
        case VK_UP:      return "UP";
        case VK_DOWN:    return "DOWN";
        case VK_LEFT:    return "LEFT";
        case VK_RIGHT:   return "RIGHT";
        case VK_LBUTTON: return "MOUSE_L";
        case VK_RBUTTON: return "MOUSE_R";
        case VK_MBUTTON: return "MOUSE_M";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "vk_%d", vk);
    return buf;
}

// XInput のボタンビット（XINPUT_GAMEPAD_* と同値。Xinput.h を引かずに済ませる）。
struct PadBit { u32 bit; const char* name; };
constexpr PadBit kPadBits[] = {
    { 0x0001, "DPAD_UP"    }, { 0x0002, "DPAD_DOWN"  },
    { 0x0004, "DPAD_LEFT"  }, { 0x0008, "DPAD_RIGHT" },
    { 0x0010, "START"      }, { 0x0020, "BACK"       },
    { 0x0040, "LSTICK"     }, { 0x0080, "RSTICK"     },
    { 0x0100, "LB"         }, { 0x0200, "RB"         },
    { 0x1000, "A"          }, { 0x2000, "B"          },
    { 0x4000, "X"          }, { 0x8000, "Y"          },
};

} // namespace

void PlaySession::Start(f32 nowSec)
{
    *this = PlaySession{};   // 前回の記録を捨てる（Play ごとに 1 セッション）
    m_started   = true;
    m_recording = true;
    m_startSec  = nowSec;
    m_endSec    = nowSec;
    m_nextSampleAt = 0.0f;

    // ログのカーソルを「いまの末尾」へ進める。0 のまま読むと Play 前のログを
    // 4000 件まるごと拾ってしまう。
    std::vector<LogEntry> discard;
    m_logCursor = Logger::ReadBuffered(0, discard);
}

void PlaySession::Stop(f32 nowSec)
{
    if (!m_recording) return;
    m_recording = false;
    m_endSec    = nowSec;

    // 最後のログを取りこぼさない（死ぬ直前のエラーがまさに読みたいもの）。
    std::vector<LogEntry> fresh;
    m_logCursor = Logger::ReadBuffered(m_logCursor, fresh);
    const f32 t = m_endSec - m_startSec;
    for (const auto& le : fresh)
        if (le.level >= 3 || le.text.rfind("[Lua]", 0) == 0)
            Push(t, le.level >= 4 ? "error" : (le.level == 3 ? "warn" : "lua"), le.text);
}

void PlaySession::Push(f32 t, std::string kind, std::string detail)
{
    if (m_events.size() >= kMaxEvents) { m_events.pop_front(); ++m_droppedEvents; }
    m_events.push_back({ t, std::move(kind), std::move(detail) });
}

void PlaySession::Update(f32 nowSec, const InputSystem& input, const Camera* cam, f32 fps)
{
    if (!m_recording) return;
    const f32 t = nowSec - m_startSec;
    m_endSec = nowSec;
    ++m_frames;
    if (fps > 0.0f) m_fpsMin = (m_fpsMin <= 0.0f) ? fps : std::min(m_fpsMin, fps);

    // ---- 入力のエッジ（毎フレーム）----
    // InputSystem 側に手を入れず、こちらで前フレーム状態を持って差分を取る。
    // WndProc 由来も MCP 合成入力も同じ m_keys に入るので、この 1 箇所で両方拾える。
    for (int vk = 0; vk < 256; ++vk)
    {
        const bool down = input.IsKeyDown(vk);
        if (down == m_prevKeys[vk]) continue;
        m_prevKeys[vk] = down;
        Push(t, down ? "key_down" : "key_up", VkName(vk));
    }

    if (input.IsPadConnected(0))
    {
        u32 pad = 0;
        for (const auto& pb : kPadBits)
            if (input.IsPadButtonDown(0, static_cast<int>(pb.bit))) pad |= pb.bit;
        const u32 diff = pad ^ m_prevPad;
        if (diff)
        {
            for (const auto& pb : kPadBits)
                if (diff & pb.bit) Push(t, (pad & pb.bit) ? "pad_down" : "pad_up", pb.name);
            m_prevPad = pad;
        }
    }

    // マウスはデルタしか無い（絶対座標を持っていない）。移動量だけ積んでサンプルへ。
    m_mouseAccum += std::fabs(input.GetMouseDeltaX()) + std::fabs(input.GetMouseDeltaY());

    // ---- 10Hz のサンプル + ログ取り込み ----
    if (t < m_nextSampleAt) return;
    m_nextSampleAt = t + 1.0f / kSampleHz;

    Sample s{};
    s.t     = t;
    s.fps   = fps;
    s.mouse = m_mouseAccum;
    m_mouseAccum = 0.0f;
    if (cam)
    {
        const auto p = cam->GetPosition();
        s.pos[0] = p.x; s.pos[1] = p.y; s.pos[2] = p.z;
        s.yaw    = cam->GetYaw();
        s.pitch  = cam->GetPitch();
    }
    if (m_samples.size() >= kMaxSamples) { m_samples.pop_front(); ++m_droppedSamples; }
    m_samples.push_back(s);

    // warn 以上 + Lua の print/log 行だけ拾う（info を全部入れるとノイズで読めない）。
    std::vector<LogEntry> fresh;
    m_logCursor = Logger::ReadBuffered(m_logCursor, fresh);
    for (const auto& le : fresh)
        if (le.level >= 3 || le.text.rfind("[Lua]", 0) == 0)
            Push(t, le.level >= 4 ? "error" : (le.level == 3 ? "warn" : "lua"), le.text);
}

nlohmann::json PlaySession::ToJson(size_t maxEvents, size_t maxSamples) const
{
    auto tail = [](const auto& src, size_t cap, auto emit) {
        json arr = json::array();
        const size_t skip = (src.size() > cap) ? src.size() - cap : 0;
        size_t i = 0;
        for (const auto& v : src) { if (i++ < skip) continue; arr.push_back(emit(v)); }
        return std::pair<json, size_t>{ std::move(arr), skip };
    };

    auto [events, evSkipped] = tail(m_events, maxEvents, [](const Event& e) {
        return json{{"t", e.t}, {"kind", e.kind}, {"detail", e.detail}};
    });
    auto [samples, smSkipped] = tail(m_samples, maxSamples, [](const Sample& s) {
        return json{{"t", s.t}, {"fps", s.fps},
                    {"camPos", json::array({s.pos[0], s.pos[1], s.pos[2]})},
                    {"camYaw", s.yaw}, {"camPitch", s.pitch}, {"mouse", s.mouse}};
    });

    size_t errors = 0, warns = 0, inputs = 0;
    for (const auto& e : m_events)
    {
        if (e.kind == "error") ++errors;
        else if (e.kind == "warn") ++warns;
        else if (e.kind.rfind("key_", 0) == 0 || e.kind.rfind("pad_", 0) == 0) ++inputs;
    }

    return json{
        {"started",   m_started},
        {"recording", m_recording},
        {"durationSec", m_started ? (m_endSec - m_startSec) : 0.0f},
        {"frames",    m_frames},
        {"fpsMin",    m_fpsMin},
        {"summary",   {{"errors", errors}, {"warnings", warns}, {"inputEvents", inputs},
                       {"totalEvents", m_events.size()}, {"totalSamples", m_samples.size()}}},
        // dropped* = リング上限で捨てた分、skipped* = 今回返しきれず省いた古い分。
        {"droppedEvents",  m_droppedEvents},
        {"droppedSamples", m_droppedSamples},
        {"skippedEvents",  evSkipped},
        {"skippedSamples", smSkipped},
        {"events",  std::move(events)},
        {"samples", std::move(samples)},
    };
}

} // namespace dx12e
