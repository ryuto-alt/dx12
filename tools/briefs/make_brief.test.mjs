// make_brief.mjs の単体テスト(合成の EXEC 文書だけを使う)
//   node --test tools/briefs
import test from "node:test";
import assert from "node:assert/strict";
import { makeBrief, listItems, laneOf, truncBytes, byteLen, splitAcceptance, extractOwnership } from "./make_brief.mjs";

const big = "あ".repeat(6000);
const DOC = `# テスト文書（EXEC_X）

- 作成: 2026-11-27
- 対象: 試験

---

## 2. 本編

### 2.2 所有表
| レーン | ファイル |
|---|---|
| ルート CMakeLists と cmake/ | **A** |
| src/render/** | B |
| tests/foo.cpp | A、B |

- **凍結**: \`Application.h\`、\`Components.h\`

### 2.3 項目

##### A1 最初の項目（A、S=1、依存なし）
- **ファイル**: \`src/a.cpp\`
- **設計**: 何かをする。
- **受け入れ**: ctest が緑。
  続きの行。
- **レビュー観点**: 漏れ。

##### B2 長い項目（B、L=4、依存 A1）
- **ファイル**: \`src/b.cpp\`
- **設計**: ${big}
- **受け入れ**: ゴールデン一致。
- **レビュー観点**: 末尾の観点 END_MARK。

##### C3（P1）別の項目（**C**（r2）、M=2、依存 A1）
- **設計**: x
`;

test("見出しから項目 ID とレーンを読む", () => {
  assert.equal(laneOf("A1 最初の項目（A、S=1、依存なし）"), "A");
  assert.equal(laneOf("C3（P1）別の項目（**C**（r2）、M=2、依存 A1）"), "C");
  assert.equal(laneOf("W05-08 ミップ（AUD-09）（L1、S=1、依存なし、M5）"), "L1");
  assert.equal(laneOf("ただの見出し"), null);
  assert.deepEqual(listItems(DOC).map((i) => i.id), ["A1", "B2", "C3"]);
});

test("受け入れの段落を本文から分ける(続きの行も含む)", () => {
  const { acceptance, body } = splitAcceptance(["- **設計**: a", "- **受け入れ**: b", "  続き", "- **レビュー観点**: c"]);
  assert.match(acceptance, /受け入れ.*b\n {2}続き/);
  assert.doesNotMatch(body, /受け入れ/);
  assert.match(body, /レビュー観点/);
});

test("所有表: 行がレーンの表と凍結の行を拾う", () => {
  const o = extractOwnership(DOC, "A");
  assert.match(o, /ルート CMakeLists/);
  assert.match(o, /tests\/foo\.cpp/);
  assert.doesNotMatch(o, /src\/render/);
  assert.match(o, /凍結/);
});

test("ブリーフ: 受け入れ・所有・要約・出典が入り、上限以下", () => {
  const b = makeBrief(DOC, "A1", { source: "EXEC_X.md" });
  assert.ok(b.bytes <= 8192);
  assert.equal(b.truncated, false);
  assert.match(b.text, /## 受け入れ\n- \*\*受け入れ\*\*: ctest が緑。\n {2}続きの行。/);
  assert.match(b.text, /出典: EXEC_X\.md:\d+-\d+/);
  assert.match(b.text, /ルート CMakeLists/);
  assert.match(b.text, /作成: 2026-11-27/);
  assert.match(b.text, /設計\*\*: 何かをする/);
});

test("長い項目は 8KB に収まり、受け入れは省略されず、本文の末尾も残る", () => {
  const b = makeBrief(DOC, "B2");
  assert.ok(b.bytes <= 8192, `bytes=${b.bytes}`);
  assert.equal(b.truncated, true);
  assert.match(b.text, /ゴールデン一致/);
  assert.match(b.text, /END_MARK/);
  assert.match(b.text, /中略/);
  const small = makeBrief(DOC, "B2", { maxBytes: 3000 });
  assert.ok(small.bytes <= 3000, `bytes=${small.bytes}`);
  assert.match(small.text, /ゴールデン一致/);
});

test("無い ID は null / truncBytes はバイト数を守る", () => {
  assert.equal(makeBrief(DOC, "ZZ9"), null);
  const t = truncBytes("あいうえお\nかきくけこ", 20);
  assert.ok(byteLen(t) <= 20);
});
