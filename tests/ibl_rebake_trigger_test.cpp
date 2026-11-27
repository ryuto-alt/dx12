// W1-C9 / BUG-17: IBL 焼き直しのきっかけと、撮影の待ちの判定表（ヘッダだけ・GPU 不要）。
#include <cstdio>
#include "renderer/atmosphere/IblRebakePolicy.h"

using namespace dx12e::atmosphere;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    // ---- 焼き直しのきっかけ ----
    IblRebakeInput in;
    in.sinceStartSec = 10.0;
    {   // 不変条件: 既定の閾値では、高度の条件が単独で決めることは無い（高度差 <= 角度差）。
        //   角度が閾値以下なら、高度差も 0.5° 未満で、焼かない。角度が閾値を超えたら、高度に関わらず焼く。
        for (double ang = 0.0; ang <= 1.0; ang += 0.01)
        {
            IblRebakeInput i = in; i.sunAngleDeg = ang; i.sunElevDeltaDeg = ang;   // 高度差の最大は角度差
            const bool need = DecideIblRebake(i).need;
            CHECK(need == (ang > i.thresholdDeg));
        }
    }
    {   // 何も動かない → 焼かない
        auto d = DecideIblRebake(in);
        CHECK(!d.need && !d.start && !d.burst);
    }
    {   // 設定の鍵が変わった → burst で即開始（間引きの時間内でも）
        IblRebakeInput i = in; i.keyChanged = true; i.sinceStartSec = 0.0;
        auto d = DecideIblRebake(i);
        CHECK(d.need && d.start && d.burst);
    }
    {   // 決定論撮影 → burst
        IblRebakeInput i = in; i.deterministic = true;
        CHECK(DecideIblRebake(i).burst);
    }
    {   // 太陽 5° 超 → burst、5° 以下は burst ではない
        IblRebakeInput i = in; i.sunAngleDeg = 5.5; i.sunElevDeltaDeg = 5.5;
        CHECK(DecideIblRebake(i).burst);
        i.sunAngleDeg = 4.0; i.sunElevDeltaDeg = 4.0;
        auto d = DecideIblRebake(i);
        CHECK(d.need && d.start && !d.burst);
    }
    {   // 高度 0.5° 以上は、ユーザー閾値 2° でも焼く。0.49° は焼かない
        IblRebakeInput i = in; i.thresholdDeg = 2.0; i.sunAngleDeg = 0.6; i.sunElevDeltaDeg = 0.5;
        CHECK(DecideIblRebake(i).need);
        i.sunAngleDeg = 0.49; i.sunElevDeltaDeg = -0.49;
        CHECK(!DecideIblRebake(i).need);
        i.sunElevDeltaDeg = -0.5; i.sunAngleDeg = 0.5;
        CHECK(DecideIblRebake(i).need);   // 符号は見ない
    }
    {   // ヒステリシス: 閾値を超えても、前回の開始から 1/maxHz 経つまでは始めない
        IblRebakeInput i = in; i.sunAngleDeg = 0.3; i.sunElevDeltaDeg = 0.3; i.maxHz = 2.0;
        i.sinceStartSec = 0.1;
        auto d = DecideIblRebake(i);
        CHECK(d.need && !d.start);
        i.sinceStartSec = 0.5;
        CHECK(DecideIblRebake(i).start);
    }
    {   // 毎フレーム走らない: 開始直後は基準が更新され（角度 0）、need が立たない
        IblRebakeInput i = in; i.sunAngleDeg = 0.0; i.sunElevDeltaDeg = 0.0; i.sinceStartSec = 0.0;
        CHECK(!DecideIblRebake(i).need);
    }
    {   // カメラ高度: 50m 未満は無視、超えたら焼く
        IblRebakeInput i = in; i.altDeltaM = 40.0; i.altBaseM = 0.0;
        CHECK(!DecideIblRebake(i).need);
        i.altDeltaM = 60.0;
        CHECK(DecideIblRebake(i).need);
    }

    {   // 走る太陽（60fps・毎フレーム 0.03° 進む（1.8°/秒））を 20 秒回しても、焼き直しの開始は maxHz で頭打ち。
        //   開始のたびに基準を今の太陽へ付け替える（Application と同じ）。burst は立たない速さ。
        const double dt = 1.0 / 60.0, stepDeg = 0.03;
        for (double maxHz : {1.0, 2.0, 4.0})
        {
            double angle = 0.0, since = 100.0, elapsed = 0.0;
            int starts = 0;
            for (int f = 0; f < 60 * 20; ++f)
            {
                angle += stepDeg; since += dt; elapsed += dt;
                IblRebakeInput i; i.sunAngleDeg = angle; i.sunElevDeltaDeg = angle;
                i.sinceStartSec = since; i.maxHz = maxHz;
                const auto d = DecideIblRebake(i);
                CHECK(!d.burst);
                if (d.start) { ++starts; angle = 0.0; since = 0.0; }
            }
            CHECK(starts >= 1);
            CHECK(starts <= maxHz * elapsed + 1.0);
        }
    }

    // ---- 撮影の待ち ----
    IblWaitState w;   // 大気 OFF・ホスト無し
    CHECK(!IblRebakePending(w));
    CHECK(IblRebakeWaitFrames(w, 6) == 0);
    {   // OFF → ON 直後（ホスト未作成）
        IblWaitState s; s.sceneEnabled = true; s.sceneDriveIbl = true;
        CHECK(IblRebakePending(s));
        CHECK(IblRebakeWaitFrames(s, 6) == kIblWaitBaseFrames);
    }
    {   // 定常（焼き済み・動いていない）→ 待たない
        IblWaitState s; s.sceneEnabled = s.sceneDriveIbl = true; s.hostExists = s.enabledPrev = s.envWantedPrev = true;
        s.iblValid = s.envLoaded = true;
        CHECK(!IblRebakePending(s));
    }
    {   // 時刻を動かした（太陽 3°）→ 待つ。0.4° は待たない（しきい 0.25° なら待つ）
        IblWaitState s; s.sceneEnabled = s.sceneDriveIbl = true; s.hostExists = s.enabledPrev = s.envWantedPrev = true;
        s.iblValid = s.envLoaded = true; s.sunAngleDeg = 3.0; s.sunElevDeltaDeg = 3.0;
        CHECK(IblRebakePending(s));
        s.thresholdDeg = 5.0; s.sunAngleDeg = 0.4; s.sunElevDeltaDeg = 0.4;
        CHECK(!IblRebakePending(s));
    }
    {   // 増分の途中 → 段数ぶん足す
        IblWaitState s; s.sceneEnabled = s.sceneDriveIbl = true; s.hostExists = s.enabledPrev = s.envWantedPrev = true;
        s.iblValid = s.envLoaded = true; s.iblRunning = true;
        CHECK(IblRebakeWaitFrames(s, 6) == kIblWaitBaseFrames + 6);
    }
    {   // ON → OFF の後始末が未了 / skyboxDirty
        IblWaitState s; s.sceneEnabled = false; s.hostExists = s.enabledPrev = true;
        CHECK(IblRebakePending(s));
        s.enabledPrev = false;
        CHECK(!IblRebakePending(s));
        s.skyboxDirty = true;
        CHECK(IblRebakePending(s));
    }
    {   // driveIBL が OFF の大気は IBL を焼かない（切り替え直後だけ待つ）
        IblWaitState s; s.sceneEnabled = true; s.sceneDriveIbl = false; s.hostExists = s.enabledPrev = true;
        CHECK(!IblRebakePending(s));
        s.envWantedPrev = true;
        CHECK(IblRebakePending(s));
    }
    // ---- 撮影応答の note（W1-FU-E-30） ----
    CHECK(IblShotNote(false, 0).empty());
    CHECK(IblShotNote(false, 9).empty());   // 頼まれていなければ何も足さない
    {
        const std::string n = IblShotNote(true, 8);
        CHECK(n.find("iblRebakePending=true") != std::string::npos);
        CHECK(n.find("iblWaitFrames=8") != std::string::npos);
        CHECK(n.find("決定論経路") != std::string::npos);
        const std::string z = IblShotNote(true, 0);
        CHECK(z.find("iblRebakePending=false") != std::string::npos);
        CHECK(z.find("iblWaitFrames=0") != std::string::npos);
    }
    std::printf(g_fail ? "IblRebakeTriggerTests FAILED (%d)\n" : "IblRebakeTriggerTests OK\n", g_fail);
    return g_fail ? 1 : 0;
}
