# MCP フリート(複数エンジンの管理)設計追補

- 対象: `tools/mcp-server`(TS)と、エンジン側の最小の C++(`src/core/mcp/FleetGuard.h`・`src/main.cpp` の引数・`ping`)
- 位置づけ: `docs/MCP_ENHANCEMENT_DESIGN.md` の追補。M1〜M3 の流儀(マニフェスト・構造化エラー・surface 3 モード・Core の説明テンプレ)をそのまま使う。この上に次の担当が M6(ジョブ/進捗 API)を載せる。
- 作成: 2026-09-30 / 状態: 設計+実装済み(実装で分かったことは §14 に追記した)

---

## 0. 要約

エンジンの TCP ブリッジは**単一クライアント**で、複数のエージェント(Claude Code・Codex CLI の各セッション)が同じエンジンに繋ぐとハングか奪い合いになる。8850〜8855 を手で振り分け、ビルド中に exe を掴まれて `LNK1104` が出ていた。

決定した方式は **「エージェントごとに専用エンジン」**(共有エンジンにはしない)。

- 各セッション(=1 つの MCP サーバプロセス)が `dx12_engine_launch` で自分専用のエンジンを背景で起動する。ポート・作業フォルダ・exe コピー・データ領域(`DX12E_DATA_DIR`)・使い捨てプロジェクトが全部別。
- 全体で**最大 3 台**、**10 分操作が無ければ自動終了**。VRAM/RAM が足りなければ起動を断り、理由と対処(止める候補)を構造化エラーで返す。
- エンジンは**必ず `--background`(既定)か `--headless`**。画面に窓を出す `visible` は既定で拒否する。ユーザーの実マウス・フォーカス・前面ウィンドウを奪わない。
- 閉じ忘れても残らない: MCP サーバ側の監視、終了時の一括 kill、エンジン自身の `--owner-pid`(親が消えたら自殺)と `--idle-exit`、次回起動時の孤児スイープの 4 段。
- 従来運用(ユーザーが手でエンジンを起動して繋ぐ・`DX12_MCP_PORT`・ポートファイル探索)は**そのまま動く**。フリートは「束縛が無いときは従来の探索」の前段に足すだけ。

## 1. 用語

| 語 | 意味 |
|---|---|
| セッション | 1 つの MCP サーバ(Node)プロセス。`owner = {pid, startMs}` で識別する |
| フリート | このマシンで MCP サーバ同士が共有する「起動済みエンジンの一覧」(レジストリ) |
| 専用エンジン(managed) | フリートが起動し、レジストリに載っているエンジン。owner のセッションだけが操作する |
| 外部エンジン(external) | ユーザーが手で起動した、または別経路のエンジン。レジストリに載らない |
| 束縛(binding) | セッションの既定エンジン。束縛があれば全ツールがそこへ向く。無ければ従来の探索 |
| attach | 他人の/外部のエンジンを**読み取り専用**で見る |

## 2. ツール(すべて TS 専用ツール。エンジンの method は増やさない)

Core 面 40 本の上限を守るため、**Core に載せるのは 5 本**(launch / list / stop / attach / refresh)。`dx12_engine_use` は長尾(`dx12_call`)に置く(理由は §9)。full 面では 6 本とも tools/list の末尾に出る。旧 220 の名前・引数・返り値は変えない。

| ツール | 引数 | 効果 | effect |
|---|---|---|---|
| `dx12_engine_launch` | `name?` `project?` `mode?`(background\|headless\|visible。既定 background)`scene?` `dpiScale?` `args?` `waitReadyMs?` `confirm?` `keepProject?` | 専用エンジンを起動し、**このセッションの既定エンジンに束縛**する。`{engineId, port, pid, dir, mode, project}` を返す | runtime |
| `dx12_engine_list` | `discover?` | フリートの一覧(自分/他人/孤児)と資源(台数・上限・VRAM・RAM)。`discover:true` は手動起動の候補ポート(8787・8850〜8859・ポートファイル)を connect だけで探す | read |
| `dx12_engine_stop` | `engine?` `all?` `force?` `confirm?` | 自分のエンジンを止める(プロセスツリーごと kill)。他人のエンジンは `force`+`confirm` が無い限り拒否。孤児は誰でも回収できる | runtime |
| `dx12_engine_attach` | `port?` `engine?` `readOnly?`(既定 true)`confirm?` | 既存エンジンを見る。既定は**読み取り専用**(§5) | runtime |
| `dx12_engine_refresh` | `engine?` `waitReadyMs?` | ビルド後に exe コピーを最新へ差し替えて同じポート・プロジェクトで再起動する | runtime |
| `dx12_engine_use` | `engine` | 既定エンジンの切替。`"none"` で従来の探索に戻す | runtime |

- **共通の解決層**(`EngineRouter`): 全ツールが使う `engine`(EngineClient)を「現在の束縛先」へ振り分ける薄い層に差し替える。他の 220 本に `engine?` 引数を足さない(スキーマ・スナップショット不変)。1 回だけ別エンジンへ向けたいときは `dx12_call {engine:"<id>"}`(AsyncLocalStorage で呼び出し内だけ切替)。
- `engine` 引数は id(`e-3fa1`)・`name`・ポート(`8862`)のどれでも受け、外れたら近い候補を `didYouMean` で返す。
- `dx12_doctor` にフリート状態を統合(§8)。

## 3. レジストリ

置き場: `%LOCALAPPDATA%\UnoEngine\fleet\`(`DX12_FLEET_DIR` で差し替え)。

```
fleet/
  registry.json            {version:1, updatedAt, engines:{<id>:Entry}}
  registry.lock            排他ロック(下記)
  instances/<id>/bin/      exe コピー一式(cwd。dx12_engine.log・imgui.ini はここに出る)
  instances/<id>/data/     DX12E_DATA_DIR(recent.json・editor_state.json を実ユーザー領域から分離)
  projects/<id>/           使い捨てプロジェクト(停止後も 24 時間は残す)
  launch/<id>.log          起動直後の標準出力/エラー
```

`Entry`: `id` / `name` / `state`(starting・ready)/ `owner{pid,startMs,heartbeatAt}` / `pid` / `imageName` / `port` / `mode` / `project{dir,disposable}` / `exe{path,sourcePath,sourceMtimeMs,sizeBytes,copiedAt}` / `startedAt` / `lastActivityAt` / `idleExitMin` / `args`。

**同時アクセス**(複数の MCP サーバ=複数エージェントが同時に読み書きする)
- **排他**: `registry.lock` を `open(..., "wx")`(原子的な排他作成)で取る。中身は `{pid, startMs, token}`。取れなければジッタ付きで再試行(最大 10 秒)。
- **古いロックの回収**: 保持者の pid が死んでいる、または mtime が 15 秒より古いロックは、**`rename` で自分専用の名前へ移す**(原子的なので 1 人だけが成功する)→ 削除 → 取り直す。
- **原子的な置換**: 同じフォルダの一時ファイルへ書いてから `rename` で置換。読み取りはロック無しで、`ENOENT`/JSON 破損/`EPERM` は短く再試行する(置換の瞬間だけ起きる)。
- すべての変更は `transaction(fn)`(ロック→読む→`fn`→書く→解放)の中でだけ行う。**台数の上限判定・ポート割当・エントリ追加は 1 つのトランザクション**なので、同時に 4 つの `launch` が来ても 4 台目は必ず断られる。

## 4. ポートの自動割当

- 範囲: **8860〜8899**(`DX12_FLEET_PORT_RANGE` で変更可)。8850〜8859 は手動用、8787 は従来の既定なので触らない。
- トランザクション内で「レジストリに載っていない」かつ「OS が bind できる(一瞬 listen して閉じる)」最小のポートを `starting` として予約してから起動する。エンジンの bind が競合で失敗したら、その予約を捨てて次のポートで 1 回だけ再試行する。
- 予約は `state:"starting"` のまま 90 秒経つか、エンジンが死んだら掃除される。

## 5. attach(読み取り専用の閲覧)

エンジンの単一クライアント枠を**塞がない**のが最優先。

- 接続は**呼び出しごとの貸し出し**: 最後の応答から 1.5 秒でソケットを閉じる(`EngineClient.leaseMs`)。持ち主の MCP サーバや人が繋ぐ余地を空ける。持ち主が今つながっている間は connect は成功しても応答が来ない → 2.5 秒で `E_ENGINE_BUSY`(cause と fix つき)。
- 許可するのは**マニフェスト上 `effect:"read"` の method だけ**(マニフェストが取れなければ `get_/list_/describe_/ping/find_/query_` の前置)。それ以外は送信前に `E_FLEET_READONLY`。`readOnly:false` は `confirm:true` が要る(手動起動のエンジンに書き込み権つきで繋ぐ用途)。
- 束縛は**セッション内のみ**(レジストリに載せない)。id は `x-<port>`。

## 6. インスタンス分離と exe コピー

- 元: `DX12_FLEET_BUILD_DIR` → リポジトリの `build\release` → `%LOCALAPPDATA%\DX12Engine`。`DX12Engine.exe`・`*.dll`・`shaders\`・`assets\`(リポジトリ直下)を `instances\<id>\bin\` へコピーする。
- **ハードリンクにするのは `dxcompiler.dll`・`dxil.dll`・`GameRuntime.exe` だけ**(前 2 つは 1.5〜18 MB のサードパーティ製で再ビルドされない。`GameRuntime.exe` はエンジンが実行せず、`dx12_build_game` が出力へコピーする材料。コピーに含めないと UI 自動テストの `build_game` が落ちた)。**exe は必ず実コピー**: 実行中イメージはハードリンク名でも上書き・削除できないため、リンクにすると `LNK1104` が消えない。`shaders`・`assets` もビルドが上書きするので実コピー。
- これで**ビルドは `build\release\DX12Engine.exe` を自由に上書きできる**(実行中の exe は別の場所)。
- `exe.sourceMtimeMs` を覚え、元がそれより新しければ「古い exe コピー」(doctor が警告・`dx12_engine_refresh` で更新)。コピー中に元がリンク中だったら(サイズが変わる/読めない)短く再試行し、駄目なら `E_FLEET_BUILD_IN_PROGRESS`。
- 環境: `DX12E_DATA_DIR=instances\<id>\data`。それでも %APPDATA% を読む箇所(`SplashCommon`/`SplashScreen` の recent・editor_state の読み取り)と `%LOCALAPPDATA%\DX12Engine\shown_version.txt`(What's New の既読)は残る。書き込みは既読ファイルだけで、ユーザーのプロジェクト一覧・設定は汚さない。
- **プロジェクト**: 指定があればそのフォルダ(自動保存で書かれる旨を警告)。**同じフォルダを別のフリートエンジンが使っていたら `E_FLEET_PROJECT_IN_USE`**(2 台の自動保存が衝突する)。未指定なら使い捨てプロジェクトを `projects\<id>\` に生成する(`.dx12proj` + `assets/scenes/main.json`(太陽・カメラ・床)+ `scripts/`)。

## 7. 起動と後始末(壊れない・残らない)

起動引数(常にこの形。`--background`/`--headless` 以外は作らない):

```
background: DX12Engine.exe --background --project <dir> --mcp-port <p> --owner-pid <mcp pid> --idle-exit <分> --instance-id <id> [--dpi-scale x] [--scene s]
headless  : DX12Engine.exe --headless --virtual-input --project <dir> --mcp-port <p> ...(同上)
```

- 作業ディレクトリは `instances\<id>\bin`、CPU 優先度は **BelowNormal**(`os.setPriority`。ユーザーのビルド・ゲームを圧迫しない)、`windowsHide:true`。`--headless` には `--virtual-input` を併用する(自動更新チェックが走らないようにする)。
- **visible は既定で拒否**: 環境変数 `DX12_MCP_ALLOW_VISIBLE=1` **かつ** 呼び出しの `confirm:true` が両方要る。通っても「実マウスとフォーカスを奪い得る」警告を返す。どちらか欠けたら `E_FLEET_VISIBLE_DENIED`(何が足りないかを `fix` に書く)。
- **4 段の後始末**
  1. MCP サーバのアイドル監視: 束縛の有無に関わらず、自分のエンジンに 10 分(`DX12_FLEET_IDLE_MIN`)呼び出しが無ければ `stop`。
  2. MCP サーバ終了時: stdio クローズ・SIGINT・SIGTERM・未捕捉例外・`exit` で、**自分が owner のエンジン全部**を `taskkill /PID <pid> /T /F`(プロセスツリーごと)。`exit` は同期 `spawnSync`。
  3. エンジン自身: `--owner-pid`(1 秒ごとに親の生存を見て、消えたら自分で終了)と `--idle-exit`(`ping`・`describe_mcp_manifest` を除く MCP 呼び出しと実入力が無い時間)。MCP サーバが強制終了(TerminateProcess)されて 2 の処理が走らないときの保険。
  4. 孤児スイープ(launch / list / doctor / 30 秒ごとの監視で実行): owner の pid が死んでいる、またはハートビート(owner が 30 秒ごとに更新)が 5 分以上止まっているエントリのエンジンを kill し、エントリとインスタンスフォルダを消す。
- **kill の安全**: 殺すのは**レジストリに載っている pid だけ**で、その pid のイメージ名が記録(`DX12Engine.exe`)と一致することを `tasklist` で確認してから。名前で全部殺すことはしない(他のエージェントのエンジンを巻き込まない)。

## 8. リソースガード(起動前)

| 判定 | 既定 | 環境変数 | 根拠 |
|---|---|---|---|
| 台数(全セッション合計。生きているエントリ) | 3 | `DX12_FLEET_MAX` | ユーザー決定 |
| 空き VRAM | 2048 MB 未満は断る | `DX12_FLEET_MIN_FREE_VRAM_MB` | 実測 1 台あたりの使用量(§検証)と、ユーザーのゲーム/ビルドに残す余裕 |
| 空き RAM | 3072 MB 未満は断る | `DX12_FLEET_MIN_FREE_RAM_MB` | 同上 |

- VRAM: `nvidia-smi --query-gpu=memory.used,memory.total`(最大空きの GPU)→ 無ければ PowerShell の GPU カウンタ+レジストリの総量 → 取れなければ「不明」(通すが警告)。RAM: `os.freemem()`。
- 断るときは構造化エラー: `E_FLEET_LIMIT`(台数)/ `E_FLEET_RESOURCE`(VRAM/RAM)。`cause`(数値つき)・`fix`(**止める候補**: 自分のエンジンを idle が長い順に `dx12_engine_stop {engine}`。他人のものは操作せず `details.others` に owner・project・idleSec を載せ、「ユーザーに確認して」と書く)・`didYouMean`(止める候補の id)。
- テストと診断のため、資源の観測値は差し替えられる(`DX12_FLEET_FAKE_RESOURCES`)。

`dx12_doctor` のフリート節: 台数/上限・VRAM/RAM・エンジン一覧・古い exe コピー・孤児・束縛。問題は `issues`(`FLEET_STALE_EXE` / `FLEET_ORPHAN` / `FLEET_LOW_RESOURCES` / `FLEET_AT_LIMIT`)に fix つきで載せる。エンジンが落ちていて束縛が無いときの fix の先頭は `dx12_engine_launch`(手動コマンドはその次)。

## 9. Core 面の選定理由(40 本上限)

現在 35 本 + fleet 5 本 = **40 本(上限ちょうど)**。`dx12_engine_use` を外した理由: launch と attach が**束縛を自動で切り替える**ので、`use` が要るのは「自分が複数のエンジンを持って切り替える」場合だけで、頻度が最も低い。1 回だけ別エンジンを使うなら `dx12_call {engine}` で足りる。`refresh` は「ビルド → 更新」というこの機能の中心の流れなので Core に残す。入れ替えで既存 Core を外す案は、`discovery` の選択率(付録 A の Core タスク)を動かすので採らない。

## 10. 互換

- `DX12_MCP_PORT`・ポートファイル・既定 8787 の探索、旧 220 ツール、core/shell/full/legacy 面、`dx12_call` の挙動は不変。束縛が無ければ従来の `EngineClient` がそのまま使われる。
- `legacy` 面にはフリートのツールを出さない(旧 220 のバイト同一の回帰基準を守る)。
- 初期状態で自動起動はしない(`DX12_FLEET_AUTOLAUNCH=1` で「束縛が無く従来の探索も繋がらないとき、最初のエンジン呼び出しで専用エンジンを起動する」を有効化できる)。
- `DX12_FLEET_DISABLE=1` でフリートのツールと監視を丸ごと止められる。

## 11. 環境変数

`DX12_FLEET_DIR` / `DX12_FLEET_MAX`(3) / `DX12_FLEET_IDLE_MIN`(10。小数可) / `DX12_FLEET_MIN_FREE_VRAM_MB`(2048) / `DX12_FLEET_MIN_FREE_RAM_MB`(3072) / `DX12_FLEET_PORT_RANGE`(`8860-8899`) / `DX12_FLEET_BUILD_DIR` / `DX12_MCP_ALLOW_VISIBLE`(1 で visible を許可) / `DX12_FLEET_AUTOLAUNCH` / `DX12_FLEET_DISABLE` / テスト用: `DX12_FLEET_ENGINE_CMD`(エンジン起動コマンドの差し替え=偽エンジン)・`DX12_FLEET_FAKE_RESOURCES`。

## 12. エンジン側(C++)

- 起動引数 `--owner-pid <pid>` / `--idle-exit <分。小数可>` / `--instance-id <id>`。`src/core/mcp/FleetGuard.h`(ヘッダオンリー・注入可能な時計と生存判定)に判定を置き、`Application` のヘッダは触らない。
- `ping` の加算キー: `pid` `instanceId` `uptimeSec` `idleSec` `idleExitMin` `ownerPid` `vramUsedMB` `vramBudgetMB`(DXGI `QueryVideoMemoryInfo`。**このプロセスの使用量**)。`dpiScale` と `background` は既存。
- 終了は `PostQuitMessage` 経由(未保存で `aiSessionEver` なら先に `SaveSceneForMcp()`)。保存確認のモーダルは出ない。

## 13. 次(M6 ジョブ/進捗 API)への申し送り

- ジョブの永続先は `.dx12/jobs/` ではなく**エンジンごと**に持つ必要がある(専用エンジンはプロジェクトが使い捨て)。`dx12_job` は `engine` を持たせ、束縛先で実行する。
- 外部プロセスジョブ(cook 等)は `owner` と同じ規則で後始末する(`taskkill /T`・pid はレジストリ相当の記録に載せる)。フリートの `kill` ヘルパをそのまま使える。
- 進捗の中間行(`{"id","progress"}`)は EngineClient が既に読み捨てている。ルーターは `call` を素通しするので、そのまま載る。

## 14. 設計からの変更点(実装と実エンジンでの検証で分かったこと)

1. **ポートの割当は「予約 → ロックの外で bind 試験」**。当初は全 40 ポートの bind 試験を先にまとめて行ってからトランザクションに入れたが、複数セッションの同時 launch で試験同士がぶつかり(同じポートを一瞬 listen して閉じるため)「空きが無い」と誤判定した。レジストリで最小の未使用ポートを予約してから 1 件だけ試験し、使えなければ予約を捨てて次へ進む。予約中のポートは他のセッションが選ばないので試験がぶつからない。
2. **`dx12_engine_stop` は engine も all も無いとき、自分のエンジンが 1 台だけならそれ、複数なら断る**(束縛中のものを黙って止めない)。
3. **refresh は使い捨てプロジェクトを作り直さない**。起動処理が毎回プロジェクトを生成していたため、refresh で自動保存された作業内容が初期シーンで上書きされた(実エンジンで発見。`S1_Box` が消えた)。
4. **attach は、持ち主が接続を保持している他セッションのエンジンには繋がらない**(`E_ENGINE_BUSY`)。エンジンが単一クライアントである以上、持ち主の MCP サーバは接続を持ち続けるので、これは設計どおり(持ち主を邪魔しないための帰結)。他人のエンジンの状況は `dx12_engine_list`(レジストリ由来: project・idleSec・owner)で見る。attach が実際に効くのは、**誰も繋いでいない手動起動のエンジン**(実エンジンで確認: 読み取りだけ通り、1.5 秒後に枠が空き、人・別クライアントが繋げる)。
5. **`--idle-exit` は「エンジン側 = MCP 側の閾値 + 余裕」**にした(既定 10 分 + 1 分)。本線は MCP サーバの監視(`engine_stop` としてレジストリ・インスタンスまで片付く)で、エンジン側は MCP サーバが強制終了された場合の保険。
6. `dx12_call {engine}` の呼び出し内では、マニフェストの取り直し(別エンジンの表で上書きしない)を止める。
7. 終了処理はプロセス終了フック(stdio クローズ・シグナル・未捕捉例外・`exit`)を**フリートを最初に使ったときだけ**入れる(フリートを使わない運用は従来と同じ挙動)。`exit` からは同期の `taskkill` とインスタンスフォルダの削除(殺した直後は cwd のハンドルが残るので数回再試行)を行う。
8. Core 面は 40 本ちょうど。将来 Core に足す(`dx12_editor_command` など)ときは、選定理由つきで何かを長尾へ移す必要がある。

## 15. 起動から停止までの流れ(1 セッションの視点)

```
dx12_engine_launch
  ├─ 引数検査(mode・name・args の管理対象フラグ・dpiScale)/ visible の門(環境変数 + confirm)
  ├─ 孤児スイープ(owner 死亡・エンジン死亡のエントリを掃除)
  ├─ 元の exe を探す(DX12_FLEET_BUILD_DIR → build
elease → インストール先)
  ├─ 台数の事前確認 → 資源の観測(nvidia-smi / RAM)→ 閾値判定           … ここで断ると何も作らない
  ├─ [ロック] 台数の再判定 + 同一プロジェクト検査 + ポート予約(state:"starting") [解放]
  ├─ ロックの外で bind 試験(使えなければ予約を捨てて次のポート)
  ├─ instances\<id>\bin へコピー(exe は実コピー / dxil・dxcompiler はハードリンク)+ 使い捨てプロジェクト生成
  ├─ spawn(cwd=bin・BelowNormal・DX12E_DATA_DIR・--owner-pid ...)→ pid をレジストリへ
  ├─ ping 応答を待つ(起動直後に落ちたら launch ログの末尾つきで E_FLEET_LAUNCH_FAILED。失敗時は予約・フォルダ・プロセスを全部片付ける)
  └─ state:"ready" → EngineRouter に束縛 → 監視タイマー開始
監視(5 秒ごと) : ハートビート更新 / 自分のエンジンのアイドル(10 分)→ stop / 死んだエンジンの掃除 / 孤児スイープ
停止           : taskkill /T /F(イメージ名を確認)→ エントリ削除 → 束縛解除 → instances\<id> 削除(projects\<id> は 24 時間残す)
```

## 16. エラーコード(フリート)

| コード | 発生条件 | `fix` の中身 |
|---|---|---|
| `E_FLEET_LIMIT` | 全セッション合計が上限 / ポート範囲が尽きた | 自分の idle なエンジンの `dx12_engine_stop`(idle が長い順)・`dx12_engine_use`。他人のものは `details.others` に載せるだけ |
| `E_FLEET_RESOURCE` | 空き VRAM / RAM が下限未満 | 同上 + 環境変数での閾値変更(`cause`) |
| `E_FLEET_VISIBLE_DENIED` | visible に環境変数か confirm が無い | `mode:"background"` で撃ち直す / (ユーザーが)環境変数を設定 / confirm 付きで撃ち直す |
| `E_FLEET_READONLY` | 読み取り専用の束縛先へ書き込み系を送った | `dx12_engine_launch`(自分専用)/ 承認つきの書き込み権つき attach |
| `E_FLEET_NOT_FOUND` | id / name / port がフリートに無い | `didYouMean`(id・name・port の近いもの)+ `dx12_engine_list` |
| `E_FLEET_NOT_OWNER` | 他のセッションのエンジンを止める・use する・refresh する | `dx12_engine_list` / `dx12_engine_attach`(読み取り専用)/ `force`+`confirm`(最後の手段) |
| `E_FLEET_PROJECT_IN_USE` | 同じプロジェクトを別のエンジンが使用中 | 既存エンジンの `dx12_engine_use` / `project` を省略 |
| `E_FLEET_BUILD_IN_PROGRESS` | 元の exe がビルド中でコピーできない | ビルド完了後に撃ち直す(動いているエンジンは止めない) |
| `E_FLEET_LAUNCH_FAILED` | 起動直後に終了 / ping に応答しない / 準備の失敗 | `waitReadyMs` を延ばして撃ち直す・`dx12_doctor`。`details.logTail` に launch ログ |
| `E_FLEET_EXE_MISSING` | コピー元の `DX12Engine.exe` が無い | `tools\build.ps1` / `DX12_FLEET_BUILD_DIR` |
| `E_FLEET_DISABLED` | `DX12_FLEET_DISABLE=1` | 環境変数を外す |

既存コードの流用: 何も居ないポートへの attach は `E_ENGINE_UNREACHABLE`、持ち主が接続中の attach は `E_ENGINE_BUSY`、書き込み権つき attach の承認漏れは `E_GUARDED`、引数の不備は `E_INVALID_PARAM` / `E_BAD_ENUM` / `E_OUT_OF_RANGE` / `E_MISSING_PARAM`。

## 17. 検証計画(合否基準)

1. **模擬エンジン(偽エンジンのプロセス。`mockEngineProc.ts`)**: 上限 3 台の強制と理由付きエラー / ポートの自動割当の重複なし(5 セッション一斉起動でちょうど 3 台成功)/ レジストリの同時アクセス(別プロセス 4 つ × 60 回の更新が 1 つも失われず、予約ポートが重複しない)/ 孤児回収と kill の安全(イメージ名が違えば殺さない)/ `--owner-pid` と `--idle-exit`(ping は活動に数えない)/ MCP 側のアイドル自動終了(時間短縮)/ 資源不足(VRAM・RAM の模擬)/ visible の拒否 / MCP サーバ終了(正常・強制)での後始末。
2. **実エンジン**: 別セッション相当の MCP サーバ 2〜4 つで、別ポート・別 exe コピー・別データ領域で同時に動き、独立に操作(spawn・スクリーンショット)できる。3 台目まで通り 4 台目は理由付きで断られる。`tools\build.ps1` を並行して走らせても `LNK1104` にならず、`dx12_engine_refresh` で最新の exe に入れ替わる(SHA-256 一致)。停止・サーバ終了・強制終了・アイドルの後に `tasklist` に自分のエンジンが残らない。
3. **人の操作を奪わない証明**: 実行中ずっと、読み取り専用の見張り(50 ms ごとに `GetCursorPos` / `GetForegroundWindow` を記録)を並走させ、前面ウィンドウがエンジンの pid にならないことを確かめる。
4. 既存の全 TS テスト・ctest・UI 自動テスト(`--background --ui-tests-run-all`)が通る。旧 220 ツールの `legacy` 面はバイト同一のまま。

## 18. 未対応

- エンジン間の実行中の共有(ジョブ・進捗)は M6。専用エンジンごとに独立している前提で載せる。
- 他セッションのエンジンを「見る」経路(持ち主が接続を持つ間の閲覧)は、エンジンが単一クライアントである限り作れない。必要ならエンジン側に読み取り専用の第 2 ポートを持たせる案があるが、今回は範囲外。
- `visible` の実機での前面化の検証は行っていない(既定で拒否のため、見張りの対象は background と headless だけ)。

