// Testes da lógica da interface com DOM mínimo; não substituem inspeção visual.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
class Element {
 constructor(){this.children=[];this.attrs={};this.handlers={};this.value='1000';this.hidden=false;this.checked=false;this.classList={add(){},remove(){}};}
 setAttribute(k,v){this.attrs[k]=v;} appendChild(el){this.children.push(el);return el;}
 replaceChildren(){this.children=[];} addEventListener(k,f){this.handlers[k]=f;}
}
const point=(t,lon)=>({t,available_t:t,lat:-15.8,lon,alt:5000,break:t===0});
const frame=t=>({t,phase:'LEVEL',alt:5000,decisions:{},quality:'DADOS_UTILIZAVEIS',position_available:true});
const data={session:'test',airports:{},aircraft:{ABC123:{points:[point(0,-47.9),point(10000,-47.8)],frames:[frame(0),frame(10000)]},DEF456:{points:[point(0,-48),point(10000,-47.95)],frames:[frame(0),frame(10000)]},AAAAAA:{points:[],frames:[frame(0)]}}};
const elements=new Map();const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};get('data').textContent=JSON.stringify(data);get('details').hidden=true;get('replayPanel').hidden=true;
const callbacks=[];const context=vm.createContext({document:{getElementById:get,createElement:()=>new Element(),createElementNS:()=>new Element()},location:{protocol:'file:'},performance:{now:()=>0},requestAnimationFrame:f=>callbacks.push(f),console});
const html=fs.readFileSync('ground/trajectory_view.html','utf8'),code=html.split('<script>')[1].split('</script>')[0];vm.runInContext(code,context);
const markers=()=>get('map').children.filter(el=>el.attrs.class==='plane');
assert.equal(markers().length,2);assert.equal(get('details').hidden,true);
markers()[0].handlers.click({stopPropagation(){}});
assert.equal(get('details').hidden,false);assert.equal(markers().length,2);assert.equal(get('aircraft').value,'ABC123');
const animate=callbacks.at(-1);animate(400);
const selected=markers().find(m=>m.attrs['data-icao']==='ABC123');assert.equal(selected.attrs.transform,'translate(500,240)');
get('timeline').value='0';get('timeline').oninput();assert.equal(markers().length,2);
get('closeDetails').onclick();assert.equal(get('details').hidden,true);assert.equal(markers().length,2);
get('aircraft').value='AAAAAA';get('aircraft').onchange();assert.equal(get('phase').textContent,'Fora deste instante');assert.equal(markers().length,2);
get('fit').onclick();assert.equal(get('details').hidden,true);
// One clock: selecting another aircraft must not reset replay time.
get('timeline').value='500';get('timeline').oninput();
get('aircraft').value='DEF456';get('aircraft').onchange();assert.equal(get('timeline').value,'500');
assert.equal(vm.runInContext('visiblePoints(DATA.aircraft.ABC123,5000).length',context),1);
assert.equal(vm.runInContext('visiblePoints(DATA.aircraft.ABC123,-1).length',context),0);
assert.equal(vm.runInContext('visiblePoints(DATA.aircraft.ABC123,70000).length',context),2);
assert.equal(vm.runInContext('visiblePoints(DATA.aircraft.ABC123,70001).length',context),0);
// A late observation is never visible before it was available to the estimator.
assert.equal(vm.runInContext('visiblePoints({points:[{t:0,available_t:10000}]},5000).length',context),0);
// Different capture times must never coexist simply because both were recorded.
vm.runInContext('DATA.aircraft.DEF456.points=DATA.aircraft.DEF456.points.map(p=>({...p,t:p.t+100000,available_t:p.available_t+100000}));render()',context);
assert.equal(markers().length,1);assert.equal(markers()[0].attrs['data-icao'],'ABC123');
vm.runInContext('closeDetails();end=110000',context);get('timeline').value='1000';get('timeline').oninput();
assert.equal(markers().length,1);assert.equal(markers()[0].attrs['data-icao'],'DEF456');
console.log('OK: relógio compartilhado, seleção sem salto temporal, expiração, mensagens tardias e capturas em horários distintos.');
// Manual seeking stops both animation and following incoming events.
vm.runInContext('playing=true',context);get('follow').checked=true;get('timeline').value='500';get('timeline').oninput();
assert.equal(vm.runInContext('playing',context),false);assert.equal(get('follow').checked,false);
const before=vm.runInContext('viewTime()',context);get('backTime').onclick();assert.equal(vm.runInContext('viewTime()',context),before-10000);
get('forwardTime').onclick();assert.equal(vm.runInContext('viewTime()',context),before);
get('fit').onclick();assert.equal(vm.runInContext('viewTime()',context),before);assert.equal(get('details').hidden,true);
// Split multi-day recordings so the slider has usable resolution.
vm.runInContext('refreshData({session:"new",airports:{},aircraft:{ABC123:{points:[],frames:[{t:0},{t:10000},{t:86400000},{t:86410000}]}}})',context);
assert.equal(vm.runInContext('periods.length',context),2);assert.equal(vm.runInContext('end-start',context),10000);
get('period').value='0';get('period').onchange();assert.equal(vm.runInContext('viewTime()',context),0);
console.log('OK: arraste pausa reprodução, passos ±10 s, visão geral preserva horário e períodos separados.');
vm.runInContext('historyMode=false',context);
assert.ok(Math.abs(vm.runInContext('viewTime()',context)-Date.now())<1000);
assert.equal(vm.runInContext('visiblePoints({frames:[{t:0,on_ground:1}],points:[{t:10000,available_t:10000}]},10000).length',context),0);
assert.equal(vm.runInContext('visiblePoints({frames:[{t:0,on_ground:0}],points:[{t:10000,available_t:10000}]},10000).length',context),1);
assert.equal(vm.runInContext('visiblePoints({frames:[],points:[{t:0,available_t:0}]},Date.now()).length',context),0);
console.log('OK: ao vivo usa hora atual, oculta solo e não reapresenta posições antigas.');
vm.runInContext(`
 historyMode=true;start=0;end=200000;selected=null;current=null;visibleAircraft=new Set();
 DATA={session:'autofit',airports:{},aircraft:{ABC123:{points:[{t:0,available_t:0,lat:-15,lon:-47,break:true}],frames:[{t:0,on_ground:0}]},DEF456:{points:[{t:20000,available_t:20000,lat:-16,lon:-49,break:true}],frames:[{t:20000,on_ground:0}]}}};
 let fits=0;const actualFit=fit;fit=(at)=>{fits++;actualFit(at);};
`,context);
get('timeline').value='0';get('timeline').oninput();assert.equal(vm.runInContext('fits',context),1);
vm.runInContext('zoom=2;panX=100;render()',context);assert.equal(vm.runInContext('fits',context),1);assert.equal(vm.runInContext('zoom',context),2);
get('timeline').value='100';get('timeline').oninput();assert.equal(vm.runInContext('fits',context),2);assert.equal(markers().length,2);
for(const marker of markers()){const nums=marker.attrs.transform.match(/-?\d+(?:\.\d+)?/g).map(Number);assert.ok(nums[0]>=0&&nums[0]<=1000);assert.ok(nums[1]>=0&&nums[1]<=480);}
get('timeline').value='1000';get('timeline').oninput();assert.equal(markers().length,0);
get('timeline').value='0';get('timeline').oninput();assert.equal(vm.runInContext('fits',context),3);
console.log('OK: reenquadra na primeira aeronave, em novas aparições e reaparições; mantém zoom manual entre elas.');
