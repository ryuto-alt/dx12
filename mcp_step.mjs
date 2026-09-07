import net from 'node:net';
const sock = net.connect(8787, '127.0.0.1');
let buf=''; let id=0; const pending=new Map();
sock.on('data', d => { buf+=d.toString(); let i; while((i=buf.indexOf('\n'))>=0){const l=buf.slice(0,i);buf=buf.slice(i+1);if(!l.trim())continue;const j=JSON.parse(l);const r=pending.get(j.id);if(r){pending.delete(j.id);r(j);}}});
const call=(m,p={},t=8000)=>new Promise(res=>{const q=++id;pending.set(q,res);sock.write(JSON.stringify({id:q,method:m,params:p})+'\n');setTimeout(()=>{if(pending.has(q)){pending.delete(q);res({timeout:true,method:m});}},t);});
console.log('step_frames(5):', JSON.stringify(await call('step_frames',{frames:5})).slice(0,140));
console.log('duplicate:', JSON.stringify(await call('duplicate_entity',{name:'McpSmokeBox'})).slice(0,160));
console.log('screenshot:', JSON.stringify(await call('screenshot',{},10000)).slice(0,160));
sock.end();
