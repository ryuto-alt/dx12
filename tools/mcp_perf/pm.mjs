// 指定した method の引数表を引く(ポート 8823 固定のメモ用)。 node pm.mjs <method>...
import net from "node:net";
const names=process.argv.slice(2);
const s=net.connect(Number(process.env.PORT || 8823),"127.0.0.1");let b="";
s.on("connect",()=>s.write(JSON.stringify({id:1,method:"describe_mcp_manifest",params:{}})+"\n"));
s.on("data",d=>{b+=d;if(b.endsWith("\n")){const m=JSON.parse(b).result.methods;
for(const x of m){ if(!names.includes(x.name)) continue; console.log(x.name,"::",x.summary.slice(0,100)); console.log("   ",x.params.map(p=>p.name+":"+p.type+(p.enum?"("+p.enum.join("|")+")":"")).join(", ")) } process.exit(0)}});
