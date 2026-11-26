// JUnit XML の組み立て(ci_suite の出力)。純関数。
import { esc } from "./common.ts";

export type JunitCase = { name: string; classname?: string; timeSec?: number; status: "pass" | "fail" | "skipped" | "error"; message?: string; detail?: string };

export function toJunit(suiteName: string, cases: JunitCase[], at = new Date()): string {
  const failures = cases.filter((c) => c.status === "fail").length;
  const errors = cases.filter((c) => c.status === "error").length;
  const skipped = cases.filter((c) => c.status === "skipped").length;
  const time = cases.reduce((a, c) => a + (c.timeSec ?? 0), 0);
  const lines = [`<?xml version="1.0" encoding="UTF-8"?>`, `<testsuites name="${esc(suiteName)}" tests="${cases.length}" failures="${failures}" errors="${errors}" skipped="${skipped}" time="${time.toFixed(3)}">`,
    `  <testsuite name="${esc(suiteName)}" tests="${cases.length}" failures="${failures}" errors="${errors}" skipped="${skipped}" time="${time.toFixed(3)}" timestamp="${at.toISOString()}">`];
  for (const c of cases) {
    const head = `    <testcase classname="${esc(c.classname ?? suiteName)}" name="${esc(c.name)}" time="${(c.timeSec ?? 0).toFixed(3)}"`;
    if (c.status === "pass") lines.push(`${head}/>`);
    else if (c.status === "skipped") lines.push(`${head}>`, `      <skipped message="${esc(c.message ?? "")}"/>`, `    </testcase>`);
    else {
      const tag = c.status === "fail" ? "failure" : "error";
      lines.push(`${head}>`, `      <${tag} message="${esc(c.message ?? "")}">${esc(c.detail ?? "")}</${tag}>`, `    </testcase>`);
    }
  }
  lines.push(`  </testsuite>`, `</testsuites>`);
  return lines.join("\n") + "\n";
}
