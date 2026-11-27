// FU-E-38: 実エンジンを相手にした E2E（C14 の取りこぼし修正の 5 点）。
//   [1] new_scene の実際の結果（sceneName / entityCount / sceneGeneration / saved / path / projectPath）
//   [2] new_scene を 2 回続けて呼ぶと 2 回目は E_MODE_CONFLICT（1 回目は成功し、世代は 1 つだけ進む）
//   [3] create_entity: type の欠落 = E_MISSING_PARAM、rotation = E_UNKNOWN_PARAM（didYouMean に position）。どちらも何も作らない
//   [4] create_entity: decal の parentName / parent で親子になる。存在しない parentName は E_NOT_FOUND_ENTITY
//   [5] create_entity: particle_emitter は "Layer 1" を 1 枚持つ
//   [6] set_upscaler を dlss → off → dlss と切り替えても、DLSS が使えない理由の警告は 1 回だけ（DLSS が使えるなら 0 回）
//
// 実行（エンジンが要る。無ければ SKIP して 0 で終わる。窓は出さない / マウスに触らない）:
//   既に起動している専用エンジンに繋ぐ:  DX12_E2E_PORT=<port> node engineE2e.test.ts
//   このテストが起動して終わりに止める:  DX12_E2E_PORT=<port> DX12_E2E_BUILD_DIR=<DX12Engine.exe のあるビルドフォルダ> node engineE2e.test.ts
//     （tools/engine_instance.ps1 を headless で使う。ビルドはしない。使い捨てのプロジェクトを一時フォルダに作る）
// ★使い捨てのプロジェクトのエンジンにだけ繋ぐこと（new_scene でシーンを作り直し、シーンのファイルを 1 つ書く）。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import "./testEnv.ts";
import { EngineClient, probePort } from "./engineClient.ts";
import { classifyCode } from "./errors.ts";
import { startMcp } from "./stdioClient.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const portEnv = Number(process.env.DX12_E2E_PORT ?? "");
const buildDir = process.env.DX12_E2E_BUILD_DIR ?? "";
const HOST = "127.0.0.1";

let failed = 0, total = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  total++;
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${typeof detail === "string" ? detail : JSON.stringify(detail).slice(0, 900)}` : ""}`); }
}
const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

if (!Number.isFinite(portEnv) || portEnv <= 0) {
  console.log("SKIP: DX12_E2E_PORT が未設定（実エンジンが要る E2E。上の「実行」を参照）");
  process.exit(0);
}

const instName = `e2e-fu38-${process.pid}`;
let projDir = "";
let ownEngine = false;

function engineInstance(args: string[]) {
  const script = path.resolve(here, "..", "engine_instance.ps1");
  return spawnSync("pwsh", ["-NoProfile", "-File", script, ...args], { encoding: "utf8" });
}

async function startOwnEngine(): Promise<void> {
  ownEngine = true; // 起動に失敗しても停止と掃除が走るよう、先に立てる
  projDir = fs.mkdtempSync(path.join(os.tmpdir(), "dx12-e2e-fu38-"));
  fs.mkdirSync(path.join(projDir, "assets"), { recursive: true });
  fs.mkdirSync(path.join(projDir, "scripts"), { recursive: true });
  fs.writeFileSync(path.join(projDir, "e2e.dx12proj"), JSON.stringify({ assetsDir: "assets", defaultScene: "", lastOpenedScene: "", name: "e2e", scriptsDir: "scripts", version: "0.1.0" }, null, 2));
  const r = engineInstance(["-Name", instName, "-Port", String(portEnv), "-Project", projDir, "-Mode", "headless", "-BuildDir", buildDir, "-IdleExitMin", "20"]);
  if (r.status !== 0) throw new Error(`engine_instance.ps1 が失敗: ${r.stderr || r.stdout}`);
}

// 起動のたびに約 150 MB のインスタンスフォルダ（exe / DLL / shaders / assets のコピー）が残るので、停止後に消す。
// プロセスが抜けてもハンドルの解放が遅れるので、少し待って再試行する。
function stopOwnEngine() {
  if (!ownEngine) return;
  engineInstance(["-Name", instName, "-Stop"]);
  ownEngine = false;
  const instDir = path.join(process.env.LOCALAPPDATA ?? "", "UnoEngine", "agent-instances", instName);
  const rm = (d: string) => {
    for (let i = 0; i < 15; i++) {
      try { fs.rmSync(d, { recursive: true, force: true }); if (!fs.existsSync(d)) return; } catch { /* ハンドル待ち */ }
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1000);
    }
    console.log(`  WARN  削除できなかった: ${d}`);
  };
  if (process.env.LOCALAPPDATA && instName.startsWith("e2e-fu38-")) rm(instDir);
  if (projDir) rm(projDir);
}

async function waitPing(c: EngineClient, ms = 90000): Promise<any> {
  const t0 = Date.now();
  let last: unknown;
  while (Date.now() - t0 < ms) {
    try {
      const p = await c.call("ping", {});
      if (!p.sceneLoading) return p;
      last = "sceneLoading";
    } catch (e) { last = e; }
    await sleep(500);
  }
  throw new Error(`エンジンが ping に答えない: ${String((last as any)?.message ?? last)}`);
}

async function expectErr(c: EngineClient, method: string, params: Record<string, unknown>): Promise<any | null> {
  try { await c.call(method, params); return null; } catch (e) { return e; }
}

let exitCode = 0;
let client: EngineClient | null = null;
try {
  if (await probePort(HOST, portEnv) !== "open") {
    if (!buildDir) { console.log(`SKIP: ポート ${portEnv} にエンジンが居ない（DX12_E2E_BUILD_DIR があればこのテストが起動する）`); process.exit(0); }
    await startOwnEngine();
  }
  const c = client = new EngineClient(HOST, portEnv);
  const ping0 = await waitPing(c);
  console.log(`エンジン: ${ping0.engineVersion} pid=${ping0.pid} project=${ping0.baseDir} (own=${ownEngine})`);
  // 使い捨てのプロジェクトだけを相手にする（自前で起動した場合は無条件、既存のエンジンなら一時フォルダ配下であること）
  const under = (child: string, parent: string) => { const r = path.relative(path.resolve(parent), path.resolve(child)); return !!r && !r.startsWith("..") && !path.isAbsolute(r); };
  const agentBase = path.join(process.env.LOCALAPPDATA ?? "-", "UnoEngine", "agent-instances");
  const disposable = under(String(ping0.baseDir), os.tmpdir()) || under(String(ping0.baseDir), agentBase) || process.env.DX12_E2E_ALLOW_ATTACH === "1";
  if (!ownEngine && !disposable) {
    console.log(`NG: 使い捨てのプロジェクトではなさそう（${ping0.baseDir}）。中止する（承知の上なら DX12_E2E_ALLOW_ATTACH=1）`);
    process.exit(3);
  }
  const assetsDir = String(ping0.assetsDir);
  const entityCount = async () => (await c.call("list_entities", {})).count as number;

  // ---------------------------------------------------------------- [1]
  console.log("[1] new_scene の実際の結果");
  {
    const g0 = (await c.call("ping", {})).sceneGeneration as number;
    const r = await c.call("new_scene", {});
    check("未保存の新規シーン: sceneName=Untitled・saved=false・path/projectPath=null", r.sceneName === "Untitled" && r.saved === false && r.path === null && r.projectPath === null, r);
    check("sceneGeneration が数値で 1 つ進む", typeof r.sceneGeneration === "number" && r.sceneGeneration === g0 + 1, { g0, r });
    const n = await entityCount();
    check("entityCount が切り替え後の実数と一致", r.entityCount === n, { r, n });
    const rel = "scenes/e2e_fu38.json";
    const r2 = await c.call("new_scene", { savePath: rel });
    check("savePath あり: sceneName=ファイル名の語幹・saved=true", r2.sceneName === "e2e_fu38" && r2.saved === true, r2);
    check("path は assets 相対・projectPath はプロジェクト相対（2 つのキーを混ぜない）", r2.path === rel && r2.projectPath === `assets/${rel}`, r2);
    check("世代がさらに 1 つ進む", r2.sceneGeneration === r.sceneGeneration + 1, r2);
    check("シーンのファイルが実際に書かれている", fs.existsSync(path.join(assetsDir, rel)), assetsDir);
    const p = await c.call("ping", {});
    check("ping の currentScene / currentSceneProjectPath が結果と同じ", p.currentScene === rel && p.currentSceneProjectPath === `assets/${rel}`, p);
    const bad = await expectErr(c, "new_scene", { savePath: "../escape.json" });
    check("assets の外を指す savePath は拒否", !!bad && bad.errName !== "E_ENGINE_TIMEOUT", bad?.message);
  }

  // ---------------------------------------------------------------- [2]
  console.log("[2] new_scene を 2 回続けて呼ぶ");
  {
    // 待たずに 2 本続けて送る（1 本目がフレーム境界で処理される前に 2 本目が届く）。速いフレームだと 2 本とも成功するので、競合が出るまで数回やり直す
    let seen = false;
    for (let attempt = 1; attempt <= 8 && !seen; attempt++) {
      const g0 = (await waitPing(c)).sceneGeneration as number;
      const [a, b] = await Promise.allSettled([c.call("new_scene", {}), c.call("new_scene", {})]);
      if (a.status === "fulfilled" && b.status === "fulfilled") { await sleep(200); continue; }
      seen = true;
      check("1 回目は成功し、実際の結果を返す", a.status === "fulfilled" && (a as any).value.sceneGeneration === g0 + 1, a);
      const be: any = b.status === "rejected" ? b.reason : null;
      // エンジンは数値コード 3（ModeConflict）で返し、MCP サーバが classifyCode で E_MODE_CONFLICT にする
      const name = b.status === "rejected" ? classifyCode(be?.code, String(be?.message ?? ""), be?.errName) : "";
      check("2 回目は E_MODE_CONFLICT", name === "E_MODE_CONFLICT", b.status === "rejected" ? { code: be?.code, errName: be?.errName, message: be?.message } : b);
      check("2 回目の hint が「ping の sceneLoading を待つ」を案内している", /sceneLoading/.test(String(be?.hint ?? "")), { hint: be?.hint });
      await sleep(300);
      const p = await waitPing(c);
      check("世代は 1 つしか進んでいない（2 回目は何も適用されていない）", p.sceneGeneration === g0 + 1, { g0, gen: p.sceneGeneration });
      const ok = await c.call("new_scene", {});
      check("落ち着いたあとの new_scene は通る", ok.sceneGeneration === g0 + 2, ok);
    }
    if (!seen) console.log("  NOT-VERIFIED: E_MODE_CONFLICT を 8 回試して一度も観測できなかった（2 本とも成功した）");
  }

  // ---------------------------------------------------------------- [3]
  console.log("[3] create_entity の type の欠落と rotation");
  {
    const n0 = await entityCount();
    const e1 = await expectErr(c, "create_entity", { name: "NoType" });
    check("type が無い → E_MISSING_PARAM（黙って box にしない）", e1?.errName === "E_MISSING_PARAM", { errName: e1?.errName, message: e1?.message });
    const e2 = await expectErr(c, "create_entity", { type: "box", name: "Rot", rotation: [0, 90, 0] });
    check("rotation → E_UNKNOWN_PARAM", e2?.errName === "E_UNKNOWN_PARAM", { errName: e2?.errName, message: e2?.message });
    check("didYouMean に position が出る", Array.isArray(e2?.didYouMean) && e2.didYouMean.includes("position"), e2?.didYouMean);
    const e3 = await expectErr(c, "create_entity", { type: "bogus" });
    check("未知の type は InvalidParam で、種別の一覧を返す", !!e3 && /box/.test(String(e3.message)), e3?.message);
    await sleep(200);
    check("どの失敗でもエンティティは増えていない", (await entityCount()) === n0, { n0 });
    const ok = await c.call("create_entity", { type: "box", name: "TypedBox", position: [1, 2, 3] });
    const got = await c.call("get_entity", { entity: ok.entityId });
    check("正しい呼び出しは作られ、位置が反映される", got.name === "TypedBox" && Math.abs(got.transform.position[1] - 2) < 1e-4, got.transform);
  }

  // ---------------------------------------------------------------- [4]
  console.log("[4] decal の parentName / parent");
  {
    const par = await c.call("create_entity", { type: "empty", name: "E2eParent", position: [4, 0, 0] });
    const dec = await c.call("create_entity", { type: "decal", name: "E2eDecal", parentName: "E2eParent", position: [0, 1, 0] });
    const h = await c.call("get_hierarchy", {});
    const node = (h.roots as any[]).find((r) => r.name === "E2eParent");
    check("get_hierarchy で decal が親の子になっている（ルートに居ない）",
      !!node && (node.children ?? []).some((ch: any) => ch.entityId === dec.entityId) && !(h.roots as any[]).some((r) => r.name === "E2eDecal"), h.roots);
    const dec2 = await c.call("create_entity", { type: "decal", name: "E2eDecalById", parent: par.entityId });
    const h2 = await c.call("get_hierarchy", {});
    const node2 = (h2.roots as any[]).find((r) => r.name === "E2eParent");
    check("parent（entityId）でも子になる", !!node2 && (node2.children ?? []).some((ch: any) => ch.entityId === dec2.entityId), node2);
    const miss = await expectErr(c, "create_entity", { type: "decal", parentName: "NoSuchParent_e2e" });
    check("存在しない parentName → E_NOT_FOUND_ENTITY", miss?.errName === "E_NOT_FOUND_ENTITY", { errName: miss?.errName, message: miss?.message });
    const cv = await expectErr(c, "create_entity", { type: "ui_canvas", parentName: "E2eParent" });
    check("ui_canvas に parentName は拒否（常にルート）", !!cv && /ui_canvas/.test(String(cv.message)), cv?.message);
  }

  // ---------------------------------------------------------------- [5]
  console.log("[5] particle_emitter の Layer 1");
  {
    const em = await c.call("create_entity", { type: "particle_emitter", name: "E2eEmitter" });
    const l = await c.call("list_particle_layers", { entity: em.entityId });
    check("既定の放出器にレイヤーが 1 枚（名前は Layer 1）", l.count === 1 && l.layers?.[0]?.name === "Layer 1", l);
    // FU-E-39 (C2): 3 層の放出器へ layer:7 → E_OUT_OF_RANGE / details {field,value,max:2} / 何も変わらない
    await c.call("add_particle_layer", { entity: em.entityId });
    await c.call("add_particle_layer", { entity: em.entityId });
    const l3 = await c.call("list_particle_layers", { entity: em.entityId });
    check("add_particle_layer x2 で 3 層", l3.count === 3, l3);
    const ent0 = JSON.stringify(await c.call("get_entity", { entity: em.entityId }));
    const lay0 = JSON.stringify(l3);
    const oor = await expectErr(c, "set_component", { entity: em.entityId, component: "particleEmitter", data: { layer: 7, rate: 99 } });
    check("3 層へ layer:7 → E_OUT_OF_RANGE + details {field:layer,value:7,max:2}",
      oor?.errName === "E_OUT_OF_RANGE" && oor.errDetails?.field === "layer" && oor.errDetails.value === 7 && oor.errDetails.max === 2 && Object.keys(oor.errDetails).sort().join() === "field,max,value", { errName: oor?.errName, errDetails: oor?.errDetails });
    check("拒否後も get_entity / list_particle_layers が呼ぶ前と同一",
      JSON.stringify(await c.call("get_entity", { entity: em.entityId })) === ent0 && JSON.stringify(await c.call("list_particle_layers", { entity: em.entityId })) === lay0);
    await c.call("set_component", { entity: em.entityId, component: "particleEmitter", data: { layer: 2, rate: 99 } });
    const lAfter = (await c.call("list_particle_layers", { entity: em.entityId })).layers;
    check("範囲内の layer:2 は 2 層目だけを書く(rate=99、0・1 層は不変)",
      lAfter?.[2]?.rate === 99 && lAfter[0].rate === l3.layers[0].rate && lAfter[1].rate === l3.layers[1].rate, lAfter);
  }

  // ---------------------------------------------------------------- [5b]
  console.log("[5b] MCP サーバのツール層（dx12_create_entity / dx12_new_scene を stdio 越しに）");
  {
    const mc = startMcp({ DX12_MCP_PORT: String(portEnv) });
    try {
      await mc.initialize();
      const n0 = await entityCount();
      const miss = await mc.raw("dx12_create_entity", { name: "ViaToolNoKind" });
      const errText = JSON.stringify(miss?.content ?? miss);
      check("ツール層: type 欠落は isError", miss?.isError === true, miss);
      check("ツール層: エラーが type フィールドを名指しする", /E_MISSING_PARAM|"type"|type/.test(errText.replace(/ViaToolNoKind/g, "")), miss);
      await sleep(200);
      check("ツール層: type 欠落でエンティティは増えていない", (await entityCount()) === n0, { n0 });
      const ok = await mc.call("dx12_create_entity", { type: "box", name: "ViaTool" });
      check("ツール層: type 付きの create_entity が通る", /entityId/.test(JSON.stringify(ok)), ok);
      const ns = await mc.call("dx12_new_scene", {});
      check("ツール層: new_scene が sceneName / sceneGeneration を返す", /Untitled/.test(JSON.stringify(ns)) && /sceneGeneration/.test(JSON.stringify(ns)), ns);
    } finally { mc.close(); }
  }

  // ---------------------------------------------------------------- [6]
  console.log("[6] set_upscaler の dlss → off → dlss で警告は 1 回だけ");
  {
    const WARN = /アップスケーラ: DLSS を使えないので TAA で描く/;
    const logLines = async () => (await c.call("get_log", { lines: 5000 })) as string[];
    const before = new Set((await logLines()).filter((l) => WARN.test(l)));
    const available = !!(await c.call("get_upscaler", {})).dlss?.available;
    const frames = async () => { await c.call("step_frames", { frames: 12 }); };
    for (const mode of ["dlss", "off", "dlss", "off", "dlss"]) {
      await c.call("set_upscaler", { mode });
      await frames();
    }
    const after = (await logLines()).filter((l) => WARN.test(l) && !before.has(l));
    if (available) check("DLSS が使える環境: 縮退の警告は 0 回", after.length === 0, after);
    else if (ownEngine) check("DLSS が使えない環境: 警告はちょうど 1 回（切り替えを繰り返しても増えない）", after.length === 1, after);
    else check("DLSS が使えない環境: 警告は増えても 1 回まで（起動済みのエンジンでは理由が前と同じなら 0）", after.length <= 1, after);
    await c.call("set_upscaler", { mode: "off" });
  }
} catch (e: any) {
  failed++;
  console.log(`  NG  例外: ${String(e?.stack ?? e).slice(0, 1200)}`);
} finally {
  try { client?.close(); } catch { /* ignore */ }
  stopOwnEngine();
}

console.log(failed === 0 ? `\nOK: ${total} checks` : `\nFAILED: ${failed}/${total}`);
exitCode = failed === 0 ? 0 : 1;
process.exit(exitCode);
