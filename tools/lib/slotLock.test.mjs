// slotLock.mjs の単体テスト(テスト専用の Mutex 接頭辞と偽の無操作秒数。実 GPU は使わない)
//   node --test tools/lib
import test from "node:test";
import assert from "node:assert/strict";
import { acquireSlot } from "./slotLock.mjs";

const tag = Math.random().toString(16).slice(2, 10);
const base = { ...process.env, DX12_TEST_SLOT_PREFIX: `Global\dx12-test-nodetest-${tag}`, DX12_TEST_SLOTS: "1",
  DX12_BUILD_FAKE_IDLE_SEC: "5", DX12_BUILD_MIN_FREE_GB: "0" };
delete base.DX12_SLOTS_DISABLE; delete base.DX12_SLOT_HELD_TEST;
const quiet = () => {};

test("枠を取って解放すると次が取れる / 取れている間は SLOT_TIMEOUT", async () => {
  const a = await acquireSlot("test", { env: base, log: quiet, waitMin: 0 });
  assert.equal(a.slot, 1);
  assert.equal(process.env.DX12_SLOT_HELD_TEST, "1");
  // 子の環境から HELD を外して(= 別プロセス相当)取りに行く
  const other = { ...base }; delete other.DX12_SLOT_HELD_TEST;
  await assert.rejects(() => acquireSlot("test", { env: other, log: quiet, waitMin: 0, ...{} }), (e) => e.code === "SLOT_TIMEOUT" || e.code === "SLOT_ERROR");
  a.release();
  assert.equal(process.env.DX12_SLOT_HELD_TEST, undefined);
  await new Promise((r) => setTimeout(r, 1500));
  const b = await acquireSlot("test", { env: other, log: quiet, waitMin: 0 });
  assert.equal(b.slot, 1);
  b.release();
});

test("DX12_SLOTS_DISABLE=1 は何もしない / 親が持っていれば入れ子で通る", async () => {
  const d = await acquireSlot("test", { env: { ...base, DX12_SLOTS_DISABLE: "1" }, log: quiet });
  assert.equal(d.disabled, true);
  const n = await acquireSlot("test", { env: { ...base, DX12_SLOT_HELD_TEST: "2", DX12_SLOT_PRIORITY: "Idle" }, log: quiet });
  assert.equal(n.nested, true);
  assert.equal(n.slot, 2);
  assert.equal(n.priority, "Idle");
});
