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
    std::printf(g_fail ? "IblRebakeTriggerTests FAILED (%d)\n" : "IblRebakeTriggerTests OK\n", g_fail);
    return g_fail ? 1 : 0;
}
