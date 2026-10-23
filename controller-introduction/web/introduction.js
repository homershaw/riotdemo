'use strict';
const introEsc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const introPage=document.getElementById('introPage');
const notice=document.getElementById('demoNotice');
let introBusy=false,introWriting=false,introState=null,introGeneration=0;
const introOpen=new Map();
let introStructure='';
function introValue(t){return typeof t.value==='boolean'?(t.value?'ON':'OFF'):String(t.value);}
function quality(q){return '<span class="'+(q==='GOOD'?'good':'bad')+'">'+introEsc(q)+'</span>';}
function help(message){return '<details class="help"><summary>Help ?</summary><p>'+introEsc(message)+'</p></details>';}
function section(id,title,content,open=false){return '<details class="card" data-intro-sec="'+id+'" '+(open?'open':'')+'><summary><b>'+introEsc(title)+'</b></summary>'+content+'</details>';}
function link(path,label){return '<a href="'+path+'">'+introEsc(label)+'</a>';}
function readonly(message){return '<p class="readOnly">'+introEsc(message)+' Read-only introduction example; no field write is available.</p>';}
async function introRequest(path,params){
 const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),5000);
 try{const r=await fetch(path,{cache:'no-store',signal:controller.signal,...(params?{method:'POST',body:new URLSearchParams(params)}:{})});const data=await r.json();if(!r.ok||data.ok===false)throw Error(data.error||('HTTP '+r.status));return data;}finally{clearTimeout(timer);}
}
function devices(s){return '<div class="deviceGrid">'+s.devices.map(d=>{
 const count=s.tags.filter(t=>t.device===d.id&&t.selected).length;
 return '<article class="card"><div class="introHead"><h3>'+link('/device?id='+d.id,d.name)+'</h3>'+quality(d.online?'GOOD':'BAD')+'</div><p>'+introEsc(d.online?'ONLINE':'OFFLINE')+' · '+introEsc(d.address)+'</p><p class="meta">'+count+' selected / active Device Tags</p></article>';
}).join('')+'</div>';}
function library(s){return help('Device Library contains Device Types. Install a Device Type, then create a Device instance and select its Device Tags. The catalog here is illustrative, not a field-validation claim.')+'<div class="deviceGrid">'+s.profiles.map(p=>'<article class="card"><h3>'+introEsc(p.name)+'</h3><p>Vendor: '+introEsc(p.vendor)+'<br>Model: '+introEsc(p.model)+'<br>Profile ID: '+introEsc(p.id)+'</p><p><b>'+p.available+' Available Tags</b></p><p class="meta">'+introEsc(p.validation)+'</p></article>').join('')+'</div>'+readonly('TOML upload, profile editing, and hardware installation are not implemented.');
}
function deviceDetail(s){
 const id=new URLSearchParams(location.search).get('id'),d=s.devices.find(x=>x.id===id);
 if(!d)return '<h1>Device unavailable</h1>'+link('/devices','Return to Devices');
 const profile=s.profiles.find(p=>p.id===d.profile),tags=s.tags.filter(t=>t.device===id);
 let html='<div class="introHead"><h1>'+introEsc(d.name)+'</h1>'+quality(d.online?'GOOD':'BAD')+'</div><p>Device Type: '+introEsc(profile.name)+' · '+introEsc(d.address)+'</p><div class="introHead"><p>'+introEsc(d.online?'ONLINE':'OFFLINE')+' · '+tags.length+' selected / active Device Tags</p><button data-device="'+id+'" data-online="'+(d.online?0:1)+'">Simulate '+(d.online?'Offline':'Online')+'</button></div>'+help('Devices are equipment installed at this site. Choose a Device Type from Device Library, configure communications, and select Device Tags. All values and output actions in this package are simulated.');
 for(const pool of new Set(tags.map(t=>t.pool))){
  html+=section('pool-'+id+'-'+pool,pool,'<div class="deviceTags">'+tags.filter(t=>t.pool===pool).map(t=>{
   const assigned=s.dashboard.items.find(i=>i.ref===t.ref);
   const output=d.driver==='m31'&&/^do[1-4]$/.test(t.id);
   return '<div class="tagRow"><b>'+introEsc(t.name)+'</b><span data-live="'+introEsc(t.ref)+'">'+introEsc(introValue(t))+' '+introEsc(t.units)+'</span>'+quality(t.quality)+'<div class="actions">'+(output?'<button data-do="'+t.id.slice(2)+'" data-value="'+(t.value?0:1)+'" '+(!d.online?'disabled':'')+'>Simulate '+(t.value?'OFF':'ON')+'</button>':'')+(s.scenario==='jc_first_start'?link('/tag-manager','Edit'):'<label><input type="checkbox" data-dashboard="'+introEsc(t.ref)+'" '+(assigned&&assigned.visible?'checked':'')+'>Dashboard</label>')+'</div><small>'+introEsc(t.ref)+(t.quality==='BAD'?' · last-known value; Device offline':'')+'</small></div>';
  }).join('')+'</div>',true);
 }
 return html+'<p>'+link(s.scenario==='jc_first_start'?'/tag-manager':'/dashboard-layout','Configure Dashboard assignments')+'</p>'+readonly('Communications fields and Tag selection are examples, not a driver configuration editor.');
}
function flow(s){
 const flow=s.tags.find(t=>t.id==='flow_rate'),total=s.tags.find(t=>t.id==='grand_total');
 return '<h1>Flow</h1><article class="card"><h2>Sales Gas</h2><div class="metrics"><div><small>Flow rate</small><b>'+flow.value+' E3m3/day</b></div><div><small>Daily volume (example)</small><b>12.34 E3m3</b></div><div><small>Hourly volume (example)</small><b>1.025 E3m3</b></div><div><small>Grand Total</small><b>'+total.value+' E3m3</b></div></div>'+quality(flow.quality)+'<p>Run state: '+(flow.quality==='GOOD'?'FLOWING':'SOURCE UNAVAILABLE')+'</p></article>'+help('Flow values may be placed in operator-selected Dashboard groups. Sources do not dictate the display group.')+readonly('No Flow calculation engine or custody-transfer calculation is running. Values do not accumulate in this first pass.');
}
function alarms(s){return '<h1>Alarms</h1>'+help('GOOD/BAD is source quality, not an alarm acknowledgement. This introduction demonstrates a high-temperature alarm and communication failures without running the firmware alarm engine.')+(s.alarms.length?s.alarms.map(a=>'<article class="card"><h2 class="bad">'+introEsc(a.name)+'</h2><p>'+a.value+' deg C > '+a.threshold+' deg C · ACTIVE</p></article>').join(''):'<article class="card">No active simulated process alarms.</article>')+s.devices.filter(d=>!d.online).map(d=>'<article class="card"><b class="bad">'+introEsc(d.name)+' communication unavailable</b><p>Device Tags are BAD; last-known values remain visible.</p></article>').join('')+readonly('Acknowledgement, alarm configuration, and notifications are not implemented.');}
function history(s){return '<h1>History</h1><article class="card"><h2>Simulated event history</h2>'+s.events.map(e=>'<p><span class="muted">'+introEsc(e.time)+' UTC</span> · '+introEsc(e.message)+'</p>').join('')+'</article>'+readonly('These are session events, not firmware historical records. No SD archive or history export is provided.');}
function setup(s){return '<h1>Setup</h1>'+
 section('site','Site','<p><b>'+introEsc(s.site.name)+'</b><br>'+introEsc(s.site.location)+'</p>'+readonly('Site metadata'),true)+
 section('library','Device Library',library(s),true)+
 section('devices','Devices',help('Create Devices from installed Device Types, configure communications, then select Device Tags.')+devices(s)+'<p>'+link('/devices','Installed Devices')+' · '+link('/tag-manager','Tags')+'</p>',true)+
 section('communications','Communications','<p>Ethernet: SIMULATED<br>Wi-Fi: OFFLINE<br>MQTT: not connected</p>'+readonly('No network scanning, Modbus transport, or MQTT client exists.'))+
 section('display','Display','<p>'+link('/dashboard-layout','Dashboard Layout')+'</p>'+help('Source determines where a value comes from. Dashboard configuration determines where it appears. A group may mix values from different Devices.'))+
 section('flow','Flow','<p>'+link('/flow','View Sales Gas flow example')+'</p>')+
 section('history','History','<p>'+link('/history','View session history')+'</p>')+
 section('alarms','Alarms','<p>'+link('/alarms','View simulated alarms')+'</p>')+
 section('advanced','Advanced',readonly('Advanced controller settings'));
}
function maintenance(s){return '<h1>Maintenance</h1>'+[
 ['backup','Backup & Restore','No controller backup is generated or restored. Demo edits are saved in state/current_state.json. Reset Demo returns to blank JC first start.'],
 ['firmware','Firmware','WEB1 is not firmware. Flashing, uploading, and firmware replacement are unavailable.'],
 ['storage','Storage','Simulation: local JSON state, restored on restart. No LittleFS or SD card is accessed.'],
 ['diagnostics','Diagnostics',s.devices.map(d=>d.name+': '+(d.online?'ONLINE':'OFFLINE')+' | good '+d.good+' / bad '+d.bad).join('\n')],
 ['logs','Logs',s.events.map(e=>e.time+' UTC | '+e.message).join('\n')],
 ['advanced','Advanced','No real device write, reset, format, or network-scan action exists.']
 ].map(([id,name,body])=>section(id,name,'<pre>'+introEsc(body)+'</pre>'+readonly('Simulated/read-only'),id==='diagnostics')).join('');}
function apiPage(){return '<h1>API</h1><p>Local simulated responses only. These are a subset, not a complete RIOT API contract.</p><article class="card">'+['/api/hmi','/status','/api/time','/api/modbus','/api/library-signals','/tags','/api/dashboard-layout','/api/sim/state'].map(path=>'<p>'+link(path,path)+'</p>').join('')+'</article>'+readonly('Output and scenario POSTs modify only this introduction process.');}
function jcGuide(s){
 return '<section class="card"><div class="introHead"><h2>JC-Riot first start</h2><b>SIMULATED</b></div><p class="meta">Blank application; Communications and system diagnostics retained. No real controller is connected. Demo clock runs at 10x, starting at 2026-10-15 12:00 UTC.</p><ol class="jcSteps"><li>'+link('/setup','Setup -> Devices')+': add the example M31 (LD02).</li><li>'+link('/tag-manager','Tags')+': choose Dashboard and Log; one Save Changes.</li><li>'+link('/','Dashboard')+': watch changing simulated M31 values.</li><li>'+link('/datalog','Datalog')+': enable sampling, then download local sample CSV.</li></ol><p class="meta">Not a full configuration replica. Serial wiring, profile upload, scaling, alarms, retention and SD operations are not commissioned here.</p><p>'+s.devices.length+' simulated Device / '+s.tags.length+' Tags / '+s.dashboard.items.length+' placements / '+s.walkthrough.logged.length+' Log selections</p></section>';
}
function jcTags(s){
 if(!s.tags.length)return '<h1>Tags</h1><p class="card">No configured Tags. '+link('/setup','Add the simulated M31 in Setup -> Devices')+'.</p>';
 return '<h1>Tags <small>SIMULATED</small></h1><form id="jcTags"><div class="jcSave"><button>Save Changes</button><span id="jcDraft" role="status">No pending changes</span></div><div class="tableScroll"><table class="jcTable"><thead><tr><th>Tag / stable demo identity</th><th>Value / quality</th><th>Dashboard</th><th>Log</th></tr></thead><tbody>'+s.tags.map(t=>'<tr><td>'+introEsc(t.name)+' <small>#'+t.identity+'</small></td><td><span data-live="'+introEsc(t.ref)+'"></span></td><td><input aria-label="'+introEsc(t.name)+' Dashboard" type="checkbox" name="dashboard" value="'+introEsc(t.ref)+'" '+(s.dashboard.items.some(i=>i.ref===t.ref)?'checked':'')+'></td><td><input aria-label="'+introEsc(t.name)+' Log" type="checkbox" name="logged" value="'+introEsc(t.ref)+'" '+(s.walkthrough.logged.includes(t.ref)?'checked':'')+'></td></tr>').join('')+'</tbody></table></div></form><p class="meta">Values are simulated; selection changes affect only local demo state. Alarm configuration is outside this walkthrough.</p>';
}
function jcDatalog(s){
 return '<h1>Datalog <small>SIMULATED</small></h1><section class="card"><form id="jcLogger" class="jcControls"><label><input type="checkbox" name="enabled" '+(s.walkthrough.logging?'checked':'')+'>Enable demo logging</label><label>Interval (simulated seconds)<input name="interval" type="number" min="1" max="300" value="'+s.walkthrough.interval+'" required></label><button>Save Changes</button></form><p>Storage: local bounded sample buffer, at most 240 rows. No SD card or filesystem connection. Samples clear on server restart or Reset Demo; selections/settings remain local.</p><p>Selected Tags: '+s.walkthrough.logged.length+' / <span id="jcSamples">'+s.sampleCount+'</span> buffered sample rows. UTC export; every row labelled SIMULATED. BAD readings have an empty value, never a valid zero.</p><a class="rowEdit" href="/sim/sample.csv" download="JC-Riot-SIMULATED.csv">Download sample CSV</a><p class="meta">Default 60-second interval = 6 real seconds at 10x demo speed. An initial sample is available after enabling. Retention is not emulated.</p></section>';
}
function renderJc(s){
 const key=JSON.stringify([location.pathname,s.scenario,s.devices.map(d=>[d.id,d.online]),s.walkthrough,s.dashboard]);
 if(key!==introStructure){
  introStructure=key;
  const p=location.pathname;
  let body;
  if(p==='/')body=jcGuide(s);
  else if(p==='/tag-manager')body=jcTags(s);
  else if(p==='/datalog')body=jcDatalog(s);
  else if(p==='/setup'||p==='/devices')body='<h1>Setup -> Devices</h1>'+jcGuide(s)+'<section class="card"><div class="introHead"><h2>M31 example</h2>'+(s.devices.length?link('/device?id=LD02','Configure'):'<button data-jc-add>Add simulated M31</button>')+'</div><p>Local fixture: 4 AI, 4 DI, 4 DO; instance LD02. Address 192.0.2.31 is documentation only, never contacted.</p>'+(!s.devices.length?'<p>No configured Devices.</p>':devices(s))+'</section><details class="card"><summary>Communications retained (illustrative, read-only)</summary><p>JC UART0 37/38 console; Field UART1 2/47 auto-RS485; Legacy UART2 3/46 RS232; MVS UART3 4/45 auto-RS485. All 9600 8N1. These are walkthrough notes, not detected hardware.</p></details>';
  else if(p==='/device')body=deviceDetail(s);
  else if(p==='/device-library')body='<h1>Device Library</h1><p class="card">Only the M31 example is provided in this walkthrough. '+link('/setup','Add simulated M31')+'. No TOML files are uploaded.</p>';
  else if(p==='/history')body=history(s);
  else if(p==='/api-reference')body=apiPage();
  else if(p==='/maintenance')body='<h1>Maintenance</h1><p class="card">Reset Demo returns to blank JC first start. No firmware, backup, erase, restart or controller action is available.</p>';
  else body='<h1>'+introEsc(p.slice(1))+'</h1><p class="card">No configured records. This first-start walkthrough does not run Flow, alarm, notification or rule engines.</p>';
  introPage.innerHTML=body;
 }
 for(const tag of s.tags)for(const cell of introPage.querySelectorAll('[data-live]'))if(cell.dataset.live===tag.ref)cell.textContent=introValue(tag)+' '+tag.units+' / '+tag.quality+' (simulated)';
 const samples=document.getElementById('jcSamples');if(samples)samples.textContent=s.sampleCount;
}
function renderIntro(s){
 document.getElementById('demoClock').textContent=s.time+' (simulated UTC)';
 if(!introPage)return;
 if(s.scenario==='jc_first_start'){renderJc(s);return;}
 introStructure='';
 if(location.pathname==='/'){introPage.innerHTML='';return;}
 const renderers={'/setup':setup,'/devices':x=>'<h1>Devices</h1>'+help('Installed Device instances belong to this site. Device Library holds reusable Device Types.')+devices(x),'/device-library':x=>'<h1>Device Library</h1>'+library(x),'/device':deviceDetail,'/flow':flow,'/alarms':alarms,'/history':history,'/maintenance':maintenance,'/api-reference':apiPage,'/rules':()=>'<h1>Rules</h1>'+section('rules','Burner permissive example','<p>If Flame Detected is GOOD and ON, display Burner Run status.</p>'+readonly('Illustrative rule only; no rule engine or output automation runs.'),true),'/tag-manager':x=>'<h1>Tags</h1>'+x.tags.map(t=>'<div class="card">'+introEsc(t.name)+' · '+introEsc(introValue(t))+' '+introEsc(t.units)+' · '+quality(t.quality)+'</div>').join('')};
 introPage.innerHTML=(renderers[location.pathname]||(()=>'<h1>Page unavailable</h1>'))(s);
 introPage.querySelectorAll('details[data-intro-sec]').forEach(d=>{
  const key=location.pathname+location.search+':'+d.dataset.introSec;
  if(introOpen.has(key))d.open=introOpen.get(key);
  d.addEventListener('toggle',()=>introOpen.set(key,d.open));
 });
}
async function refreshIntro(){
 if(introBusy||introWriting)return;
 introBusy=true;const generation=introGeneration;
 try{const state=await introRequest('/api/sim/state');if(generation!==introGeneration)return;introState=state;document.getElementById('scenario').value=introState.scenario;if(introState.scenario==='jc_first_start'||!document.activeElement?.closest('#introPage'))renderIntro(introState);}
 catch(err){notice.textContent='Simulation data unavailable: '+err.message;notice.classList.add('bad');}
 finally{introBusy=false;}
}
async function writeIntro(path,params,reload=false){
 if(introWriting)return;introWriting=true;++introGeneration;
 try{await introRequest(path,params);notice.textContent='Simulated change saved locally';notice.classList.remove('bad');if(reload){const draft=document.getElementById('jcDraft');if(draft)draft.dataset.dirty='0';location.reload();return;}introState=await introRequest('/api/sim/state');introStructure='';renderIntro(introState);const feedback=document.getElementById('jcDraft');if(feedback)feedback.textContent='Saved locally (simulated)';}
 catch(err){notice.textContent='Simulated action failed: '+err.message;notice.classList.add('bad');const feedback=document.getElementById('jcDraft');if(feedback)feedback.textContent='Save failed; draft retained: '+err.message;if(introState)document.getElementById('scenario').value=introState.scenario;await refreshAfterFailure();}
 finally{introWriting=false;}
}
async function refreshAfterFailure(){try{introState=await introRequest('/api/sim/state');renderIntro(introState);}catch(err){notice.textContent+='; refresh failed: '+err.message;}}
function allowScenario(){
 if(document.getElementById('jcDraft')?.dataset.dirty==='1'&&!confirm('Discard unsaved simulated Tag selections?'))return false;
 // The extracted layout owns its unsaved-draft guard.
 if(location.pathname==='/dashboard-layout'&&(typeof dlDirty!=='undefined'&&(dlDirty||dlSaving))){notice.textContent='Save Dashboard Layout changes before switching or resetting the demo.';return false;}
 return true;
}
document.getElementById('scenario').addEventListener('change',ev=>{if(!allowScenario()){ev.target.value=introState?.scenario||'normal_site';return;}writeIntro('/sim/scenario',{scenario:ev.target.value},true);});
document.getElementById('resetDemo').addEventListener('click',()=>{if(allowScenario()&&confirm('Return to blank JC first start? Clears local demo Devices, Tags, placements, selections and sample rows.'))writeIntro('/sim/reset',{},true);});
document.getElementById('theme').addEventListener('change',ev=>{document.documentElement.dataset.theme=ev.target.value;});
introPage?.addEventListener('click',ev=>{
 const button=ev.target.closest('button');if(!button)return;
 if(button.hasAttribute('data-jc-add'))writeIntro('/sim/jc',{action:'add-m31'});
 if(button.dataset.do)writeIntro('/m31/output/do',{ch:button.dataset.do,value:button.dataset.value});
 if(button.dataset.device)writeIntro('/sim/device',{id:button.dataset.device,online:button.dataset.online});
});
introPage?.addEventListener('change',ev=>{if(ev.target.dataset.dashboard)writeIntro('/sim/tag-dashboard',{ref:ev.target.dataset.dashboard,visible:ev.target.checked?'1':'0'});});
introPage?.addEventListener('change',ev=>{if(ev.target.closest('#jcTags')){const status=document.getElementById('jcDraft');status.dataset.dirty='1';status.textContent='Unsaved simulated selections';}});
introPage?.addEventListener('submit',ev=>{
 ev.preventDefault();const data=new FormData(ev.target);
 if(ev.target.id==='jcTags')writeIntro('/sim/jc',{action:'tags-save',dashboard:data.getAll('dashboard').join(','),logged:data.getAll('logged').join(',')});
 if(ev.target.id==='jcLogger')writeIntro('/sim/jc',{action:'logger-save',interval:data.get('interval'),enabled:data.has('enabled')?'1':'0'});
});
window.addEventListener('beforeunload',ev=>{if(document.getElementById('jcDraft')?.dataset.dirty==='1'){ev.preventDefault();ev.returnValue='';}});
document.querySelectorAll('nav a').forEach(a=>{if(a.getAttribute('href')===location.pathname)a.setAttribute('aria-current','page');});
(async()=>{try{const result=await introRequest('/api/sim/scenarios');document.getElementById('scenario').innerHTML=result.scenarios.map(s=>'<option value="'+introEsc(s.id)+'">'+introEsc(s.name)+'</option>').join('');await refreshIntro();document.getElementById('scenario').disabled=false;document.getElementById('resetDemo').disabled=false;}catch(err){notice.textContent='Could not initialize introduction: '+err.message;notice.classList.add('bad');}})();
setInterval(refreshIntro,2000);
