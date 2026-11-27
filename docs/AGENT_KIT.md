# アイテムキットと枠(スロット)

エージェントを 6〜8 体並べても、オーナーの PC を重くしないための道具一式です。

- **枠(スロット)**: ビルド・GPU・テストが同時に走る数を、PC 全体で制限します。
- **kit.ps1**: ビルド・テスト・ゴールデン・エンジン・撮影を、少ないターンで回す 1 本のコマンドです。
- **-Changed / -Auto / known_red**: 項目の途中の確認を、変更に関係するものだけに絞ります。
- **ブリーフ生成**: 200KB を超える EXEC 文書を毎回読み直さずに済むよう、1 項目ぶんを 8KB 以下にします。

統合とゲートは、これらの絞り込みを使わず**全件**で確かめます。

## 1. 枠(スロット)

`tools/build.ps1` のスロット制を、GPU とテストに広げたものです。

| 種類 | Mutex 名 | 昼(操作中) | 離席中 | 取る処理 |
|---|---|---|---|---|
| ビルド | `Global\dx12-build`(2 本目は `-2`) | 1 本 | 2 本(`DX12_BUILD_SLOTS`、最大 3) | `tools/build.ps1` |
| GPU | `Global\dx12-gpu`(`-2`、`-3`) | 1 本 | 2 本(`DX12_GPU_SLOTS`、最大 3) | `golden.mjs`、`run_ui_tests.ps1`、`engine_instance.ps1`、`ctest.ps1 -Gpu/-Ui` |
| テスト | `Global\dx12-test`(`-2`、`-3`) | 1 本 | 3 本(`DX12_TEST_SLOTS`、最大 3) | `ctest.ps1`、`with_slot.ps1`(npm test など) |

- **スロット 1 は常に使えます。** スロット 2 以降は、最後のキー・マウス入力から `DX12_BUILD_IDLE_MIN` 分(既定 10)以上たち、
  かつ空き物理メモリが `DX12_BUILD_MIN_FREE_GB`(既定 8)以上のときだけ取れます。
- スロット 2 以降で取れた処理は **Idle 優先度**で走ります(スロット 1 は BelowNormal)。
- 取れるまで待ちます(既定 40 分。30 秒ごとに状況を出す)。待ち切れなければ**終了コード 3**です。
- `DX12_SLOTS_DISABLE=1` にすると、枠を一切使わない従来の挙動になります。
- 入れ子: 親が枠を持っているとき、子は取り直しません(`DX12_SLOT_HELD_GPU` / `DX12_SLOT_HELD_TEST`)。
  `run_ui_tests.ps1` と `golden.mjs` が GPU の枠を持ち、その下で起動される `engine_instance.ps1` は二重に取りません。
- ctest の GPU と UI を回すとき(`-Gpu` / `-Ui`)は、**gpu → test の順**で両方を取ります(順序を固定して、デッドロックを避ける)。
- 実装: `tools/lib/SlotLock.psm1`(PowerShell)、`tools/lib/slotLock.mjs`(Node)。
  Node は名前付き Mutex を持てないので、`tools/lib/slot_hold.ps1`(枠を持つ専用プロセス)を子として立てます。
  このプロセスが死ぬと OS が Mutex を放棄扱いにし、次の待ち手が引き継ぎます(異常終了しても枠は戻る)。

### 1.1 エンジンの枠は「エンジンの寿命」のあいだ持ちます

`tools/engine_instance.ps1` は、エンジンを起動する前に GPU の枠を取り、**エンジンが終了するまで**持ち続けます。

- 枠を持つのは専用プロセスで、エンジンの `engine.pid` を見張ります。`-Stop`、`--idle-exit`、クラッシュのどれでも自動で戻ります。
- 昼は GPU の枠が 1 本なので、**エンジンは使い終わったらすぐ `engine-stop`(または `-Stop`)してください。**
  放置すると `--idle-exit`(既定 45 分)まで、他のエージェントの GPU 作業が待たされます。
- 枠が取れなければ起動せず終了コード 3 です。`-NoSlot` で枠なしの従来の挙動にできます。

### 1.2 性能計測の窓ロックとの併用

`%TEMP%\uno-gpu-window.lock`(`tools/mcp-server/testing/perfCore.ts` と `tools/scorecard/baseline_perf.mjs` が使う、
`{pid, owner, startedAt}` の JSON)は、そのままです。計測ツールは変更していません。

- GPU の枠を取った後、窓ロックの pid が**生きている間は待ちます**(計測中の GPU を邪魔しない)。
- pid が死んでいる古い窓ロックは無視します。`DX12_GPU_WINDOW_IGNORE=1` で窓ロックを無視できます。
- 計測ツール自身は GPU の枠を取りません(窓ロックだけ)。計測の最中に GPU の枠を取る側が待つ、という一方向の併用です。

### 1.3 環境変数

| 変数 | 意味 |
|---|---|
| `DX12_SLOTS_DISABLE=1` | 枠を使わない |
| `DX12_GPU_SLOTS` / `DX12_TEST_SLOTS` | 枠の数(1〜3。既定は GPU 2、テスト 3) |
| `DX12_BUILD_IDLE_MIN` | 「離席」とみなす無操作の分数(既定 10。ビルドと共通) |
| `DX12_BUILD_MIN_FREE_GB` | スロット 2 以降に要る空き物理メモリ(既定 8) |
| `DX12_BUILD_FAKE_IDLE_SEC` | テスト用: 最後の入力からの秒数を差し替える(ビルドと共通) |
| `DX12_GPU_SLOT_PREFIX` / `DX12_TEST_SLOT_PREFIX` | テスト用: Mutex 名の接頭辞 |
| `DX12_GPU_WINDOW_IGNORE` / `DX12_GPU_WINDOW_FILE` | 窓ロックを無視する / テスト用の置き場 |

### 1.4 ほかのコマンドを枠に載せる

```
pwsh tools/with_slot.ps1 -Kind test npm test          # tools/mcp-server へ cd してから。テストの枠
pwsh tools/with_slot.ps1 -Kind test -Label mcp node --test tools/bench
```

終了コードはコマンドのものです。枠が取れなければ 3 です。

## 2. build.ps1 の固定オーバーヘッド(B1)

何もしないビルドでも、vcvars の読み込みと `#deps 0` の検査で、中央値 6.4 秒かかっていました。

- **vcvars64 の環境をキャッシュ**します(`tools/lib/BuildEnv.psm1`)。
  - 場所: `%LOCALAPPDATA%\dx12-build-cache\vcvars-<キー>.json`(`DX12_BUILD_CACHE_DIR` で変更)。
  - キー: vcvars64.bat のパスと更新時刻、MSVC の既定バージョン(`Microsoft.VCToolsVersion.default.txt`)。VS を更新すれば自動で作り直します。
  - 保存するのは、vcvars が変えた変数と、PATH に足されたエントリだけです。呼び出し元の PATH は壊しません。
  - `DX12_BUILD_NO_VCVARS_CACHE=1` でキャッシュを使いません。
- **`#deps 0` の検査**は、`.ninja_deps` が前回の「異常 0 件」の確認から変わったときだけ走ります。意味は変えていません。
  - ビルド前の検査と clean は、スロットを取る**前**に行います。
  - ビルド後の検査は、スロットを手放した**後**に行います(他のビルドを待たせない)。
  - 記録は `<ビルドディレクトリ>\.deps0-stamp` です。

## 3. 変更に関係するテストだけを回す(T1)

### 3.1 ctest.ps1 -Changed

```
pwsh tools/ctest.ps1 -Changed                 # 基準ブランチからの変更に関係する CPU テストだけ
pwsh tools/ctest.ps1 -Changed -Plan           # 実行せず、選んだテストと理由を JSON で出す
pwsh tools/ctest.ps1 -Changed -Base master    # 基準を指定(既定: DX12_BASE_REF > overhaul/wave0 > master)
```

変更ファイルは、基準との共通祖先からの差分、作業ツリー、未追跡ファイルです。対応づけは次のとおりです(`tools/lib/ChangedMap.psm1`)。

| 変更 | 選ばれるもの |
|---|---|
| `tests/<x>.cpp`、CMake の `add_executable` に載っている src の cpp | その実行ファイルのテスト |
| `src/` の他のファイル | テストのソースがそのファイル名を `#include` していれば、そのテスト |
| それでも対応が無い `src/` `shaders/` `assets/` | 前回合格の後にリンクし直された exe のテスト(**先にビルドが要る**)。合格の記録が無ければ全件 |
| `tests/CMakeLists.txt` | 追加行に出てくるテスト名だけ。読めなければ全件 |
| 他の `CMakeLists.txt`、`cmake/`、`vcpkg.json`、`third_party/` | 全件 |
| `docs/`、`*.md`、`tools/mcp-server/` など | 無視(ctest の対象ではない) |
| `tools/ctest_changed_map.json` の rules に合うもの | 表のテスト名(正規表現)。`ignore: true` で対応づけから外す |

- **迷ったら全件**です。選んだ理由は、先頭の 12 行を出します。
- 合格した exe の署名(更新時刻とサイズ)は `<ビルドディレクトリ>\.ctest-pass-stamp.json` に記録します。
- 離席中(`DX12_BUILD_IDLE_MIN` 分以上無操作)は、既定の `-j3` が `-j6` になります(`-Jobs` で上書き)。
- 統合とゲートでは `-Changed` を使いません。

### 3.2 run_ui_tests.ps1 -Auto

```
pwsh tools/run_ui_tests.ps1 -Auto              # 変更に関係する UI テストだけ(-Only を自動で決める)
pwsh tools/run_ui_tests.ps1 -Auto -AutoPlan    # 選んだテストだけを JSON で出して終了(エンジンは起動しない)
```

- `src/editor/**` と `src/gui/**` の変更は、ファイル名の語(`HierarchyPanel` なら `Hierarchy`)を、テスト名とテスト本体の文字列から探して選びます。
  `tools/ui_tests_map.json` の rules(`only` と `categories`)で足せます。
- `UiTestHarness`、`uitests/`、テーマ、`EditorLayer`、CMake などの共通部分の変更は**全件**です。
- UI と関係ない変更でも、最低 1 本(`open_all_tool_windows`)は流します。
- GPU の枠を取ってから走らせます(§1)。

### 3.3 known_red(既知の赤)

`tools/known_red.json` に、既知の赤を書きます。統合担当が更新します。

```json
{
  "ctest": [ { "name": "SomeTests", "reason": "FU-E-9 待ち", "lane": "lane-a", "expires": "2026-12-15" } ],
  "ui":    [ { "name": "build_game", "reason": "Game.exe を前面起動する既知の問題", "expires": "2026-12-15" } ]
}
```

- 載っていて**期限内**のテストだけが赤のとき、`ctest.ps1` と `run_ui_tests.ps1` は終了コード 0 です。要約に「NEW RED / known red」を分けて出します。
- 期限が切れたものは、新しい赤として扱います。直ったテストは「now passing - remove」と出るので、一覧から消します。
- `-StrictRed`(または `DX12_KNOWN_RED_DISABLE=1`)で無視します。統合とゲートの全件確認はこれで回します。

## 4. アイテムキット(`tools/kit.ps1`)

出力は常に **40 行以内**です。全文は `build\kit-logs\*.log` に書き、最後の行にそのパスを出します。
前景で実行して、終わるまで待ってください(sleep や Monitor でのポーリングは要りません)。終了コードは中身のコマンドのものです。

| コマンド | 内容 |
|---|---|
| `status` | ブランチ、変更数、基準との差、離席か、枠の空き、エンジン、ビルドの古さ、使用中のポート |
| `build [-Tests] [-Target a,b] [-Dir ...] [-Dlss off]` | `tools/build.ps1`。OK/FAILED、最初のエラー、枠の待ち時間 |
| `test-changed [-Base ref] [-Gpu]` | `ctest.ps1 -Changed`。新しい赤と既知の赤を分けて表示 |
| `test [-R regex]` | CPU の ctest 全件 |
| `ui-auto [-Base ref]` | `run_ui_tests.ps1 -Auto` |
| `golden-subset <id,id,...> [-UiOnly]` | `node tools/bench/golden.mjs --only ...`。OK/FAIL の行と合否 |
| `engine-start [-Name n] [-Project dir] [-Port N] [-Mode background\|headless]` | 空きポート(8881〜8889)を選び、`--background` で起動して、ポートが開くまで待つ。プロジェクト省略時は使い捨て |
| `engine-stop [-Name n]` | 止める(GPU の枠も戻る) |
| `screenshot -Port N [-Out file] [-Scene]` | 起動中のエンジンから 1 枚。既定は ImGui 込み、`-Scene` で 3D だけ(決定論)。窓もマウスも使わない |
| `verify <項目ID> [-Base ref]` | `build -Tests` の後に `ctest -Changed` を回し、`build\kit\evidence\<ID>.txt` に結果を書く |

- ポートは **8881〜8889 だけ**を使います(`KIT_PORT_MIN` / `KIT_PORT_MAX` で変更)。8851〜8860 は、ほかのワークフロー用です。
- エンジンは必ず `--background`(窓を出さない・マウスを奪わない)です。
- 標準的な流れ:

```
pwsh tools/kit.ps1 status
pwsh tools/kit.ps1 build -Tests
pwsh tools/kit.ps1 test-changed
pwsh tools/kit.ps1 engine-start -Name mine          # 出力のポートを覚える
pwsh tools/kit.ps1 screenshot -Port 8881
pwsh tools/kit.ps1 engine-stop -Name mine
pwsh tools/kit.ps1 verify W2-A3                     # 証跡つきで最後の確認
```

## 5. 項目ブリーフ(`tools/briefs/make_brief.mjs`)

EXEC 文書(200〜350KB)から、1 項目ぶんを **8KB 以下**の 1 ファイルに抜き出します。

```
node tools/briefs/make_brief.mjs EXEC_W2.md --list                       # 項目 ID、レーン、行番号、見出し
node tools/briefs/make_brief.mjs EXEC_W2.md A3 --out briefs/A3.md        # 1 項目
node tools/briefs/make_brief.mjs EXEC_W2.md --all --out-dir briefs/      # 全項目(<ID>.md)
node tools/briefs/make_brief.mjs EXEC_W2.md A3 --max 6000 --lane A       # 上限とレーンの指定
```

入るもの(優先度の高い順。8KB を超えるときは、本文の中ほどから削ります):

1. 見出しと出典(ファイルの行範囲。「足りないときだけ、この範囲を読む」)
2. **受け入れ**(項目の「受け入れ」の段落。省略しません。最大 2.8KB)
3. **所有と凍結**(見出しに「所有」を含む節の表から、そのレーンの行または列と、「凍結」の行)
4. 文書の要約(表題と冒頭の箇条書き。最大 1KB)
5. 項目の本文(受け入れの段落を除いたもの。長いときは前半と末尾を残し、中ほどを「中略」にする)

項目は、`#`〜`######` の見出しで、先頭の語が ID と一致し、見出しに `（<レーン>、<S|M|L>=<点>、依存 …）` を持つものです。
レーンは見出しから読みます。読めなければ `--lane` で渡します。

## 6. テスト

```
pwsh tools/tests/slot_lock_test.ps1       # 枠(偽の無操作秒数。実 GPU もビルドも動かさない。約 40 秒)
pwsh tools/tests/build_env_test.ps1       # vcvars キャッシュと #deps 0 検査の省略
pwsh tools/tests/changed_map_test.ps1     # -Changed / -Auto / known_red の選択
pwsh tools/tests/kit_test.ps1             # kit.ps1 の出力と終了コード
pwsh tools/tests/build_slots_test.ps1     # 既存: build.ps1 のスロット
node --test tools/lib tools/briefs tools/bench
```

どれも、オーナーが作業中でも安全です(テスト専用の Mutex 接頭辞と偽の無操作秒数を使い、重い処理を走らせません)。
