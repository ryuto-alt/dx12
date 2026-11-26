// 構造化エラー(M2)。AI が読んで【そのまま撃ち直せる】形に統一する。
//
// 形(dx12_call はこれを本文 JSON として返す。旧ツールは従来の本文の後ろにこの JSON を 1 ブロック足す):
//   { ok:false,
//     error:"<人が読む 1 文>",            // 旧来の error 文字列(エンジンの error をそのまま)
//     error_code:"E_BAD_ENUM",           // 文字列コード(この表が正)
//     engineCode:2,                      // 旧来の数値 error_code(エンジン/TS が付けていたもの。あれば)
//     cause:"…", retryable:false,
//     didYouMean:["sculpt"], validValues:[…],
//     fix:[{tool:"dx12_x", args:{…}, why:"…"}],   // dx12_call にそのまま渡せる。shell コマンドは {command, why}
//     details:{…}, docs:"dx12_tool_describe {name:'…'}" }
//
// 語調は標準語・簡潔(方言や命令口調は使わない。errors.test.ts の lint が見張る)。

import { editDistance } from "./paramGuard.ts";

export type ErrorCodeName =
  | "E_ENGINE_UNREACHABLE" | "E_ENGINE_BUSY" | "E_ENGINE_TIMEOUT" | "E_ENGINE_TOO_OLD"
  | "E_UNKNOWN_TOOL" | "E_UNKNOWN_PARAM" | "E_MISSING_PARAM" | "E_BAD_TYPE" | "E_BAD_ENUM" | "E_OUT_OF_RANGE"
  | "E_NOT_FOUND_ENTITY" | "E_NOT_FOUND_ASSET" | "E_NOT_FOUND_SCENE" | "E_NOT_FOUND_COMPONENT" | "E_NOT_FOUND_COMMAND"
  | "E_STALE_SCENE" | "E_MODE_CONFLICT" | "E_MODAL_OPEN" | "E_VIRTUAL_INPUT_OFF"
  | "E_UNSUPPORTED" | "E_GUARDED" | "E_VALIDATION_FAILED" | "E_FILE_IO" | "E_CANCELLED"
  | "E_SAFETY_VIOLATION" | "E_INTERNAL"
  // フリート(複数エンジンの管理。docs/MCP_FLEET_DESIGN.md)
  | "E_FLEET_LIMIT" | "E_FLEET_RESOURCE" | "E_FLEET_VISIBLE_DENIED" | "E_FLEET_READONLY" | "E_FLEET_NOT_FOUND" | "E_FLEET_NOT_OWNER"
  | "E_FLEET_PROJECT_IN_USE" | "E_FLEET_BUILD_IN_PROGRESS" | "E_FLEET_LAUNCH_FAILED" | "E_FLEET_EXE_MISSING" | "E_FLEET_DISABLED"
  // ジョブ API(M6)と冪等キー(M5)
  | "E_JOB_NOT_FOUND" | "E_JOB_NOT_FINISHED" | "E_JOB_NOT_OWNER" | "E_JOB_TOOL_MISSING" | "E_JOB_DISABLED" | "E_JOB_FAILED" | "E_JOB_TIMEOUT" | "E_JOB_INTERRUPTED" | "E_JOB_RUNNER_LOST"
  | "E_IDEMPOTENCY_CONFLICT" | "E_IDEMPOTENCY_IN_FLIGHT"
  // 旧数値コード 1 / 2 を細分化できなかったときの受け皿(エンジンが具体名を付けなかった古い経路)。
  | "E_NOT_FOUND" | "E_INVALID_PARAM";

export type Fix = {
  /** dx12_call の name にそのまま渡せる名前(旧ツール名 or エンジン method 名)。 */
  tool?: string;
  args?: Record<string, unknown>;
  /** ツールではなく shell で撃つコマンド(エンジン起動など)。 */
  command?: string;
  /** 撃ち直す前にやること(例: 先に dx12_stop)。この fix を撃った後で元の呼び出しを再送する。 */
  thenRetry?: boolean;
  why?: string;
};

export type ErrorBody = {
  code: ErrorCodeName;
  message: string;
  cause?: string;
  retryable?: boolean;
  engineCode?: number;
  didYouMean?: string[];
  validValues?: string[];
  fix?: Fix[];
  details?: Record<string, unknown>;
  docs?: string;
  issues?: unknown[];
  hint?: string;
};

/** コード表(ドキュメントと errors.test.ts の突き合わせ用)。retryable は既定値。 */
export const ERROR_CODES: Record<ErrorCodeName, { legacy: number | null; retryable: boolean; meaning: string }> = {
  E_ENGINE_UNREACHABLE: { legacy: null, retryable: true, meaning: "エンジンに繋がらない(ポートが閉じている/プロセスが無い/切断)" },
  E_ENGINE_BUSY: { legacy: null, retryable: true, meaning: "接続は通るが応答が無い(別クライアントが単一ブリッジを保持している等)" },
  E_ENGINE_TIMEOUT: { legacy: null, retryable: true, meaning: "期限内に応答が来ない。エンジンは処理を続けている可能性がある" },
  E_ENGINE_TOO_OLD: { legacy: null, retryable: false, meaning: "呼びたい method/プロトコルをエンジンが持たない(エンジンが古い)" },
  E_UNKNOWN_TOOL: { legacy: 8, retryable: false, meaning: "ツール/メソッド名が無い" },
  E_UNKNOWN_PARAM: { legacy: 2, retryable: false, meaning: "知らない引数(打ち間違い)" },
  E_MISSING_PARAM: { legacy: 2, retryable: false, meaning: "必須の引数が無い" },
  E_BAD_TYPE: { legacy: 2, retryable: false, meaning: "引数の型が違う" },
  E_BAD_ENUM: { legacy: 2, retryable: false, meaning: "列挙値に無い値" },
  E_OUT_OF_RANGE: { legacy: 2, retryable: false, meaning: "範囲外の値" },
  E_NOT_FOUND_ENTITY: { legacy: 1, retryable: false, meaning: "エンティティが無い(近い名前を添える)" },
  E_NOT_FOUND_ASSET: { legacy: 1, retryable: false, meaning: "アセットが無い(近いパスを添える)" },
  E_NOT_FOUND_SCENE: { legacy: 1, retryable: false, meaning: "シーンが無い(近いシーンを添える)" },
  E_NOT_FOUND_COMPONENT: { legacy: 6, retryable: false, meaning: "コンポーネントの jsonKey が無い" },
  E_NOT_FOUND_COMMAND: { legacy: 1, retryable: false, meaning: "エディタのコマンド id が表に無い(近い id を添える。dx12_editor_command {op:'list'} で探す)" },
  E_STALE_SCENE: { legacy: 4, retryable: true, meaning: "expectGeneration が現在の sceneGeneration と違う(古い entityId)" },
  E_MODE_CONFLICT: { legacy: 3, retryable: true, meaning: "Editor/Playing が要件と合わない、トランザクション中に禁止された method など" },
  E_MODAL_OPEN: { legacy: 13, retryable: true, meaning: "ImGui のモーダルが開いていて UI 操作できない" },
  E_VIRTUAL_INPUT_OFF: { legacy: 3, retryable: true, meaning: "仮想入力モードが OFF" },
  E_UNSUPPORTED: { legacy: 10, retryable: false, meaning: "GPU/環境が非対応。再送しても無駄" },
  E_GUARDED: { legacy: 11, retryable: false, meaning: "取り返しの付かない操作。confirm:true が要る" },
  E_VALIDATION_FAILED: { legacy: 2, retryable: false, meaning: "宣言的な入力(scene spec 等)の検証失敗" },
  E_FILE_IO: { legacy: 14, retryable: true, meaning: "ファイルの読み書きに失敗(path 未指定の罠など)" },
  E_CANCELLED: { legacy: 12, retryable: true, meaning: "呼び出しが中断された" },
  E_SAFETY_VIOLATION: { legacy: null, retryable: false, meaning: "仮想入力中に OS のカーソル/前面窓が動いた(以降の UI 操作を止める)" },
  E_INTERNAL: { legacy: 7, retryable: false, meaning: "エンジン内部エラー" },
  E_FLEET_LIMIT: { legacy: null, retryable: true, meaning: "同時起動数の上限(既定 3 台)。止める候補が fix に出る" },
  E_FLEET_RESOURCE: { legacy: null, retryable: true, meaning: "空き VRAM / RAM が下限未満で起動を断った" },
  E_FLEET_VISIBLE_DENIED: { legacy: null, retryable: false, meaning: "窓を画面に出す起動(visible)は既定で拒否(DX12_MCP_ALLOW_VISIBLE=1 と confirm:true が要る)" },
  E_FLEET_READONLY: { legacy: null, retryable: false, meaning: "読み取り専用で繋いだエンジンへ書き込み系の method を送ろうとした" },
  E_FLEET_NOT_FOUND: { legacy: null, retryable: false, meaning: "指定したエンジン(id / name / port)がフリートに無い" },
  E_FLEET_NOT_OWNER: { legacy: null, retryable: false, meaning: "他のセッションのエンジンは止められない(force と confirm が要る。孤児は可)" },
  E_FLEET_PROJECT_IN_USE: { legacy: null, retryable: false, meaning: "同じプロジェクトを別のフリートのエンジンが使っている(自動保存が衝突する)" },
  E_FLEET_BUILD_IN_PROGRESS: { legacy: null, retryable: true, meaning: "exe の元がビルド中で、コピーできない" },
  E_FLEET_LAUNCH_FAILED: { legacy: null, retryable: true, meaning: "エンジンが起動しない/期限内に ping に応答しない" },
  E_FLEET_EXE_MISSING: { legacy: null, retryable: false, meaning: "コピー元の DX12Engine.exe が見つからない" },
  E_FLEET_DISABLED: { legacy: null, retryable: false, meaning: "フリートが無効(DX12_FLEET_DISABLE=1)" },
  E_JOB_NOT_FOUND: { legacy: null, retryable: false, meaning: "ジョブ id が無い(または保存期間を過ぎて消えた)" },
  E_JOB_NOT_FINISHED: { legacy: null, retryable: true, meaning: "まだ終わっていないジョブの結果を求めた" },
  E_JOB_NOT_OWNER: { legacy: null, retryable: false, meaning: "他のセッションが起動した(生きている)ジョブは止められない(force と承認が要る)" },
  E_JOB_TOOL_MISSING: { legacy: null, retryable: false, meaning: "ジョブが使う道具(build.ps1・vgeo_cook・ctest・exe など)が見つからない/起動できない" },
  E_JOB_DISABLED: { legacy: null, retryable: false, meaning: "ジョブ API が無効(DX12_JOBS_DISABLE=1)" },
  E_JOB_FAILED: { legacy: null, retryable: false, meaning: "ジョブの処理が失敗した(終了コード・失敗したテスト・ビルドエラーは summary と details)" },
  E_JOB_TIMEOUT: { legacy: null, retryable: true, meaning: "ジョブが timeoutSec を超えたのでプロセスツリーを終了した" },
  E_JOB_INTERRUPTED: { legacy: null, retryable: true, meaning: "ジョブを起動した MCP サーバが終了したため、実行中(または開始前)に中断された" },
  E_JOB_RUNNER_LOST: { legacy: null, retryable: true, meaning: "ジョブの runner が終了結果を残さずに消えた(強制終了・異常終了)" },
  E_IDEMPOTENCY_CONFLICT: { legacy: 2, retryable: false, meaning: "同じ冪等キーで別の要求(method / 引数 / kind が違う)を送った" },
  E_IDEMPOTENCY_IN_FLIGHT: { legacy: 9, retryable: true, meaning: "同じ冪等キーの処理がまだ進行中(少し待って再送する。完了していれば前回の結果が返る)" },
  E_NOT_FOUND: { legacy: 1, retryable: false, meaning: "対象が無い(種類を特定できなかった旧経路)" },
  E_INVALID_PARAM: { legacy: 2, retryable: false, meaning: "引数不正(種類を特定できなかった旧経路)" },
};

/** 数値の旧コード → 文字列コードの既定(メッセージで細分化できないとき)。 */
const LEGACY_TO_NAME: Record<number, ErrorCodeName> = {
  1: "E_NOT_FOUND", 2: "E_INVALID_PARAM", 3: "E_MODE_CONFLICT", 4: "E_STALE_SCENE", 6: "E_NOT_FOUND_COMPONENT",
  7: "E_INTERNAL", 8: "E_UNKNOWN_TOOL", 9: "E_ENGINE_BUSY", 10: "E_UNSUPPORTED", 11: "E_GUARDED",
  12: "E_CANCELLED", 13: "E_MODAL_OPEN", 14: "E_FILE_IO",
};

export function isErrorCodeName(s: unknown): s is ErrorCodeName {
  return typeof s === "string" && Object.prototype.hasOwnProperty.call(ERROR_CODES, s);
}

/** エンジン/TS が付けた数値コード + メッセージから文字列コードを決める。 */
export function classifyCode(engineCode: number | undefined, message: string, explicitName?: string): ErrorCodeName {
  if (isErrorCodeName(explicitName)) return explicitName;
  const m = message ?? "";
  if (engineCode === 1) {
    if (/entity|エンティティ/i.test(m)) return "E_NOT_FOUND_ENTITY";
    if (/scene|シーン/i.test(m)) return "E_NOT_FOUND_SCENE";
    if (/asset|texture|model|file|path|アセット|テクスチャ|モデル|ファイル/i.test(m)) return "E_NOT_FOUND_ASSET";
    return "E_NOT_FOUND";
  }
  if (engineCode === 2) {
    if (/unknown method|unknown tool|not found.*tool/i.test(m)) return "E_UNKNOWN_TOOL";
    if (/知らない引数|unknown (param|key|argument)/i.test(m)) return "E_UNKNOWN_PARAM";
    if (/missing|required|が無い|必須/i.test(m)) return "E_MISSING_PARAM";
    if (/must be|expected|型|invalid type/i.test(m)) return "E_BAD_TYPE";
    if (/unknown\s+\w+\s*:|not one of|列挙|有効な値|valid values/i.test(m)) return "E_BAD_ENUM";
    if (/range|out of|以上|以下|範囲/i.test(m)) return "E_OUT_OF_RANGE";
    return "E_INVALID_PARAM";
  }
  if (engineCode === 3 && /virtual input|仮想入力/i.test(m)) return "E_VIRTUAL_INPUT_OFF";
  if (engineCode != null && LEGACY_TO_NAME[engineCode]) return LEGACY_TO_NAME[engineCode];
  if (/unknown method|unknown tool/i.test(m)) return "E_UNKNOWN_TOOL";
  // 数値コードが無い = TS 側のハンドラが投げた素の Error。大半は引数の検証エラーなので、文面から細分化する。
  if (engineCode == null) {
    if (/未知の|使えるのは|有効な値|not one of|invalid (value|option)/i.test(m)) return "E_BAD_ENUM";
    if (/(見つからない|存在しない|無い)\s*$|not found|no such/i.test(m)) return "E_NOT_FOUND";
    if (/必須|missing|required|を指定して/i.test(m)) return "E_MISSING_PARAM";
    if (/EACCES|EPERM|ENOENT|EISDIR|書き込め|読み込め|書けない|読めない/i.test(m)) return "E_FILE_IO";
    if (/範囲外|out of range|以上|以下/i.test(m)) return "E_OUT_OF_RANGE";
    return "E_INVALID_PARAM";
  }
  return "E_INTERNAL";
}

/** 「使えるのは: a, b, c」「valid values: a, b」の形でメッセージに埋まっている有効値を取り出す。 */
export function extractValidValues(message: string): string[] {
  const m = /(?:使えるのは|有効な値|valid values?|one of|options?)\s*[:：]\s*([^\n]+)$/i.exec(message.trim());
  if (!m) return [];
  return m[1].split(/[,、]\s*|\s+\/\s+/).map((s) => s.trim().replace(/[。.]$/, "")).filter((s) => s && s.length <= 64);
}

export function envelope(body: ErrorBody): Record<string, unknown> {
  const meta = ERROR_CODES[body.code];
  const out: Record<string, unknown> = {
    ok: false,
    error: body.message,
    error_code: body.code,
  };
  if (body.engineCode != null) out.engineCode = body.engineCode;
  if (body.cause) out.cause = body.cause;
  out.retryable = body.retryable ?? meta.retryable;
  if (body.didYouMean?.length) out.didYouMean = body.didYouMean;
  if (body.validValues?.length) out.validValues = body.validValues;
  if (body.fix?.length) out.fix = body.fix;
  if (body.hint) out.hint = body.hint;
  if (body.issues?.length) out.issues = body.issues;
  if (body.details && Object.keys(body.details).length) out.details = body.details;
  if (body.docs) out.docs = body.docs;
  return out;
}

// ── 候補提示(編集距離 + 前方/部分一致) ───────────────────────────────

/**
 * target に近い候補を近い順に最大 limit 件返す。大文字小文字違い/区切り違い(-,_,空白)を最優先、
 * 次に前方/部分一致、次に編集距離(長さに応じた閾値)。
 */
export function nearest(target: string, candidates: readonly string[], limit = 5, opts: { liberal?: boolean } = {}): string[] {
  const norm = (s: string) => s.toLowerCase().replace(/[\s_\-./]+/g, "");
  const t = norm(target);
  if (!t) return [];
  const scored: { c: string; s: number }[] = [];
  for (const c of new Set(candidates)) {
    const n = norm(c);
    if (!n) continue;
    let s: number;
    if (n === t) s = 0;
    else if (n.startsWith(t) || t.startsWith(n)) s = 0.5 + Math.abs(n.length - t.length) / 100;
    else if (n.includes(t) || t.includes(n)) s = 1 + Math.abs(n.length - t.length) / 100;
    else {
      const d = editDistance(t, n);
      // liberal: 候補が少ない閉じた集合(enum)では、音の近い打ち間違い(nite → night)まで拾う。
      const maxLen = Math.max(t.length, n.length);
      const limitD = opts.liberal ? Math.max(3, Math.floor(maxLen / 2)) : Math.max(2, Math.floor(maxLen / 3));
      if (d > limitD) continue;
      s = 2 + d + Math.abs(n.length - t.length) / 100;
    }
    scored.push({ c, s });
  }
  scored.sort((a, b) => a.s - b.s || a.c.localeCompare(b.c));
  return scored.slice(0, limit).map((x) => x.c);
}

// ── E_UNKNOWN_TOOL の fix(BUG-14) ───────────────────────────────────────────────
// ★「呼べない名前を勧めない」: tools/list に出ていないツール(full 面の dx12_sequence、core 面の長尾、エンジンの method 名)を
//   `{tool:"<名前>"}` のまま勧めると、エージェントはそれを MCP のツールとして直接呼び、同じ「ツールが無い」に戻って無限に繰り返す。
//   直接呼べないものは `dx12_call {name:"<名前>", args}` の形に包んで示す。
// ★同じ修復案を 2 回返したら、3 回目以降の撃ち直しを誘わない文面(打ち切り)に切り替える。

/** 直接呼べない名前を dx12_call の形に包むための、shell の実行口。 */
export const CALL_TOOL = "dx12_call";

/**
 * fix の tool が MCP の tools/list に出ていない(直接呼べない)なら、`dx12_call {name, args}` の形へ包む。
 * dx12_call 自身・直接呼べる名前・tool を持たない fix(command など)はそのまま返す。
 * 引数の dx12_call 用 `dryRun` / `confirm` などは触らない(元の fix の args を name の args に入れるだけ)。
 */
export function routeFixViaCall(fixes: Fix[] | undefined, isDirect: ((name: string) => boolean) | undefined): Fix[] | undefined {
  if (!fixes || !isDirect) return fixes;
  return fixes.map((f) => {
    if (!f.tool || f.tool === CALL_TOOL || isDirect(f.tool)) return f;
    return {
      ...f,
      tool: CALL_TOOL,
      args: { name: f.tool, args: f.args ?? {} },
      why: `${f.why ?? "撃ち直す"}(${f.tool} は MCP のツールとしては直接呼べない。dx12_call の name に渡す)`,
    };
  });
}

type RepeatRecord = { n: number; at: number };
const REPEAT_WINDOW_MS = 10 * 60 * 1000;
const REPEAT_MAX_KEYS = 128;
const repeatedFixes = new Map<string, RepeatRecord>();

/** テスト用: 同じ fix の繰り返しの記録を空にする。 */
export function resetRepeatedFixGuard(): void { repeatedFixes.clear(); }

/**
 * E_UNKNOWN_TOOL の本文に「同じ fix を返した回数」を数え、2 回目以降は撃ち直しを誘わない文面へ置き換える(打ち切り)。
 * 鍵は「呼ばれた名前 + 先頭の fix」。同じ呼び出しがもう一度来た = 前回の fix では直っていない。
 */
export function guardRepeatedFix(body: ErrorBody, requestedName: string, now: number = Date.now()): ErrorBody {
  const head = body.fix?.find((f) => f.tool);
  // 引数が違っても「同じ名前を同じ形で勧めた」なら同じ鍵(名前が通らない理由は引数と無関係)
  const headName = head ? (head.tool === CALL_TOOL ? `${CALL_TOOL}:${(head.args as any)?.name}` : head.tool) : "-";
  const key = `${requestedName.replace(/^dx12_/, "")}|${headName}`;
  for (const [k, r] of repeatedFixes) if (now - r.at > REPEAT_WINDOW_MS) repeatedFixes.delete(k);
  const prev = repeatedFixes.get(key);
  const n = (prev?.n ?? 0) + 1;
  repeatedFixes.delete(key);
  repeatedFixes.set(key, { n, at: now });
  while (repeatedFixes.size > REPEAT_MAX_KEYS) { const oldest = repeatedFixes.keys().next().value; if (oldest === undefined) break; repeatedFixes.delete(oldest); }
  if (n < 2) return body;
  const shown = head?.tool === CALL_TOOL ? `dx12_call {name:'${(head.args as any)?.name}'}` : head?.tool ?? "(なし)";
  body.message = `${body.message}(同じ呼び出しがもう一度来た)`;
  body.cause = `同じ修復案(${shown})を ${n} 回返している。前回の案では直っていないので、同じ呼び出しを撃ち直しても結果は変わらない`;
  body.retryable = false;
  body.fix = [
    { tool: "dx12_tool_search", args: { query: requestedName.replace(/^dx12_/, "").replace(/_/g, " ") }, why: "名前を検索し直す(検索結果に無ければ、そのツールはこの環境に無い)" },
    { why: "ここで撃ち直しをやめる。検索しても見つからないときは、呼びたい操作と得られたエラーをユーザーに報告する" },
  ];
  body.details = { ...(body.details ?? {}), repeatedFix: n, giveUp: true };
  return body;
}

/** 方言・命令口調の混入検査(テスト用。新規メッセージは標準語で書く)。 */
export const DIALECT_PATTERN = /かもしれん|かかるで|してくれ(?!る)|やで|ちゃう|やんか|せなあかん|ほんま|ええ/;
