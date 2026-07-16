import assert from "node:assert/strict";
import { auditUiTree, designBrief } from "./uiQuality.ts";

const node = (name: string, rect: number[], extra: any = {}) => ({
  entityId: Math.floor(Math.random() * 100000), name, resolvedRect: rect,
  uiRect: { visible:true, anchorMin:[0,0], anchorMax:[0,0] }, components:[], ...extra,
});

const good = { canvases:[{ name:"Canvas", uiCanvas:{refWidth:1920,refHeight:1080}, children:[
  node("Start", [100,850,320,64], { components:["uiImage","uiButton"], uiImage:{color:[.1,.1,.1,1],raycastBlock:true,gradientDir:0,outlineWidth:0,shadowAlpha:0,shape:0}, uiButton:{interactable:true,onClickEvent:"start"} }),
  node("Title", [100,100,900,100], { components:["uiText"], text:"TITLE", uiText:{fontSize:64,wrap:false,rich:false,charAnim:0,outlineWidth:0,shadowAlpha:0} }),
] }] };
const a = auditUiTree(good, "strict");
assert.equal(a.summary.errors, 0);
assert.ok(a.score >= 90);

const bad = { canvases:[{ name:"Canvas", uiCanvas:{refWidth:1920,refHeight:1080}, children:[
  node("Blocker", [0,0,1920,1080], { components:["uiImage"], uiImage:{color:[0,0,0,1],raycastBlock:true,gradientDir:1,outlineWidth:2,shadowAlpha:.5,shape:3,gradientScrollSpeed:.8} }),
  node("A", [100,100,40,30], { components:["uiButton"], uiButton:{interactable:true,onClickEvent:""} }),
  node("B", [105,105,40,30], { components:["uiButton"], uiButton:{interactable:true,onClickEvent:"b"} }),
  node("Copy", [0,0,100,12], { components:["uiText"], text:"this is far too long", uiText:{fontSize:24,wrap:false,rich:true,charAnim:1,outlineWidth:1,shadowAlpha:.5} }),
] }] };
const b = auditUiTree(bad, "strict");
assert.equal(b.pass, false);
for (const code of ["INPUT_BLOCKER","SMALL_HIT_TARGET","INTERACTIVE_OVERLAP","TEXT_CLIPPED_HEIGHT","OVER_DECORATED"])
  assert.ok(b.issues.some((i:any) => i.code === code), `missing ${code}`);

const brief = designBrief("horror", "title");
assert.match(brief.direction.composition, /余白/);
assert.ok(brief.avoid.length >= 5);
console.log("OK: UI品質監査テスト通過");
