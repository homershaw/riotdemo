'use strict';
// Tag-centric design simulator: one Tag table, owners create Tags, consumers read Tags.
const $=id=>document.getElementById(id);
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const page=$('tcPage'),notice=$('tcNotice');
let S=null,structure='',busy=false;
const KIND={m31:'M31 rack',modbus:'Modbus device',mvs:'MVS sensor',flow:'Flow run',calc:'Calculation',manual:'Manual value',pid:'PID loop',feature:'Feature'};
const INPUT_LABEL={dp:'Differential pressure',sp:'Static pressure',pt:'Temperature',tubing:'Tubing pressure',casing:'Casing pressure',line:'Line pressure',arrival:'Plunger arrival',flow:'Flow rate',lshh:'LSHH',pshh:'PSHH',esd:'ESD push button',valve:'Well valve (DO)',siteDo:'Site shutdown output (DO)'};
const DIGITAL_INPUTS=new Set(['arrival','lshh','pshh','esd']);

function say(text,cls){notice.textContent=text;notice.className=cls||'';}
async function act(body,quiet){
 const r=await fetch('/api/tc/action',{method:'POST',body:new URLSearchParams({json:JSON.stringify(body)})});
 const d=await r.json();
 if(!r.ok||d.ok===false&&!d.needsConfirm)throw Error(d.error||('HTTP '+r.status));
 if(!quiet)say('Saved '+new Date().toLocaleTimeString(),'good');
 return d;
}
function fmt(t){
 if(t.value===null||t.value===undefined)return '--';
 if(t.states&&t.states[String(Math.round(t.value))])return t.states[String(Math.round(t.value))];
 if(t.kind==='digital')return t.value?'ON':'OFF';
 const v=Number(t.value),a=Math.abs(v);
 return (a>=1000?v.toFixed(0):a>=100?v.toFixed(1):a>=1?v.toFixed(2):v.toFixed(3))+(t.units?' '+t.units:'');
}
function qual(t){return t.quality==='GOOD'?(t.alarm?'<span class="bad">'+t.alarm+'</span>':'<span class="good">GOOD</span>'):'<span class="bad">'+esc(t.quality)+'</span>';}
function live(t){return '<span data-v="'+t.id+'">'+esc(fmt(t))+'</span> <small data-q="'+t.id+'">'+qual(t)+'</small>';}
function ownerLink(o){return '<a href="/tc/owner?id='+o.id+'">'+esc(o.name)+'</a>';}
function tagsOf(id){return S.tags.filter(t=>t.owner===id);}
function tagOptions(sel,filter){
 const opts=S.tags.filter(filter).map(t=>'<option value="'+t.id+'"'+(t.id===sel?' selected':'')+'>'+esc(t.name)+(t.units?' ('+esc(t.units)+')':'')+'</option>').join('');
 const gone=sel&&typeof sel==='object'?'<option value="" selected>Tag removed ('+esc(sel.removed)+')</option>':'';
 return '<option value="">Not selected</option>'+gone+opts;
}
function refText(ref){if(!ref)return '<span class="muted">not selected</span>';if(typeof ref==='object')return '<span class="bad">Tag removed ('+esc(ref.removed)+')</span>';const t=S.tags.find(x=>x.id===ref);return t?esc(t.name)+' = '+live(t):'?';}

// ---------- pages ----------
function dashboard(){
 const dash=S.tags.filter(t=>t.dash);
 return '<div class="tcHead"><h1>Dashboard</h1><a class="rowEdit" href="/tc/tags">Configure</a></div>'+
  (dash.length?'<div class="tcCards">'+dash.map(t=>'<article class="card"><small class="muted">'+esc(t.ownerName)+'</small><div class="tcName">'+esc(t.name)+'</div><div class="tcBig">'+live(t)+'</div></article>').join('')+'</div>':'<p class="card">No Tags are on the Dashboard. Tick <b>Dash</b> on rows in <a href="/tc/tags">Tags</a>.</p>')+
  '<h2>Owners</h2><div class="tcCards">'+S.owners.map(o=>'<article class="card"><small class="muted">'+esc(KIND[o.kind])+'</small><div class="tcName">'+ownerLink(o)+'</div><div>'+o.tagCount+' Tags · '+(o.enabled?(o.online===false?'<span class="bad">OFFLINE</span>':'<span class="good">ENABLED</span>'):'<span class="bad">DISABLED</span>')+'</div></article>').join('')+'</div>';
}

function tagsPage(){
 const q=new URLSearchParams(location.search),owner=q.get('owner')||'';
 const rows=S.tags.map(t=>{
  const rng=t.raw?'<span class="tcRange">'+t.raw[0]+'-'+t.raw[1]+' '+esc(t.rawUnits)+' = <input data-f="eu0" type="number" step="any" value="'+t.eu[0]+'"> to <input data-f="eu1" type="number" step="any" value="'+t.eu[1]+'"></span><small class="muted" data-raw="'+t.id+'">'+(t.rawValue??'--')+' mA</small>':'<span class="muted">'+(t.ownerKind==='modbus'||t.ownerKind==='mvs'?'from device':'--')+'</span>';
  const al=t.kind==='analog'?['hh','h','l','ll'].map(l=>'<input data-f="'+l+'" type="number" step="any" placeholder="'+l.toUpperCase()+'" aria-label="'+l.toUpperCase()+' alarm" value="'+(t.alarms[l]??'')+'">').join(''):'<span class="muted">--</span>';
  const set=t.io==='setpoint'||(t.io==='out'&&!t.writer)?'<button type="button" data-set="'+t.id+'">Set</button>':t.writer?'<small class="muted">by '+esc(t.writer)+'</small>':'';
  return '<tr data-id="'+t.id+'" data-owner="'+t.owner+'"'+(owner&&String(t.owner)!==owner?' hidden':'')+'>'+
   '<td class="nameCell"><input data-f="name" value="'+esc(t.name)+'" aria-label="Name"><small class="muted">#'+t.id+' · <a href="/tc/owner?id='+t.owner+'">'+esc(t.ownerName)+'</a>.'+esc(t.point)+(t.description?' · '+esc(t.description):'')+'</small></td>'+
   '<td data-label="Value" class="valueCell">'+live(t)+' '+set+'</td>'+
   '<td data-label="Units"><input data-f="units" class="tcUnits" value="'+esc(t.units)+'" maxlength="11"></td>'+
   '<td data-label="Scaling">'+rng+'</td>'+
   '<td data-label="Log"><input type="checkbox" data-f="log"'+(t.log?' checked':'')+' aria-label="Log"></td>'+
   '<td data-label="Dash"><input type="checkbox" data-f="dash"'+(t.dash?' checked':'')+' aria-label="Dashboard"></td>'+
   '<td data-label="Alarms" class="tcAlarms">'+al+'</td>'+
   '<td data-label="Modbus"><input data-f="modbus" class="tcReg" type="number" min="40001" max="49999" value="'+(t.modbus??'')+'"></td>'+
   '<td data-label="Used by"><small data-u="'+t.id+'">'+esc(t.usedBy.join(', ')||'-')+'</small></td></tr>';
 }).join('');
 return '<div class="tcHead"><h1>Tags <small class="muted">'+S.tags.length+' / '+S.measure.tagLimit+'</small></h1>'+
  '<input id="tcSearch" placeholder="Search name, owner or use" aria-label="Search Tags"><select id="tcOwnerFilter" aria-label="Owner"><option value="">All owners</option>'+S.owners.map(o=>'<option value="'+o.id+'"'+(String(o.id)===owner?' selected':'')+'>'+esc(o.name)+'</option>').join('')+'</select></div>'+
  '<p class="tcNote">Every value is a Tag owned by the device or function that created it. Rename, scale, log, show, alarm and map it here; one Save applies all changed rows. Add Tags by adding an owner in <a href="/tc/devices">Devices</a>, <a href="/tc/flow">Flow and Control</a> or <a href="/tc/features">Features</a>.</p>'+
  (S.tags.length?'<form id="tcTags"><div class="tcSave"><button id="tcSaveTags" disabled>Save changes</button><button type="button" id="tcReload">Discard</button><span id="tcDirty" role="status">No unsaved changes</span></div><div class="tableWrap"><table class="tcTable"><thead><tr><th>Name / owner</th><th>Value</th><th>Units</th><th>Scaling (raw = engineering)</th><th>Log</th><th>Dash</th><th>Alarms HH H L LL</th><th>Modbus</th><th>Used by</th></tr></thead><tbody>'+rows+'</tbody></table></div></form>':'<p class="card">No Tags yet. Add a device in <a href="/tc/devices">Devices</a> or load an example site above.</p>');
}

function devicesPage(){
 const prof=Object.entries(S.profiles);
 const group=(kinds,title)=>{const list=S.owners.filter(o=>kinds.includes(o.kind));return '<h2>'+title+'</h2>'+(list.length?'<div class="tcCards">'+list.map(ownerCard).join('')+'</div>':'<p class="muted">None.</p>');};
 return '<h1>Devices</h1><p class="tcNote">Adding a device creates its Tags; removing it removes them after a preview of what uses them. Calculations and manual values are owners too.</p>'+
  '<details class="card" open><summary><b>Add a device or value</b></summary><div class="tcAdd">'+
  '<form data-add="m31"><b>M31 rack</b><span class="tcFields">'+['ai','ao','di','do'].map(k=>'<label>'+k.toUpperCase()+' <input name="'+k+'" type="number" min="0" max="16" value="'+(k==='ai'?8:4)+'"></label>').join('')+'</span><button>Add M31</button></form>'+
  '<form data-add="modbus"><b>Modbus device</b><span class="tcFields"><label>Profile <select name="profile">'+prof.map(([k,p])=>'<option value="'+k+'">'+esc(p.name)+'</option>').join('')+'</select></label><label>Name <input name="name" maxlength="16" placeholder="e.g. SALES"></label></span><button>Add device</button><small class="muted">All profile points are selected; untick points on the device page.</small></form>'+
  '<form data-add="mvs"><b>MVS sensor</b><span class="tcFields muted">DP, static pressure, temperature</span><button>Add MVS</button></form>'+
  '<form data-add="manual"><b>Manual value</b><span class="tcFields"><label>Name <input name="name" maxlength="35" required placeholder="e.g. WELL_DEPTH"></label><label>Units <input name="units" maxlength="11" class="tcUnits"></label><label>Type <select name="type"><option value="analog">Number</option><option value="digital">On/off</option></select></label></span><button>Add value</button></form>'+
  '<form data-add="calc"><b>Calculation</b><span class="tcFields"><label>Name <input name="name" maxlength="35" required placeholder="e.g. DIFF_PRESS"></label><label>Formula <input name="formula" required placeholder="CASING_PRESS - TUBING_PRESS" class="tcFormula"></label><label>Units <input name="units" maxlength="11" class="tcUnits"></label></span><button>Add calculation</button></form>'+
  '</div></details>'+
  group(['m31'],'M31 racks')+group(['modbus'],'Modbus devices')+group(['mvs'],'MVS sensors')+group(['calc','manual'],'Calculations and manual values')+
  '<div id="tcRemove"></div>';
}
function ownerCard(o){
 return '<article class="card"><div class="tcHead"><span class="tcName">'+ownerLink(o)+'</span><small class="muted">'+esc(o.label)+'</small></div><div>'+o.tagCount+' Tags · '+(o.enabled?(o.online===false?'<span class="bad">OFFLINE</span>':'<span class="good">ENABLED</span>'):'<span class="bad">DISABLED</span>')+'</div><div class="tcActions"><a class="rowEdit" href="/tc/tags?owner='+o.id+'">Tags</a><a class="rowEdit" href="/tc/owner?id='+o.id+'">Configure</a><button type="button" data-remove="'+o.id+'">Remove</button></div></article>';
}

function flowPage(){
 const flows=S.owners.filter(o=>o.kind==='flow'),pids=S.owners.filter(o=>o.kind==='pid');
 const v=(o,p)=>{const t=S.tags.find(x=>x.owner===o.id&&x.point===p);return t?live(t):'--';};
 return '<h1>Flow and Control</h1><p class="tcNote">Flow runs and PID loops are scheduled calculations. Each creates its own Tags and picks its inputs from the Tag table. Flow results here are illustrative, not AGA-3.</p>'+
  '<div class="tcHead"><h2>Flow runs <small class="muted">'+flows.length+' / 6</small></h2><button type="button" data-addkind="flow"'+(flows.length>=6?' disabled':'')+'>Add Flow run</button></div>'+
  (flows.length?'<table class="tcTable"><thead><tr><th>Run</th><th>DP / static / temperature</th><th>Rate</th><th>Hour</th><th>Day (06:00)</th><th>Grand</th></tr></thead><tbody>'+flows.map(o=>'<tr><td>'+ownerLink(o)+'</td><td><small>'+['dp','sp','pt'].map(k=>o.inputs[k]?refText(o.inputs[k]):'manual '+o.manual[k]).join('<br>')+'</small></td><td>'+v(o,'RATE')+'</td><td>'+v(o,'HOUR')+'</td><td>'+v(o,'DAY')+'</td><td>'+v(o,'GRAND')+'</td></tr>').join('')+'</tbody></table>':'<p class="muted">No Flow runs.</p>')+
  '<div class="tcHead"><h2>PID loops <small class="muted">'+pids.length+' / '+S.measure.pidLimit+' (planning limit until measured)</small></h2><button type="button" data-addkind="pid"'+(pids.length>=S.measure.pidLimit?' disabled':'')+'>Add PID loop</button></div>'+
  (pids.length?'<table class="tcTable"><thead><tr><th>Loop</th><th>Process variable</th><th>Setpoint</th><th>Output</th><th>Mode</th></tr></thead><tbody>'+pids.map(o=>{const sp=S.tags.find(t=>t.owner===o.id&&t.point==='SP'),au=S.tags.find(t=>t.owner===o.id&&t.point==='AUTO');return '<tr><td>'+ownerLink(o)+'</td><td><small>'+refText(o.pv)+'</small></td><td>'+live(sp)+' <button type="button" data-set="'+sp.id+'">Set</button></td><td>'+v(o,'OUT')+'<br><small>to '+refText(o.out)+'</small></td><td><button type="button" data-auto="'+au.id+'" data-on="'+(au.value?0:1)+'">'+(au.value?'Auto: switch to Manual':'Manual: switch to Auto')+'</button></td></tr>';}).join('')+'</tbody></table>':'<p class="muted">No PID loops.</p>');
}

function featuresPage(){
 const inst=S.owners.filter(o=>o.kind==='feature');
 return '<h1>Features</h1><p class="tcNote">Features are add-ons any controller can carry. Adding one creates its Tags (state, step, setpoints); its inputs and outputs are picked from the Tag table. The Optimizer logic here is a simplified illustration of the cycle, not the v5 or v8.00 program.</p>'+
  '<div class="tcCards">'+Object.entries(S.features).map(([k,f])=>'<article class="card"><div class="tcName">'+esc(f.name)+'</div><small class="muted">Inputs: '+f.inputs.map(i=>esc(INPUT_LABEL[i])).join(', ')+'</small><div class="tcActions"><button type="button" data-feature="'+k+'"'+(inst.length?' disabled':'')+'>Add</button></div></article>').join('')+'</div>'+
  '<h2>Installed</h2>'+(inst.length?inst.map(o=>'<article class="card"><div class="tcHead"><span class="tcName">'+ownerLink(o)+'</span><small class="muted">'+esc(o.featureName)+'</small>'+(o.running?'<span class="good">RUNNING</span>':'<span class="muted">STOPPED</span>')+'</div><div class="tcGrid">'+tagsOf(o.id).map(t=>'<span><small class="muted">'+esc(t.name)+'</small><br>'+live(t)+'</span>').join('')+'</div><div class="tcActions"><a class="rowEdit" href="/tc/owner?id='+o.id+'">Inputs and outputs</a><button type="button" data-run="'+o.id+'" data-on="'+(o.running?0:1)+'">'+(o.running?'Stop':'Start')+'</button><button type="button" data-remove="'+o.id+'">Remove</button></div></article>').join(''):'<p class="muted">No feature installed.</p>')+'<div id="tcRemove"></div>';
}

function ownerPage(){
 const id=Number(new URLSearchParams(location.search).get('id')),o=S.owners.find(x=>x.id===id);
 if(!o)return '<h1>Owner not found</h1><p><a href="/tc/devices">Devices</a></p>';
 let f='<label>Name <input name="name" value="'+esc(o.name)+'" maxlength="35"></label><label><input type="checkbox" name="enabled"'+(o.enabled?' checked':'')+'> Enabled</label>';
 if(['m31','modbus','mvs'].includes(o.kind))f+='<label><input type="checkbox" name="online"'+(o.online?' checked':'')+'> Communicating (simulate)</label>';
 if(o.kind==='m31')f+=['ai','ao','di','do'].map(k=>'<label>'+k.toUpperCase()+' <input name="'+k+'" type="number" min="0" max="16" value="'+o[k]+'"></label>').join('');
 if(o.kind==='modbus')f+='<fieldset><legend>Points (a ticked point has a Tag)</legend>'+S.profiles[o.profile].points.map(p=>'<label><input type="checkbox" name="sel" value="'+esc(p.key)+'"'+(o.selected.includes(p.key)?' checked':'')+'> '+esc(p.key)+' <small class="muted">'+esc(p.label)+(p.units?' '+esc(p.units):'')+'</small></label>').join('')+'</fieldset>';
 if(o.kind==='flow')f+=['dp','sp','pt'].map(k=>'<label>'+INPUT_LABEL[k]+' <select name="in_'+k+'">'+tagOptions(o.inputs[k],t=>t.kind==='analog'&&t.owner!==o.id)+'</select></label><label>Manual '+k.toUpperCase()+' <input name="man_'+k+'" type="number" step="any" value="'+o.manual[k]+'"></label>').join('');
 if(o.kind==='calc')f+='<label>Formula <input name="formula" class="tcFormula" value="'+esc(o.formula)+'"></label>';
 if(o.kind==='pid')f+='<label>Process variable <select name="pv">'+tagOptions(o.pv,t=>t.kind==='analog'&&t.owner!==o.id&&t.io!=='out')+'</select></label><label>Output to <select name="out">'+tagOptions(o.out,t=>t.kind==='analog'&&t.io==='out'&&(!t.writer||t.writer===o.name))+'</select></label>'+['kp','ki','kd'].map(k=>'<label>'+k.toUpperCase()+' <input name="'+k+'" type="number" step="any" value="'+o[k]+'"></label>').join('')+'<label>Interval <select name="interval"><option value="1"'+(o.interval>=1?' selected':'')+'>1 s</option><option value="0.1"'+(o.interval<1?' selected':'')+'>100 ms</option></select></label><label><input type="checkbox" name="reverse"'+(o.reverse?' checked':'')+'> Reverse acting</label><label><input type="checkbox" name="simulate"'+(o.simulate?' checked':'')+'> Simulated process (output moves the PV)</label>';
 if(o.kind==='feature'){
  f+=Object.keys(o.inputs).map(k=>'<label>'+INPUT_LABEL[k]+' <select name="in_'+k+'">'+tagOptions(o.inputs[k],t=>(DIGITAL_INPUTS.has(k)?t.kind==='digital':t.kind==='analog')&&t.owner!==o.id)+'</select></label>').join('');
  f+=Object.keys(o.outputs).map(k=>'<label>'+INPUT_LABEL[k]+' <select name="out_'+k+'">'+tagOptions(o.outputs[k],t=>t.kind==='digital'&&t.io==='out'&&(!t.writer||t.writer===o.name))+'</select></label>').join('');
 }
 const rep=o.kind==='modbus'?'<details class="card"><summary><b>Replace device</b></summary><p class="tcNote">Swap the instrument for another profile. Each Tag you match keeps its ID, name, history, alarms and every use; unmatched Tags are removed.</p><label>New profile <select id="tcRepProfile">'+Object.entries(S.profiles).map(([k,p])=>'<option value="'+k+'"'+(k===o.profile?' selected':'')+'>'+esc(p.name)+'</option>').join('')+'</select></label> <button type="button" id="tcRepPreview">Match points</button><div id="tcRepMap"></div></details>':'';
 return '<div class="tcHead"><h1>'+esc(o.name)+'</h1><small class="muted">'+esc(o.label)+'</small></div>'+
  '<form id="tcOwner" class="card tcForm">'+f+'<div class="tcSave"><button>Save</button><span id="tcOwnerMsg" role="status"></span></div></form>'+rep+
  '<h2>Tags of '+esc(o.name)+' <a class="rowEdit" href="/tc/tags?owner='+o.id+'">Edit in Tags</a></h2><table class="tcTable"><thead><tr><th>Name</th><th>Point</th><th>Value</th><th>Used by</th></tr></thead><tbody>'+tagsOf(o.id).map(t=>'<tr><td>'+esc(t.name)+'</td><td>'+esc(t.point)+'</td><td>'+live(t)+'</td><td><small data-u="'+t.id+'">'+esc(t.usedBy.join(', ')||'-')+'</small></td></tr>').join('')+'</tbody></table>'+
  '<p><button type="button" data-remove="'+o.id+'">Remove '+esc(o.name)+'</button></p><div id="tcRemove"></div>';
}

function loggerPage(){
 const logged=S.tags.filter(t=>t.log);
 return '<h1>Logger</h1><form id="tcLogger" class="card tcForm"><label><input type="checkbox" name="enabled"'+(S.logger.enabled?' checked':'')+'> Logging on</label><label>Interval (s) <input name="interval" type="number" min="10" max="3600" value="'+S.logger.interval+'"></label><div class="tcSave"><button>Save</button></div></form>'+
  '<p>'+logged.length+' Tags have Log ticked · <span id="tcSamples">'+S.samples+'</span> rows buffered (last 2000) · <a class="rowEdit" href="/tc/log.csv">Download CSV</a></p><p class="tcNote">Logged Tags are chosen with the Log column in <a href="/tc/tags">Tags</a>. Rows are keyed by Tag ID, so a rename does not split a trend. BAD readings have an empty value, never zero.</p>'+
  '<table class="tcTable"><thead><tr><th>Tag</th><th>Owner</th><th>Value</th></tr></thead><tbody>'+logged.map(t=>'<tr><td>'+esc(t.name)+'</td><td>'+esc(t.ownerName)+'</td><td>'+live(t)+'</td></tr>').join('')+'</tbody></table>';
}

function maintenancePage(){
 const m=S.measure,pct=Math.round(m.tags*100/m.tagLimit);
 const kinds=Object.entries(m.byOwnerKind).sort((a,b)=>b[1]-a[1]);
 return '<h1>Maintenance</h1><section class="card"><h2>Measurements</h2>'+
  '<div class="tcMeter"><div class="tcBar"><span style="width:'+Math.min(100,pct)+'%" class="'+(pct>=90?'tcHigh':'')+'"></span></div><b>'+m.tags+' / '+m.tagLimit+' Tags</b> ('+pct+'%)</div>'+
  '<table class="tcTable tcMeasure"><tbody>'+
  '<tr><td>Tag memory</td><td>'+(m.memoryBytes/1024).toFixed(1)+' KB of '+(m.tagLimit*m.tagBytes/1024).toFixed(1)+' KB</td><td class="muted">planning estimate '+m.tagBytes+' B per Tag; the controller shows measured bytes</td></tr>'+
  '<tr><td>Owners</td><td>'+m.owners+'</td><td>'+kinds.map(([k,n])=>esc(KIND[k])+' '+n).join(' · ')+'</td></tr>'+
  '<tr><td>PID loops</td><td>'+m.pidLoops+' / '+m.pidLimit+'</td><td class="muted">planning limit until measured at 1 s and 100 ms on a JC and a Nano</td></tr>'+
  '<tr><td>Logged Tags</td><td>'+m.logged+'</td><td></td></tr><tr><td>Dashboard Tags</td><td>'+m.dashboard+'</td><td></td></tr><tr><td>Alarmed Tags</td><td>'+m.alarmed+'</td><td></td></tr><tr><td>Modbus-mapped Tags</td><td>'+m.modbus+'</td><td></td></tr>'+
  '<tr><td>Scan time</td><td><span id="tcScan">'+m.scanMsAvg+' ms avg / '+m.scanMsMax+' ms max</span></td><td class="muted">measured in this simulator on this PC; the controller measures its own scan</td></tr>'+
  '</tbody></table></section><section class="card"><h2>Audit log</h2>'+S.events.slice().reverse().map(e=>'<p class="tcEvent"><span class="muted">'+esc(e.time)+'</span> '+esc(e.message)+'</p>').join('')+'</section>';
}

// ---------- render and live refresh ----------
function render(){
 const p=location.pathname;
 page.innerHTML=p==='/tc/tags'?tagsPage():p==='/tc/devices'?devicesPage():p==='/tc/owner'?ownerPage():p==='/tc/flow'?flowPage():p==='/tc/features'?featuresPage():p==='/tc/logger'?loggerPage():p==='/tc/maintenance'?maintenancePage():dashboard();
 if(p==='/tc/tags')wireTags();
}
function structureKey(){
 const p=location.pathname;
 const base=[p,location.search,S.owners.map(o=>JSON.stringify(o.inputs||'')+o.id+o.name+o.tagCount+o.enabled+o.online+o.running+(o.pv||'')+(o.out||'')),S.tags.length,S.logger.enabled];
 if(p==='/tc/tags'||p==='/tc/owner')base.push(S.tags.map(t=>[t.id,t.name,t.units,t.log,t.dash,JSON.stringify(t.alarms),t.modbus,t.eu].join()).join('|'));
 else base.push(S.tags.map(t=>t.id+t.name+t.dash+t.log).join('|'));
 if(p==='/tc/maintenance')base.push(S.events.length);
 return JSON.stringify(base);
}
function updateLive(){
 for(const t of S.tags){
  for(const el of document.querySelectorAll('[data-v="'+t.id+'"]'))el.textContent=fmt(t);
  for(const el of document.querySelectorAll('[data-q="'+t.id+'"]'))el.innerHTML=qual(t);
  for(const el of document.querySelectorAll('[data-u="'+t.id+'"]'))el.textContent=t.usedBy.join(', ')||'-';
  for(const el of document.querySelectorAll('[data-raw="'+t.id+'"]'))el.textContent=(t.rawValue??'--')+' mA';
 }
 const scan=$('tcScan');if(scan)scan.textContent=S.measure.scanMsAvg+' ms avg / '+S.measure.scanMsMax+' ms max';
 const n=$('tcSamples');if(n)n.textContent=S.samples;
}
function dirty(){return !!document.querySelector('[data-dirty]')||document.activeElement&&page.contains(document.activeElement)&&document.activeElement.matches('input,select')&&location.pathname!=='/tc';}
async function refresh(force){
 if(busy)return;busy=true;
 try{
  const r=await fetch('/api/tc/state',{cache:'no-store'});S=await r.json();
  $('tcClock').textContent=S.time+' UTC';
  const key=structureKey();
  if(force||(key!==structure&&!dirty())){structure=key;render();}
  updateLive();
 }catch(e){say('Simulator unavailable: '+e.message,'bad');}
 finally{busy=false;}
}

// ---------- Tags table editing ----------
function rowDraft(tr){
 const t=S.tags.find(x=>x.id===Number(tr.dataset.id)),g=f=>tr.querySelector('[data-f="'+f+'"]');
 const row={id:t.id,name:g('name').value.trim(),units:g('units').value,log:g('log').checked,dash:g('dash').checked,modbus:g('modbus').value};
 if(t.raw)row.eu=[g('eu0').value,g('eu1').value];
 if(t.kind==='analog'){row.alarms={};for(const l of ['hh','h','l','ll'])row.alarms[l]=g(l).value;}
 return [t,row];
}
function rowChanged(t,row){
 if(row.name!==t.name||row.units!==t.units||row.log!==t.log||row.dash!==t.dash||String(row.modbus)!==String(t.modbus??''))return true;
 if(row.eu&&(Number(row.eu[0])!==t.eu[0]||Number(row.eu[1])!==t.eu[1]))return true;
 if(row.alarms)for(const l of ['hh','h','l','ll'])if(String(row.alarms[l])!==String(t.alarms[l]??''))return true;
 return false;
}
function wireTags(){
 const form=$('tcTags');if(!form)return;
 const count=()=>{let n=0;for(const tr of form.querySelectorAll('tbody tr')){const [t,row]=rowDraft(tr);const c=rowChanged(t,row);if(c){tr.dataset.dirty='1';n++;}else delete tr.dataset.dirty;}$('tcSaveTags').disabled=!n;$('tcDirty').textContent=n?n+' row'+(n>1?'s':'')+' changed':'No unsaved changes';};
 form.addEventListener('input',count);form.addEventListener('change',count);
 form.addEventListener('submit',async e=>{e.preventDefault();const rows=[];for(const tr of form.querySelectorAll('tbody tr[data-dirty]'))rows.push(rowDraft(tr)[1]);
  try{await act({op:'saveTags',rows});for(const tr of form.querySelectorAll('[data-dirty]'))delete tr.dataset.dirty;structure='';await refresh(true);$('tcDirty').textContent='Saved '+rows.length+' row'+(rows.length>1?'s':'');}catch(err){$('tcDirty').textContent='Not saved: '+err.message;$('tcDirty').className='bad';}});
 $('tcReload').onclick=()=>{for(const tr of form.querySelectorAll('[data-dirty]'))delete tr.dataset.dirty;structure='';refresh(true);};
 const filter=()=>{const q=$('tcSearch').value.toLowerCase(),o=$('tcOwnerFilter').value;for(const tr of form.querySelectorAll('tbody tr')){const t=S.tags.find(x=>x.id===Number(tr.dataset.id));const hay=(t.name+' '+t.ownerName+' '+t.point+' '+t.usedBy.join(' ')).toLowerCase();tr.hidden=!!(o&&String(t.owner)!==o)||!!(q&&!hay.includes(q));}};
 $('tcSearch').oninput=filter;$('tcOwnerFilter').onchange=()=>{history.replaceState(null,'',$('tcOwnerFilter').value?'/tc/tags?owner='+$('tcOwnerFilter').value:'/tc/tags');filter();};
}

// ---------- actions ----------
page.addEventListener('submit',async e=>{
 const f=e.target;
 if(f.dataset.add){e.preventDefault();const p={kind:f.dataset.add};for(const [k,v] of new FormData(f))p[k]=v;
  try{await act({op:'addOwner',params:p});structure='';refresh(true);}catch(err){say(err.message,'bad');}}
 if(f.id==='tcOwner'){e.preventDefault();const id=Number(new URLSearchParams(location.search).get('id')),o=S.owners.find(x=>x.id===id),fd=new FormData(f),p={name:fd.get('name'),enabled:fd.has('enabled')};
  if(['m31','modbus','mvs'].includes(o.kind))p.online=fd.has('online');
  if(o.kind==='m31')for(const k of ['ai','ao','di','do'])p[k]=fd.get(k);
  if(o.kind==='modbus')p.selected=fd.getAll('sel');
  if(o.kind==='flow'){p.inputs={};p.manual={};for(const k of ['dp','sp','pt']){p.inputs[k]=fd.get('in_'+k);p.manual[k]=fd.get('man_'+k);}}
  if(o.kind==='calc')p.formula=fd.get('formula');
  if(o.kind==='pid'){for(const k of ['kp','ki','kd','interval','pv','out'])p[k]=fd.get(k);p.reverse=fd.has('reverse');p.simulate=fd.has('simulate');}
  if(o.kind==='feature'){p.inputs={};p.outputs={};for(const k of Object.keys(o.inputs))p.inputs[k]=fd.get('in_'+k);for(const k of Object.keys(o.outputs))p.outputs[k]=fd.get('out_'+k);}
  try{let d=await act({op:'changeOwner',owner:id,params:p},true);
   if(d.needsConfirm){if(!confirm('This change removes Tags that are in use:\n\n'+d.removed.map(t=>t.name+': '+t.uses.join(', ')).join('\n')+'\n\nTheir uses become "Tag removed". Continue?'))return;d=await act({op:'changeOwner',owner:id,params:p,confirm:true},true);}
   $('tcOwnerMsg').textContent='Saved';$('tcOwnerMsg').className='good';structure='';refresh(true);}catch(err){$('tcOwnerMsg').textContent='Not saved: '+err.message;$('tcOwnerMsg').className='bad';}}
 if(f.id==='tcLogger'){e.preventDefault();const fd=new FormData(f);try{await act({op:'logger',enabled:fd.has('enabled'),interval:fd.get('interval')});structure='';refresh(true);}catch(err){say(err.message,'bad');}}
});
page.addEventListener('click',async e=>{
 const b=e.target.closest('button');if(!b)return;
 try{
  if(b.dataset.addkind){await act({op:'addOwner',params:{kind:b.dataset.addkind}});structure='';refresh(true);}
  else if(b.dataset.feature){await act({op:'addOwner',params:{kind:'feature',feature:b.dataset.feature}});structure='';refresh(true);}
  else if(b.dataset.run){await act({op:'changeOwner',owner:Number(b.dataset.run),params:{running:b.dataset.on==='1'}});structure='';refresh(true);}
  else if(b.dataset.auto){await act({op:'setValue',tag:Number(b.dataset.auto),value:Number(b.dataset.on)});structure='';refresh(true);}
  else if(b.dataset.set){const t=S.tags.find(x=>x.id===Number(b.dataset.set));const v=prompt('New value for '+t.name+(t.kind==='digital'?' (1 = ON, 0 = OFF)':t.io==='out'?' (0-100 %)':(t.units?' ('+t.units+')':'')),t.kind==='digital'?(t.value?0:1):(t.value??''));if(v===null)return;await act({op:'setValue',tag:t.id,value:v});refresh(true);}
  else if(b.dataset.remove){const d=await act({op:'removePreview',owner:Number(b.dataset.remove)},true);const box=$('tcRemove');
   box.innerHTML='<section class="card tcConfirm"><h2>Remove '+esc(d.owner)+'?</h2><p>'+d.tags.length+' Tags are removed with it. History files stay on the SD card. Uses below become "Tag removed".</p><table class="tcTable"><tbody>'+d.tags.map(t=>'<tr><td>'+esc(t.name)+'</td><td>'+(t.uses.length?'<span class="bad">'+esc(t.uses.join(', '))+'</span>':'<span class="muted">not used</span>')+'</td></tr>').join('')+'</tbody></table><button type="button" data-confirmremove="'+b.dataset.remove+'">Remove</button> <button type="button" data-cancel="1">Cancel</button></section>';box.scrollIntoView({behavior:'smooth'});}
  else if(b.dataset.confirmremove){await act({op:'removeOwner',owner:Number(b.dataset.confirmremove)});if(location.pathname==='/tc/owner')location.href='/tc/devices';structure='';refresh(true);}
  else if(b.dataset.cancel){$('tcRemove').innerHTML='';}
  else if(b.id==='tcRepPreview'){const id=Number(new URLSearchParams(location.search).get('id'));const d=await act({op:'replacePreview',owner:id,profile:$('tcRepProfile').value},true);
   $('tcRepMap').innerHTML='<p>'+esc(d.from)+' to '+esc(d.to)+'</p><table class="tcTable"><thead><tr><th>Tag</th><th>Used by</th><th>New point</th></tr></thead><tbody>'+d.tags.map(t=>'<tr><td>'+esc(t.name)+' <small class="muted">'+esc(t.point)+'</small></td><td><small>'+esc(t.uses.join(', ')||'-')+'</small></td><td><select data-map="'+t.id+'"><option value="">Remove this Tag</option>'+d.points.map(p=>'<option value="'+esc(p.key)+'"'+(p.key===t.suggest?' selected':'')+'>'+esc(p.key+' '+p.label)+'</option>').join('')+'</select></td></tr>').join('')+'</tbody></table><button type="button" id="tcRepSave">Replace device</button>';}
  else if(b.id==='tcRepSave'){const id=Number(new URLSearchParams(location.search).get('id')),mapping={};for(const s of document.querySelectorAll('[data-map]'))mapping[s.dataset.map]=s.value;
   if(!confirm('Replace the device? Matched Tags keep their ID, history and uses.'))return;await act({op:'replaceOwner',owner:id,profile:$('tcRepProfile').value,mapping});structure='';refresh(true);}
 }catch(err){say(err.message,'bad');}
});
page.addEventListener('change',e=>{if(e.target.closest('#tcOwner'))e.target.closest('#tcOwner').dataset.dirty='1';});
$('tcLoad').onclick=async()=>{if(!confirm('Load the example site? The current simulated configuration is replaced.'))return;try{await act({op:'example',name:$('tcExample').value});structure='';refresh(true);}catch(err){say(err.message,'bad');}};
window.addEventListener('beforeunload',e=>{if(document.querySelector('[data-dirty]')){e.preventDefault();e.returnValue='';}});
const theme=$('theme');
try{const saved=localStorage.getItem('riotIntroTheme');if(saved){document.documentElement.dataset.theme=saved;theme.value=saved;}}catch(_){}
theme.onchange=()=>{document.documentElement.dataset.theme=theme.value;try{localStorage.setItem('riotIntroTheme',theme.value);}catch(_){}};
refresh(true);setInterval(()=>refresh(false),2000);
