// エンジン(C++)の MCP ハンドラが受け付けるフィールドと、この MCP サーバの zod スキーマが
// 宣言しているフィールドを突き合わせるための純パーサ(fs も engine も触らない)。
//
// ★なぜ要るか
//   エンジンに新しい設定フィールドが足されても TS スキーマに足し忘れると、その引数は
//   zod に黙って捨てられ、set_* は {applied:true} を返す。AI からは「効かない操作」が
//   延々と続く。実際 tonemapper / godraysOn / lensflareOn / dofOn / motionBlurOn /
//   autoExposureOn などが長期間このまま渡せなくなっていた。
//   完全自動同期までは要らないが「次に足し忘れたらテストが赤くなる」だけは必要。
//
// ★真実の情報源
//   src/core/Application.cpp の HandleMcpCommand() 内 `method == "..."` の分岐。
//   params から読むキーを静的に拾う(params.value / contains / [] / find / Mcp*Param 系)。
//   キー名リテラルを取らないヘルパ(ResolveMcpEntity=entity/name, ParseMcpVk=key)は別途補う。

/** engine のヘルパ関数のうち、引数にキー名リテラルを持たないもの → 実際に読むキー。 */
const IMPLICIT_HELPER_KEYS: ReadonlyArray<{ re: RegExp; keys: readonly string[] }> = [
  { re: /ResolveMcpEntity\s*\(|ResolveMcpComponentEntity\s*</, keys: ["entity", "name"] },
  { re: /ParseMcpVk\s*\(/, keys: ["key"] },
];

/** params からキーを読む書き方(Application.cpp で実際に使われている形)。 */
const PARAM_READ_PATTERNS: readonly RegExp[] = [
  /params\s*\.\s*value\(\s*"([A-Za-z0-9_]+)"/g,
  /params\s*\.\s*contains\(\s*"([A-Za-z0-9_]+)"\s*\)/g,
  /params\s*\[\s*"([A-Za-z0-9_]+)"\s*\]/g,
  /params\s*\.\s*at\(\s*"([A-Za-z0-9_]+)"\s*\)/g,
  /params\s*\.\s*find\(\s*"([A-Za-z0-9_]+)"\s*\)/g,
  // McpFloatParam / McpIntParam / McpEnumParam / McpVec3Required / McpTryVec3 ...
  /\bMcp[A-Za-z0-9_]*\s*(?:<[^>()]*>\s*)?\(\s*params\s*,\s*"([A-Za-z0-9_]+)"/g,
];

export type EngineMethod = {
  keys: string[];
  line: number;
  /** ハンドラが {"applied", true} を返す＝「成功したように見えるだけ」の返り値。 */
  returnsAppliedTrue: boolean;
};

/**
 * Application.cpp から「method 名 → 受け付ける params キー」を抜く。
 * 同じ分岐が複数 method を受ける場合(overlap_box || overlap_sphere)は両方に同じ集合を割り当てる。
 */
export function parseEngineMethods(cppSource: string): Map<string, EngineMethod> {
  const lines = cppSource.split(/\r?\n/);
  const heads: { line: number; names: string[] }[] = [];
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(
      /(?:^|\s)(?:else\s+)?if\s*\(method\s*==\s*"([a-z0-9_]+)"(?:\s*\|\|\s*method\s*==\s*"([a-z0-9_]+)")?\s*\)/,
    );
    if (m) heads.push({ line: i, names: [m[1], m[2]].filter(Boolean) as string[] });
  }
  const out = new Map<string, EngineMethod>();
  for (let k = 0; k < heads.length; k++) {
    const from = heads[k].line;
    const to = k + 1 < heads.length ? heads[k + 1].line : lines.length;
    const body = lines.slice(from, to).join("\n");
    const keys = new Set<string>();
    for (const p of PARAM_READ_PATTERNS) {
      p.lastIndex = 0;
      let m: RegExpExecArray | null;
      while ((m = p.exec(body))) keys.add(m[1]);
    }
    for (const h of IMPLICIT_HELPER_KEYS) if (h.re.test(body)) for (const key of h.keys) keys.add(key);
    const returnsAppliedTrue = /\{\s*"applied"\s*,\s*true\s*\}/.test(body);
    for (const n of heads[k].names) out.set(n, { keys: [...keys].sort(), line: from + 1, returnsAppliedTrue });
  }
  return out;
}

/**
 * PostProcessSettings.h の X マクロ(DX12E_POST_FIELDS / DX12E_SSAO_FIELDS)からフィールド名を抜く。
 * これは「エンジン側が正だと決めた唯一の名前表」(Lua の post.set もここから生成される)。
 */
export function parseFieldMacro(header: string, macroName: string): string[] {
  const start = header.indexOf(`#define ${macroName}(`);
  if (start < 0) return [];
  // 行末 '\' で継続するマクロ本体を最後まで取る
  const rest = header.slice(start);
  const endRel = rest.search(/\\\r?\n(?![\s\S]*?\\\r?\n)/);   // 最後の継続行を探す用の保険
  let body = "";
  for (const line of rest.split(/\r?\n/)) {
    body += line;
    if (!/\\\s*$/.test(line)) break;
  }
  void endRel;
  const names: string[] = [];
  // 定義行そのもの(#define NAME(B, F, I, V, S)) を落としてから X(field) を拾う
  const afterDecl = body.slice(body.indexOf(")") + 1);
  for (const m of afterDecl.matchAll(/\b[A-Z]\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)/g)) names.push(m[1]);
  return [...new Set(names)];
}

/**
 * Application.cpp の `static const std::vector<std::string> kName{"a", "b", ...};` から
 * 文字列リストを抜く(McpEnumParam に渡している列挙表)。見つからなければ null。
 */
export function parseStringVector(cppSource: string, varName: string): string[] | null {
  const re = new RegExp(`std::vector<std::string>\\s+${varName}\\s*\\{([^}]*)\\}`, "s");
  const m = cppSource.match(re);
  if (!m) return null;
  return [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]);
}

export type TsTool = { tool: string; schemaKeys: string[]; methods: string[]; line: number };

/**
 * index.ts のテキストから reg(...) / regRaw(...) のツール名・引数キー・呼ぶ engine method を抜く。
 * index.ts は import した時点で stdio サーバが起動してしまうので、実行せずテキストで読む。
 */
export function parseTsTools(indexSource: string): TsTool[] {
  const tools: TsTool[] = [];
  const re = /\b(reg|regRaw)\s*\(/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(indexSource))) {
    // 関数定義側 (function reg(...) / function regRaw(...)) は拾わない
    const before = indexSource.slice(Math.max(0, m.index - 12), m.index);
    if (/function\s*$/.test(before)) continue;
    const open = m.index + m[0].length - 1;
    const args = splitCallArgs(indexSource, open);
    if (args.length < 3) continue;
    const nameM = args[0].match(/^\s*"([a-z0-9_]+)"\s*$/);
    if (!nameM) continue;
    // reg: (name, title, description, inputSchema, ann, handler) / regRaw: (name, config, handler)
    const isReg = m[1] === "reg";
    const schemaArg = isReg ? args[3] : args[1];
    const schemaKeys = isReg ? objectKeys(schemaArg) : objectKeys(extractProperty(schemaArg, "inputSchema"));
    const handler = args.slice(isReg ? 5 : 2).join(",");
    const methods = [...handler.matchAll(/engine\.call\(\s*"([a-z0-9_]+)"/g)].map((x) => x[1]);
    const applied = [...handler.matchAll(/applyAndVerify\(\s*"([a-z0-9_]+)"\s*,\s*"([a-z0-9_]+)"/g)];
    for (const a of applied) { methods.push(a[1]); methods.push(a[2]); }
    tools.push({
      tool: nameM[1],
      schemaKeys: schemaKeys ?? [],
      methods: [...new Set(methods)],
      line: indexSource.slice(0, m.index).split("\n").length,
    });
  }
  return tools;
}

// ── 以下、素朴なソース走査(文字列/コメント/入れ子括弧を飛ばすだけ) ──────────

/** '(' の位置から呼び出し引数をトップレベルのカンマで割る。 */
function splitCallArgs(text: string, openIdx: number): string[] {
  const args: string[] = [];
  let depth = 0, cur = "", inStr: string | null = null, esc = false, inLine = false, inBlock = false;
  for (let i = openIdx; i < text.length; i++) {
    const c = text[i], n = text[i + 1];
    if (inLine) { if (c === "\n") inLine = false; cur += c; continue; }
    if (inBlock) { if (c === "*" && n === "/") { inBlock = false; cur += "*/"; i++; continue; } cur += c; continue; }
    if (inStr) {
      cur += c;
      if (esc) { esc = false; continue; }
      if (c === "\\") { esc = true; continue; }
      if (c === inStr) inStr = null;
      continue;
    }
    if (c === "/" && n === "/") { inLine = true; cur += "//"; i++; continue; }
    if (c === "/" && n === "*") { inBlock = true; cur += "/*"; i++; continue; }
    if (c === '"' || c === "'" || c === "`") { inStr = c; cur += c; continue; }
    if ("([{".includes(c)) { depth++; if (depth === 1) continue; cur += c; continue; }
    if (")]}".includes(c)) { depth--; if (depth === 0) { args.push(cur); return args; } cur += c; continue; }
    if (c === "," && depth === 1) { args.push(cur); cur = ""; continue; }
    cur += c;
  }
  args.push(cur);
  return args;
}

/** オブジェクトリテラルのトップレベルのキー名(`...spread` はそのまま返す)。 */
export function objectKeys(body: string | null): string[] | null {
  if (body == null) return null;
  const t = body.trim();
  if (!t.startsWith("{")) return null;
  const inner = t.slice(1, t.lastIndexOf("}"));
  const parts: string[] = [];
  let depth = 0, cur = "", inStr: string | null = null, esc = false, inLine = false, inBlock = false;
  for (let i = 0; i < inner.length; i++) {
    const c = inner[i], n = inner[i + 1];
    if (inLine) { if (c === "\n") inLine = false; continue; }
    if (inBlock) { if (c === "*" && n === "/") { inBlock = false; i++; } continue; }
    if (inStr) {
      if (esc) { esc = false; cur += c; continue; }
      if (c === "\\") { esc = true; cur += c; continue; }
      if (c === inStr) inStr = null;
      cur += c; continue;
    }
    if (c === "/" && n === "/") { inLine = true; i++; continue; }
    if (c === "/" && n === "*") { inBlock = true; i++; continue; }
    if (c === '"' || c === "'" || c === "`") { inStr = c; cur += c; continue; }
    if ("([{".includes(c)) { depth++; cur += c; continue; }
    if (")]}".includes(c)) { depth--; cur += c; continue; }
    if (c === "," && depth === 0) { parts.push(cur); cur = ""; continue; }
    cur += c;
  }
  parts.push(cur);
  const keys: string[] = [];
  for (const p of parts) {
    const s = p.trim();
    if (!s) continue;
    if (s.startsWith("...")) { keys.push(s.replace(/^\.\.\./, "...").split(/[\s,}]/)[0]); continue; }
    const km = s.match(/^["']?([A-Za-z0-9_$]+)["']?\s*:/);
    if (km) keys.push(km[1]);
  }
  return keys;
}

/** オブジェクトリテラルから 1 プロパティの値部分だけ取り出す(regRaw の inputSchema 用)。 */
export function extractProperty(objectLiteral: string, prop: string): string | null {
  const t = objectLiteral.trim();
  if (!t.startsWith("{")) return null;
  const inner = t.slice(1, t.lastIndexOf("}"));
  const re = new RegExp(`(^|[,{\\s])${prop}\\s*:`);
  const m = inner.match(re);
  if (!m || m.index == null) return null;
  const start = inner.indexOf(":", m.index) + 1;
  let depth = 0, inStr: string | null = null, esc = false;
  for (let i = start; i < inner.length; i++) {
    const c = inner[i];
    if (inStr) {
      if (esc) { esc = false; continue; }
      if (c === "\\") { esc = true; continue; }
      if (c === inStr) inStr = null;
      continue;
    }
    if (c === '"' || c === "'" || c === "`") { inStr = c; continue; }
    if ("([{".includes(c)) depth++;
    else if (")]}".includes(c)) depth--;
    else if (c === "," && depth === 0) return inner.slice(start, i);
    if (depth < 0) return inner.slice(start, i);
  }
  return inner.slice(start);
}
