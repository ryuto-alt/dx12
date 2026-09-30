# 物理ベース大気（A1）

Hillaire 2020 の 4 LUT（Transmittance / Multi-scattering / Sky-View / Aerial-Perspective）による空・遠景の霞・時刻系。
シーン設定 `atmosphere.enabled` で「従来の空 / 物理大気」を切り替える（**既定は OFF ＝ 従来の空・従来の時刻曲線・従来の IBL のまま、絵は変わらない**）。
モデルと LUT の仕様・時刻系の仕様・IBL / DDGI 更新の仕様・性能・既知の差は、別の節（親が追記）を参照。

## MCP の使い方

設定は `dx12_get_scene_settings` / `dx12_set_scene_settings` の `atmosphere` オブジェクトで読み書きする。指定したキーだけ適用され、Undo は 1 エントリ。

### 読む

```json
dx12_get_scene_settings {}
→ { "skybox": {...}, "atmosphere": { "enabled": false, "timeOfDay": 12, "timeSpeed": 0, "latitudeDeg": 35,
      "dayOfYear": 81, "sunMode": 0, "driveSun": true, "driveIBL": true, "sunIlluminance": 128000, ... },
    "atmosphereState": { "active": true, "sunElevationDeg": 55.0, "sunAzimuthDeg": 180.0, "isMoon": false,
      "groundIlluminanceLux": [110000, 101000, 88000], "iblRebakeCount": 3, "iblBigCount": 1, "lastSunDeltaDeg": 0.1,
      "gpuMs": { "skyViewLut": 0.02, "aerialPerspectiveLut": 0.02, "skyDraw": 0.05, ... },
      "lutUpdates": { "transMs": 1, "skyView": 40, "ap": 300, "cube": 4 } } }
```

- `atmosphere` は全項目（RGB は `[r,g,b]`）。`atmosphereState` は `enabled` のときだけ意味を持つ（OFF のときは `{"active": false}`）。
- `atmosphereState.gpuMs` は自前のタイムスタンプによる直近の GPU 時間（ms）。`gpuMsRanLastFrame` が false のスコープは古い値。

### 書く

```json
// 物理大気を ON にして正午にする
dx12_set_scene_settings { "atmosphere": { "enabled": true, "timeOfDay": 12 } }

// 夕方（18 時ごろ）。太陽ライトの向き・色・強さは大気が毎フレーム決める
dx12_set_scene_settings { "atmosphere": { "enabled": true, "timeOfDay": 18.2 } }

// プリセット + 個別の上書き（preset を先に適用してから個別項目を上書きする）
dx12_set_scene_settings { "atmosphere": { "preset": "haze", "apStrength": 1.5 } }

// Play 中に時間を進める（0.1 時間/秒 = 1 日 240 秒）。Stop すると元の時刻へ戻る
dx12_set_scene_settings { "atmosphere": { "timeSpeed": 0.1 } }
```

- プリセット: `earth`（既定値）/ `mars`（薄い大気 + 赤い塵）/ `haze`（霞）/ `twilight`（薄明。時刻を 18.35 時にする）。
  プリセットが変えるのは物理パラメータ（と twilight の時刻）だけで、`enabled` / `driveSun` / `driveIBL` / `timeSpeed` などの使い方の設定は変えない。
- 範囲外の値は丸める（`timeOfDay` 0..24、`latitudeDeg` -90..90、`dayOfYear` 1..366 など）。未知のキー・型の違いは `INVALID_PARAM`（何も変わらない）。
- `enabled` を切り替えると、次のフレームで環境（IBL）が大気の空へ／従来の環境マップへ焼き直される。
- 大気を ON にしても従来の空（`skybox.envMapPath`）は残っている。`enabled:false` に戻すと従来の空・従来の IBL がそのまま使われる（従来へ戻す方法はこれだけ）。
- 太陽ライト（最初の DirectionalLight）は、`driveSun=true` の間、大気が向き（`sunMode=0` のとき）・色・強さを毎フレーム上書きする。
  手で決めたいなら `sunMode:1`（向きを直接指定）か `driveSun:false`。

### `dx12_set_sun`

```json
// 物理大気が ON: timeOfDay は「大気の時刻」を設定する（従来の曲線は使わない）
dx12_set_sun { "timeOfDay": 6.5 }
→ { ..., "atmosphere": { "timeOfDay": 6.5, "sunMode": 0, "sunElevationDeg": 7.8, "sunAzimuthDeg": 95.0,
                          "groundIlluminanceLux": [...], "isMoon": false } }

// 方位・高度の直接指定: sunMode=1（太陽の向きを直接指定）へ切り替わる
dx12_set_sun { "azimuth": 250, "elevation": 12 }
```

- 大気が OFF のときの `dx12_set_sun` は従来どおり（時刻カーブ・冪等）。
- 大気 ON で `timeOfDay` を渡した応答の `direction` / `color` / `intensity` は書き込み直後の値で、実効値は応答の `atmosphere` と `dx12_list_lights` を見る。
- `sunMode:1` から時刻で動かしたいときは `dx12_set_scene_settings { "atmosphere": { "sunMode": 0 } }` で戻す。

## エディタでの操作

ツール > **ライティング** 窓の「大気 (物理ベース)」節（「スカイ / IBL」の下）。

| 項目 | 意味 |
|---|---|
| 物理大気を使う | ON で空・遠景の霞・太陽の色と強さを大気から決める。OFF（既定）は従来のまま。ON にしても環境マップの設定は残り、OFF に戻すとそのまま使われる |
| プリセット | 地球 / 火星風 / 霞 / 薄明。物理パラメータをまとめて切り替える（薄明は時刻を 18.35 時にする）。使い方の設定は変えない |
| 時刻 | 現地太陽時 0〜24。春分・秋分は 6 時が日の出・18 時が日の入り。太陽の向き・色・強さ・空・IBL・影が 1 つの時刻でつながる。下に `HH:MM` を表示 |
| 時間経過 (時間/秒) | Play 中に時刻を進める速さ。0 = 止める。0.1 なら 240 秒で 1 日。エディタ編集中は進まず、Stop で元の時刻へ戻る |
| 緯度 / 日付 (年内通日) / 北の向き | 太陽の南中高度・日の長さ・昇る方角。81 = 春分、172 = 夏至、355 = 冬至。北の向きはワールドの +Z が北から時計回りに何度ずれているか |
| 太陽の向き | 「時刻から決める」/「太陽ライトの向きを直接指定」（DirectionalLight の向きをそのまま使う。月は使わない） |
| 太陽ライトを大気で駆動 | ON で太陽ライトの向き（時刻から決めるとき）・色・強さを透過率から毎フレーム決める（夕方は赤く、夜は月光）。OFF ならライトは手動で、空だけが大気 |
| 空を環境光へ反映 | ON で空を環境マップ（IBL / DDGI の空の項）へ反映。時刻が動くと数フレームに分けて焼き直す |
| 太陽照度 (lux) | 大気の上端での太陽の照度（既定 128000）。地表では約 8〜10 万 lux |
| 星 / 月 | 夜の星・月（月は夜の光源にもなる） |
| 地表アルベド | 地面の反射率。地平線の下の色と多重散乱の地面反射に効く |
| 遠景の霞 (AP) / 開始距離 / 最大距離 / 濃さ | エアリアルパースペクティブ。開始距離より手前には掛けない（既定 100 m）。濃さ 1 = 物理どおり |
| いまの太陽（読み取り） | 太陽の高度・方位・地表の照度・光の色（設定から計算した値） |

散乱係数・惑星半径・ミー g などの詳細な物理パラメータは UI に出していない。MCP の `set_scene_settings` の `atmosphere` から変える。
設定はシーン JSON の `atmosphere` に保存される（既定と同じなら書かない）。

## Lua から使うには

- **A1 では大気専用の Lua API は無い**（Lua リファレンスにも足していない）。
- `Lighting.setTimeOfDay(hour)` は従来の時刻曲線で太陽ライトを動かす関数で、大気とは独立。
  大気が ON で `driveSun=true` の間は、大気が太陽ライトの向き・色・強さを**毎フレーム上書きする**ので、Lua で設定した値は次のフレームで消える。
  大気の時刻は `atmosphere.timeOfDay`（シーン設定）で、Lua からは変えられない。
- Lua から時刻を動かしたいときは次のどれか。
  - 時間経過（`timeSpeed`）をシーン設定で決めておく（Play 中に自動で進む）。
  - MCP（`dx12_set_scene_settings` / `dx12_set_sun`）から動かす。
  - 大気の時刻を Lua から扱う API は将来の課題（A2 以降）。
- 大気を使いつつ Lua で太陽を完全に手動制御したい場合は、`atmosphere.driveSun` を false にする（空・遠景の霞だけが大気になり、太陽ライトは Lua のまま）。
