// Browser-script smoke test: boot without data, then attach the live ADS-B view.
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const path = require('path');
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    style: {}, lastChild: {}, checked: false, width: 500, height: 100,
    getAttribute(k) { return this[k]; }, setAttribute(k,v) { this[k]=v; },
    addEventListener() {}, querySelectorAll() { return []; },
    getContext() { return new Proxy({}, {get: () => () => {}}); },
    getBoundingClientRect() { return {width:500,height:100}; }
  });
  return elements.get(id);
}
let stream;
const context = {
  document: {getElementById:element,querySelectorAll:()=>[],documentElement:{},body:{}},
  localStorage: {getItem:()=>null},
  window: {devicePixelRatio:1}, devicePixelRatio:1,
  getComputedStyle:()=>({getPropertyValue:()=>''}),
  setInterval() {}, setTimeout() {}, clearTimeout() {},
  EventSource: function () { stream=this; },
  console
};
const html = fs.readFileSync(path.join(__dirname,'gs_web_ui.html'),'utf8');
vm.runInNewContext(html.split('<script>')[1].split('</script>')[0],context);
assert(stream, 'initial render must reach the event subscription');
stream.onmessage({data:JSON.stringify({source:'exec',log:[],ground:{},rssi:[],snr:[],acks:{},adsb:{processed:4,rejected:1,repeats:2,error:null}})});
assert.equal(element('adsb-frame').src,'/adsb/');
assert.equal(element('adsb-panel').style.display,'block');
assert(element('adsb-status').textContent.includes('Processados: 4'));
console.log('OK: empty startup and live ADS-B panel attachment');
