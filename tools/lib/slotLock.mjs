// Node 用の枠(スロット)ロック。実体は tools/lib/SlotLock.psm1 の名前付き Mutex で、
// 「持つ専用プロセス」tools/lib/slot_hold.ps1 を子として立てて、PowerShell 側(run_ui_tests.ps1 など)と
// 同じ Mutex を共有する(Node には名前付き Mutex が無いため)。docs/AGENT_KIT.md「枠」の節を参照。
//
//   import { acquireSlot } from "../lib/slotLock.mjs";
//   const slot = await acquireSlot("gpu", { label: "golden" });
//   try { ... } finally { slot.release(); }
//
// - DX12_SLOTS_DISABLE=1 なら何もしない(従来の挙動)。親が既に取っている(DX12_SLOT_HELD_<KIND>)なら取らずに通す。
// - 取れたら process.env に DX12_SLOT_HELD_<KIND> / DX12_SLOT_PRIORITY を立てる(以降に起動する子が二重に取らない)。
// - スロット 2 以降(離席中)で取れたときは、この Node プロセスを Idle 優先度(PRIORITY_LOW)に下げる。
// - 取れなければ(待ち時間切れ)例外 code="SLOT_TIMEOUT"。呼び出し側は終了コード 3 にする。
import { spawn } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const HOLD_SCRIPT = path.join(HERE, "slot_hold.ps1");

const noop = (kind, extra = {}) => ({ kind, slot: 0, count: 0, priority: "BelowNormal", nested: false, disabled: false, waitedSec: 0, release() {}, ...extra });

/** 枠を取る。options: { label, waitMin=40, env=process.env, log=console.error, shell="pwsh" } */
export async function acquireSlot(kind, options = {}) {
  const { label = "", waitMin = 40, env = process.env, log = (s) => console.error(s), shell = "pwsh" } = options;
  const up = kind.toUpperCase();
  if (env.DX12_SLOTS_DISABLE === "1") return noop(kind, { disabled: true });
  if (/^\d+$/.test(env[`DX12_SLOT_HELD_${up}`] ?? "")) return noop(kind, { nested: true, slot: Number(env[`DX12_SLOT_HELD_${up}`]), priority: env.DX12_SLOT_PRIORITY ?? "BelowNormal" });

  const child = spawn(shell, ["-NoProfile", "-File", HOLD_SCRIPT, "-Kind", kind, "-Label", label, "-WaitMinutes", String(waitMin), "-ParentPid", String(process.pid)],
    { stdio: ["ignore", "pipe", "pipe"], windowsHide: true, env });
  let buf = "";
  const info = await new Promise((resolve, reject) => {
    const onLine = (line) => {
      const m = /^ACQUIRED slot=(\d+)\/(\d+) priority=(\w+) waited=(\d+)/.exec(line);
      if (m) return resolve({ slot: +m[1], count: +m[2], priority: m[3], waitedSec: +m[4] });
      if (line.trim() === "TIMEOUT") { const e = new Error(`${kind} の枠が ${waitMin} 分以内に取れなかった`); e.code = "SLOT_TIMEOUT"; return reject(e); }
      if (line.trim()) log(line);
    };
    child.stdout.on("data", (d) => { buf += d.toString("utf8"); let i; while ((i = buf.indexOf("\n")) >= 0) { onLine(buf.slice(0, i).replace(/\r$/, "")); buf = buf.slice(i + 1); } });
    child.stderr.on("data", (d) => log(String(d).trimEnd()));
    child.once("error", reject);
    child.once("exit", (code) => { const e = new Error(`slot_hold が枠を取る前に終了した(${code})`); e.code = code === 3 ? "SLOT_TIMEOUT" : "SLOT_ERROR"; reject(e); });
  });
  child.removeAllListeners("exit");

  const prevHeld = process.env[`DX12_SLOT_HELD_${up}`], prevPrio = process.env.DX12_SLOT_PRIORITY;
  process.env[`DX12_SLOT_HELD_${up}`] = String(info.slot);
  process.env.DX12_SLOT_PRIORITY = info.priority;
  if (info.priority === "Idle") { try { os.setPriority(0, os.constants.priority.PRIORITY_LOW); } catch { /* 無視 */ } }
  let released = false;
  const release = () => {
    if (released) return; released = true;
    if (prevHeld === undefined) delete process.env[`DX12_SLOT_HELD_${up}`]; else process.env[`DX12_SLOT_HELD_${up}`] = prevHeld;
    if (prevPrio === undefined) delete process.env.DX12_SLOT_PRIORITY; else process.env.DX12_SLOT_PRIORITY = prevPrio;
    try { child.kill(); } catch { /* 既に終了 */ }
  };
  process.once("exit", release);
  return noop(kind, { ...info, release });
}

/** 枠を取ってから fn を実行し、必ず解放する。取れなければ SLOT_TIMEOUT を投げる。 */
export async function withSlot(kind, options, fn) {
  const slot = await acquireSlot(kind, options);
  try { return await fn(slot); } finally { slot.release(); }
}
