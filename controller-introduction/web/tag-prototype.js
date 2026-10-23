'use strict';
const root=document.getElementById('prototypePage');
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const clone=o=>JSON.parse(JSON.stringify(o));
const path=location.pathname, ident=new URLSearchParams(location.search).get('id');
let state=null,draft=null,baseline='',revision=0,busy=false,refreshing=false,preview=null,previewRequest=0,conflict=false;
const $=id=>document.getElementById(id);
const link=(url,label,style='')=>`<a class="${style}" href="${url}">${esc(label)}</a>`;
const checked=v=>v?' checked':'';
const selected=(v,current)=>v===current?' selected':'';
const isDirty=()=>draft!==null&&JSON.stringify(draft)!==baseline;
const statuses={GOOD:'good',BAD:'bad',DISABLED:'warn',UNAVAILABLE:'warn',INCOMPATIBLE:'bad'};
const qual=q=>`<span class="${statuses[q]||'warn'}">${esc(q)}</span>`;
const device=id=>state.devices.find(d=>d.id===id);
const pointOf=b=>device(b?.device_id)?.points.find(p=>p.id===b.point_id);
function usage(t){const u=t.usage;return [u.flow_role&&`Flow 1: ${u.flow_role}`,u.dashboard&&'Dashboard',u.mqtt&&'MQTT',u.rule&&'Rule'].filter(Boolean).join(' · ')||'Unused';}
function alarmSummary(t){return Object.entries(t.alarms.levels).filter(([,v])=>v.enabled).map(([k,v])=>`${k} ${v.threshold}`).join(' · ')||'Off';}
function historySummary(t){return t.history.enabled?`Hourly · ${t.history.sample_sec}s samples`:'Off';}
function live(t){
 const v=t.live,value=v.value===null?'—':typeof v.value==='boolean'?(v.value?'ON':'OFF'):Number(v.value).toFixed(2);
 return `${esc(value)} ${esc(t.units)} ${t.reference!=='none'?esc(t.reference):''} · ${qual(v.quality)}
 <span class="muted">${v.age_sec===null?'No sample':`${esc(v.age_sec)}s old`}${v.quality!=='GOOD'&&v.value!==null?' · last-known':''}</span>`;
}
async function request(url,params){
 const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),5000);
 try{
  const response=await fetch(url,{cache:'no-store',signal:controller.signal,...(params?{method:'POST',body:new URLSearchParams({draft:JSON.stringify(params)})}:{})});
  const result=await response.json();if(!response.ok||result.ok===false)throw Error(result.error||`HTTP ${response.status}`);return result;
 }finally{clearTimeout(timer);}
}
function message(text,bad=false){const el=$('result');if(el){el.textContent=text;el.className=bad?'bad':'good';}}
function saveBar(){return `<div class="saveBar"><button id="save" type="submit" disabled>Save Changes</button><button id="reload" type="button">Reload</button><span id="dirty" class="muted">No unsaved changes</span><span id="result" role="status" aria-live="polite"></span></div>`;}
function updateDirty(){
 if($('save'))$('save').disabled=busy||!isDirty()||conflict||(path==='/prototype/replacement'&&(!preview?.compatible||!draft.confirm));
 if($('dirty'))$('dirty').textContent=conflict?'Changed elsewhere — reload required':isDirty()?'Unsaved changes':'No unsaved changes';
 updateSummaries();
}
function getField(object,key){return key.split('.').reduce((a,k)=>a[k],object);}
function setField(object,key,value){const keys=key.split('.'),last=keys.pop();keys.reduce((a,k)=>a[k],object)[last]=value;}
function input(label,key,type='text',disabled=false){
 return `<label>${esc(label)}<input data-field="${key}" type="${type}" ${type==='number'?'step="any"':''} value="${esc(getField(draft,key))}"${disabled?' disabled':''}></label>`;
}
function check(label,key){return `<label><input type="checkbox" data-field="${key}"${checked(getField(draft,key))}>${esc(label)}</label>`;}
function disclosure(key,title,summary,content,open=false){
 return `<details data-section="${key}"${open?' open':''}><summary>${esc(title)}<span id="summary-${key}" class="muted">${esc(summary)}</span></summary>${content}</details>`;
}
function diagnostics(content){
 return disclosure('advanced','Advanced diagnostics','Prototype identities and simulated save failure',
 `${content}<label class="flagLine"><input id="failSave" type="checkbox">Simulate a rejected save (draft and committed state retained)</label>`);
}
function beginDraft(value){draft=clone(value);baseline=JSON.stringify(draft);revision=state.revision;conflict=false;}
function sourceErrors(tag,binding){
 if(!binding)return [];
 const d=device(binding.device_id),p=pointOf(binding);
 if(!d||d.state==='missing'||!p)return ['Source missing; no slot-based reassignment'];
 const errors=[];
 if(['disabled','incompatible'].includes(d.state))errors.push(`Device ${d.state}`);
 if(!p.selected)errors.push('Select this Device Tag first');
 if(binding.conversion){if(p.quantity!=='current'||tag.quantity!=='pressure'||tag.units!=='kPa')errors.push('Invalid current conversion');}
 else if(p.quantity!==tag.quantity||p.units!==tag.units||p.reference!==tag.reference)errors.push('Quantity, units or pressure reference mismatch');
 return errors;
}
function sourceEditor(){
 const current=draft.binding;
 let options='<option value="">Unbound</option>',seen=false;
 for(const d of state.devices){
  for(const p of d.points){
   const b={device_id:d.id,point_id:p.id,conversion:p.quantity==='current'&&draft.quantity==='pressure'?{kind:'current_range',eu4:0,eu20:1000}:null};
   const active=current?.device_id===d.id&&current.point_id===p.id;
   const errors=sourceErrors(draft,b);
   if(!active&&(d.state==='missing'||!p.selected||['disabled','incompatible'].includes(d.state)||(errors.length&&ident!=='new')))continue;
   if(active)seen=true;
   options+=`<option value="${esc(d.id+'|'+p.id)}"${active?' selected':''}>${esc(d.name+' / '+p.name+(errors.length?' — '+errors.join('; '):d.state!=='online'?' — '+d.state:''))}</option>`;
  }
 }
 if(current&&!seen)options+=`<option value="${esc(current.device_id+'|'+current.point_id)}" selected>Missing stored binding</option>`;
 const conversion=current?.conversion;
 return `<div class="fields"><label>Selected Device Tag<select id="source">${options}</select></label>${input('Name','name')}${input('Quantity','quantity','text',true)}${input('Engineering units','units','text',true)}${input('Pressure reference','reference','text',true)}</div>
 <div id="conversion">${conversion?`<div class="fields">${input('Engineering value at 4 mA','binding.conversion.eu4','number')}${input('Engineering value at 20 mA','binding.conversion.eu20','number')}</div>`:''}</div>
 <div class="preview" id="bindingPreview"></div>
 <p class="note">Choose selected compatible Device Tags. Device selection is saved on the Device page. No auto-enable, substitution or field connection. Commissioning and pressure basis are illustrative, not hardware validation.</p>`;
}
function tagForm(t,isNew=false){
 beginDraft(t);
 if(isNew)baseline='';
 const alarmRows=['HH','H','L','LL'].map(k=>`<b>${k}</b><label><input aria-label="Enable ${k}" data-field="alarms.levels.${k}.enabled" type="checkbox"${checked(draft.alarms.levels[k].enabled)}>On</label><input aria-label="${k} threshold" type="number" step="any" data-field="alarms.levels.${k}.threshold" value="${draft.alarms.levels[k].threshold}">`).join('');
 root.innerHTML=`<div class="toolbar"><h1>${isNew?'Add Tag':esc(t.name)}</h1>${link('/prototype/tags','All Tags','rowEdit')}</div>
 <div class="liveLine" id="live">${isNew?'New process identity will be issued on Save':live(t)}</div>
 <p class="note">Committed live sample; edits below are a draft. Flow, History, Alarm and consumer settings demonstrate relationships only.</p>
 <form class="workspace" id="editor">
 ${disclosure('source','Source',t.live?.source||'Choose a source',sourceEditor(),true)}
 ${disclosure('history','History',historySummary(t),`<div class="checks">${check('Record numeric history','history.enabled')}</div><div class="fields">${input('Sample interval (seconds)','history.sample_sec','number')}</div><p class="note">Hourly min/max/average configuration only. No samples, graph engine or archives are generated. Rebind starts a recorded provenance segment.</p>`)}
 ${disclosure('alarms','Alarms',alarmSummary(t),`<div class="alarmFields">${alarmRows}</div><div class="fields">${input('Deadband','alarms.deadband','number')}${input('Delay (seconds)','alarms.delay_sec','number')}</div><div class="checks">${check('Latch','alarms.latch')}</div><p class="note">Threshold configuration only. No evaluation, ACK lifecycle or notifications run.</p>`)}
 ${disclosure('usage','Usage',usage(t),`<div class="checks">${check('Dashboard','usage.dashboard')}${check('MQTT telemetry','usage.mqtt')}${check('Illustrative rule','usage.rule')}</div><div class="fields"><label>Flow 1 input<select data-field="usage.flow_role">${[['','Unassigned'],['dp','DP'],['static','Static pressure'],['temp','Temperature'],['reverse','Separate reverse DP']].map(([v,l])=>`<option value="${v}"${selected(v,draft.usage.flow_role)}>${l}</option>`).join('')}</select></label></div><p class="note">${esc(state.flow_policy)} Gauge pressure requires an atmospheric reference at commissioning. No automatic signed-DP inference.</p>`)}
 ${diagnostics(`<p class="note">Process ID: ${esc(t.id||'Issued on Save')} · Namespace: ${esc(state.namespace)} · Binding revision: ${t.binding_revision||1} · Configuration revision: ${revision}</p><div class="audit">${audit(t.id)}</div>`)}
 ${saveBar()}</form>`;
 updateDirty();
}
function updateSummaries(){
 if(!draft||!$('summary-history'))return;
 $('summary-history').textContent=historySummary(draft);
 $('summary-alarms').textContent=alarmSummary(draft);
 $('summary-usage').textContent=usage(draft);
 const b=draft.binding,d=device(b?.device_id),p=pointOf(b);
 $('summary-source').textContent=b?`${d?.name||'Missing Device'} / ${p?.name||'Missing point'}`:'Unbound';
 const errors=sourceErrors(draft,b);
 $('bindingPreview').textContent=errors.length?`Binding unavailable/incompatible: ${errors.join('; ')}`:b?'Compatible quantity, units and pressure reference. Binding changes retain process ID and usages.':'Unbound: consumers retain identity but receive UNAVAILABLE.';
 $('bindingPreview').className='preview '+(errors.length?'bad':'good');
}
function audit(tagId){
 const events=state.events.filter(e=>!tagId||e.tag_id===tagId).slice(-20).reverse();
 return events.length?events.map(e=>`<p>${esc(e.time)} · ${esc(e.message)}${e.kind==='binding'?`<br><span class="muted">${esc(e.tag_id)} · revision ${e.binding_revision} · ${esc(e.old?e.old.device_id+'/'+e.old.point_id:'Unbound')} → ${esc(e.new?e.new.device_id+'/'+e.new.point_id:'Unbound')}</span>`:''}</p>`).join(''):'<p class="note">No recorded changes yet.</p>';
}
function tagRows(){
 const focused=document.activeElement;
 const focusHref=$('rows').contains(focused)?focused.getAttribute('href'):null;
 const query=($('search')?.value||'').toLowerCase();
 const rows=state.tags.filter(t=>`${t.name} ${t.live.source}`.toLowerCase().includes(query));
 $('rows').innerHTML=rows.length?`<table><thead><tr><th>Name</th><th>Source</th><th>Value / quality</th><th>History</th><th>Alarms</th><th>Usage</th><th>Action</th></tr></thead><tbody>${rows.map(t=>`<tr>
 <td class="nameCell"><b>${esc(t.name)}</b></td><td data-label="Source">${esc(t.live.source)}</td><td data-label="Value" class="valueCell">${live(t)}</td>
 <td data-label="History">${link('/prototype/tag?id='+t.id+'#history',historySummary(t),'rowEdit')}</td><td data-label="Alarms">${link('/prototype/tag?id='+t.id+'#alarms',alarmSummary(t),'rowEdit')}</td>
 <td data-label="Usage">${link('/prototype/tag?id='+t.id+'#usage',usage(t),'rowEdit')}</td><td class="actionCell">${link('/prototype/tag?id='+t.id,'Edit','rowEdit')}</td></tr>`).join('')}</tbody></table>`:'<p class="empty">No matching process Tags.</p>';
 if(focusHref)$('rows').querySelector(`a[href="${CSS.escape(focusHref)}"]`)?.focus({preventScroll:true});
}
function list(){
 draft=null;root.innerHTML=`<div class="toolbar"><h1>Tags</h1>${link('/prototype/tag?id=new','Add Tag','rowEdit')}<input id="search" aria-label="Search Tags" placeholder="Search name or source"></div><p class="note">${state.tags.length} configured process Tags. Demo count is not proven Nano capacity. Values are static illustrative samples refreshed locally.</p><div class="workspace tableWrap" id="rows"></div><p id="result" role="status"></p>`;
 tagRows();
}
function deviceRows(){
 root.innerHTML=`<div class="toolbar"><h1>Devices</h1>${link('/prototype/device?id=new','Add simulated Device','rowEdit')}</div><p class="note">IMV25, Rosemount and M31 Device-point selection uses isolated P0 state. Burner, totalizer and ABB NGC8206 fixtures are preserved.</p><div class="workspace"><table><thead><tr><th>Device</th><th>Device Type</th><th>Selected / available</th><th>State</th><th>Action</th></tr></thead><tbody>${state.devices.map(d=>`<tr><td class="nameCell"><b>${esc(d.name)}</b></td><td data-label="Type">${esc(state.profiles.find(p=>p.id===d.profile)?.name)}</td><td data-label="Selection">${d.points.filter(p=>p.selected).length} / ${d.points.length}</td><td data-label="State">${esc(d.state.toUpperCase())}</td><td class="actionCell">${link('/prototype/device?id='+d.id+(d.state==='missing'?'&recreate=1':''),d.state==='missing'?'Recreate':'Edit','rowEdit')}</td></tr>`).join('')}</tbody></table></div><p id="result" role="status"></p>`;
}
function missingDevice(d){
 draft=null;
 root.innerHTML=`<div class="toolbar"><h1>${esc(d.name)} — missing</h1>${link('/prototype/devices','All Devices','rowEdit')}</div><div class="workspace"><p class="note">Deletion saved. Existing process bindings remain unresolved; they cannot follow a reused slot. Recreating issues a new Device identity.</p><div class="rowActions">${link('/prototype/device?id='+d.id+'&recreate=1','Recreate simulated Device','rowEdit')}${state.tags.filter(t=>t.binding?.device_id===d.id).map(t=>link('/prototype/tag?id='+t.id,t.name,'rowEdit')).join('')}</div>${disclosure('advanced','Advanced diagnostics','Retained instance identity',`<p class="note">${esc(d.id)} · slot ${esc(d.slot)} · MISSING</p>`)}</div><p id="result" role="status"></p>`;
}
function deviceForm(d){
 const newDevice=!d||d.state==='missing';
 const requestedProfile=new URLSearchParams(location.search).get('profile');
 beginDraft(newDevice?{profile:d?.profile==='p0_imv25'||requestedProfile==='p0_imv25'?'p0_imv25':'p0_rosemount',slot:d?.slot||'LD07',name:d?'Replacement '+d.name:'New simulated Device'}:{device_id:d.id,name:d.name,state:d.state,selected:d.points.filter(p=>p.selected).map(p=>p.id),confirm:''});
 root.innerHTML=`<div class="toolbar"><h1>${newDevice?'Create simulated Device':esc(d.name)}</h1>${link('/prototype/devices','All Devices','rowEdit')}</div><form class="workspace" id="editor">
 ${newDevice?`<p class="note">${d?'The old instance remains missing. A new ID is issued even when reusing its slot; process bindings will not follow it.':'New Device points start unselected.'}</p><div class="fields">${input('Name','name')}${input('Slot label','slot')}<label>Device Type<select data-field="profile"><option value="p0_imv25"${selected('p0_imv25',draft.profile)}>IMV25-M fixture</option><option value="p0_rosemount"${selected('p0_rosemount',draft.profile)}>Rosemount 3095FB fixture</option></select></label></div>`:
 `<div class="fields">${input('Name','name')}<label>Simulated source state<select data-field="state">${['online','disconnected','disabled','missing','incompatible'].map(s=>`<option value="${s}"${selected(s,draft.state)}>${s==='missing'?'Deleted / missing':s[0].toUpperCase()+s.slice(1)}</option>`).join('')}</select></label></div><p class="note">Disconnected retains last-known values. Disabled suppresses use. Missing preserves unresolved bindings. Incompatible simulates units/configuration mismatch.</p><label class="flagLine"><input id="deleteConfirm" type="checkbox">Confirm deletion if Missing is chosen; dependent Tags become unavailable</label>
 <table><thead><tr><th>Device Tag</th><th>Sample</th><th>Selected</th><th>Process usage</th></tr></thead><tbody>${d.points.map(p=>{
 const tags=state.tags.filter(t=>t.binding?.device_id===d.id&&t.binding.point_id===p.id);
 return `<tr><td class="nameCell">${esc(p.name)}</td><td data-label="Sample">${esc(p.value)} ${esc(p.units)}</td><td data-label="Selected"><label><input aria-label="Select ${esc(p.name)}" data-point="${esc(p.id)}" type="checkbox"${checked(p.selected)}${tags.length?' disabled':''}>${tags.length?'Required by process Tags':'Acquire point'}</label></td><td data-label="Usage">${tags.map(t=>link('/prototype/tag?id='+t.id,t.name,'rowEdit')).join(' · ')||(p.selected&&!['disabled','incompatible'].includes(d.state)?link('/prototype/tag?id=new&source='+encodeURIComponent(d.id+'|'+p.id),'Add Tag','rowEdit'):'Select and Save before adding a Tag')}</td></tr>`;
 }).join('')}</tbody></table>`}
 ${diagnostics(`<p class="note">Device ID: ${esc(d?.id||'Issued on Save')} · slot label ${esc(d?.slot||draft.slot)}. Slot is not identity. No register map or hardware validation is claimed.</p>`)}${saveBar()}</form>`;
 if(newDevice)baseline='';
 updateDirty();
}
function library(){
 draft=null;root.innerHTML=`<h1>Device Library</h1><p class="note">Available Device Tags are definitions, not automatically active process Tags. All profiles here are illustrative; no real TOML validation or installation occurs.</p><div class="workspace">${state.profiles.map(p=>disclosure('profile-'+p.id,p.name,`${p.available} available Device Tags`,`<p class="note">${esc(p.validation)}</p><div class="rowActions">${state.devices.filter(d=>d.profile===p.id).map(d=>link('/prototype/device?id='+d.id,d.name+' → Select Device Tags','rowEdit')).join('')}${['p0_imv25','p0_rosemount'].includes(p.id)?link('/prototype/device?id=new&profile='+p.id,'Create simulated Device','rowEdit'):''}</div>`)).join('')}</div><p id="result" role="status"></p>`;
}
async function replacement(){
 const target=state.devices.find(d=>d.profile==='p0_rosemount'&&d.state!=='missing');
 beginDraft({target_id:target?.id||'',confirm:''});
 root.innerHTML=`<h1>Replace IMV25 with Rosemount</h1><form id="editor" class="workspace"><p class="note">One draft maps DP, static pressure and temperature. Process IDs, History, Alarms and all consumer assignments are preserved. No calculation, integration or hardware commissioning runs.</p><div class="fields"><label>Replacement Device<select id="target">${state.devices.filter(d=>d.profile==='p0_rosemount').map(d=>`<option value="${d.id}"${selected(d.id,draft.target_id)}>${esc(d.name)} (${esc(d.state)})</option>`).join('')}</select></label></div><div id="replacementPreview" class="preview" role="status">Loading compatibility...</div><label class="flagLine"><input id="replaceConfirm" type="checkbox">I reviewed compatibility; replace all three bindings</label>${diagnostics(`<div class="audit">${audit()}</div>`)}${saveBar()}</form>`;
 await loadPreview();
}
async function loadPreview(){
 const serial=++previewRequest;
 preview=null;updateDirty();
 try{
  const result=await request('/api/prototype/replacement?target='+encodeURIComponent(draft.target_id));
  if(serial!==previewRequest||busy)return;
  preview=result;
  if(preview.revision!==revision){conflict=true;throw Error('Configuration changed elsewhere; Reload before replacement.');}
  $('replacementPreview').innerHTML=`<table><thead><tr><th>Process Tag</th><th>Old source → new source</th><th>Compatibility</th><th>Preserved usage</th></tr></thead><tbody>${preview.rows.map(r=>`<tr><td class="nameCell">${esc(r.name)}</td><td data-label="Mapping">${esc(device(r.old.device_id)?.name)} / ${esc(r.old.point_id)} → ${esc(device(r.new.device_id)?.name)} / ${esc(r.new.point_id)}</td><td data-label="Compatibility">${r.compatible?'<span class="good">Compatible: units / quantity / pressure basis</span>':`<span class="bad">${esc(r.reasons.join('; '))}</span>`}</td><td data-label="Usage">${esc(usage({usage:r.usage}))}</td></tr>`).join('')}</tbody></table><p class="note">${preview.compatible?'Ready to review. Save records three binding changes and provenance segments.':'Not ready. Select the Rosemount points on its Device page and ensure compatibility.'}</p>`;
 }catch(e){if(serial!==previewRequest||busy)return;$('replacementPreview').textContent=e.message;}
 updateDirty();
}
function newTag(){
 const tag={name:'New Process Tag',quantity:'pressure',units:'kPa',reference:'gauge',binding:null,
 history:{enabled:false,sample_sec:1,aggregation:'hourly_min_max_avg'},
 alarms:{levels:Object.fromEntries(['HH','H','L','LL'].map(k=>[k,{enabled:false,threshold:0}])),deadband:0,delay_sec:0,latch:false},
 usage:{dashboard:false,mqtt:false,rule:false,flow_role:''}};
 const source=new URLSearchParams(location.search).get('source');
 if(source){
  const [d,p]=source.split('|'),q=pointOf({device_id:d,point_id:p});
  if(!q)throw Error('Requested Device point is missing; no identity substitution.');
  tag.binding={device_id:d,point_id:p,conversion:q.quantity==='current'?{kind:'current_range',eu4:0,eu20:1000}:null};
  if(q.quantity!=='current'){tag.quantity=q.quantity;tag.units=q.units;tag.reference=q.reference;}
 }
 return tag;
}
async function render(){
 if(path==='/prototype/tags')list();
 else if(path==='/prototype/devices'){draft=null;deviceRows();}
 else if(path==='/prototype/library')library();
 else if(path==='/prototype/tag'){
  const t=ident==='new'?newTag():state.tags.find(t=>t.id===ident);
  if(!t){root.innerHTML='<p class="empty">Process Tag missing. No identity substitution.</p>';return;}
  tagForm(t,ident==='new');
 } else if(path==='/prototype/device'){
  if(ident!=='new'&&!device(ident)){root.innerHTML='<p class="empty">Device identity missing. No slot-based substitution.</p>';return;}
  const d=device(ident);
  if(d?.state==='missing'&&!new URLSearchParams(location.search).has('recreate'))missingDevice(d);
  else deviceForm(d);
 }
 else if(path==='/prototype/replacement')await replacement();
 if(location.hash){const key=location.hash.slice(1),section=root.querySelector(`[data-section="${CSS.escape(key)}"]`);if(section)section.open=true;}
}
async function refresh(){
 if(refreshing||busy)return;refreshing=true;
 try{
  const latest=await request('/api/prototype/state');
  if(busy||latest.revision<state.revision)return;
  if(draft&&latest.revision!==revision){
   conflict=true;message('Configuration changed elsewhere. Your draft is retained; Reload to resolve.',true);updateDirty();
  }
  state=latest;
  updateSummaries();
  if(!draft&&path==='/prototype/tags')tagRows();
  const t=state.tags.find(t=>t.id===ident);
  if(t&&$('live'))$('live').innerHTML=live(t);
 }catch(e){message('Live refresh failed: '+e.message,true);}
 finally{refreshing=false;}
}
root.addEventListener('input',e=>{
 if(!draft||busy)return;
 const el=e.target,key=el.dataset.field;if(!key)return;
 setField(draft,key,el.type==='checkbox'?el.checked:el.type==='number'?(el.value===''?null:Number(el.value)):el.value);updateDirty();
});
root.addEventListener('change',async e=>{
 if(!draft||busy)return;
 const el=e.target;
 if(el.id==='source'){
  const [d,p]=el.value.split('|');
  const q=pointOf({device_id:d,point_id:p});
  if(ident==='new'&&q?.quantity==='current'){
   draft.quantity='pressure';draft.units='kPa';draft.reference='gauge';
   for(const key of ['quantity','units','reference'])root.querySelector(`[data-field="${key}"]`).value=draft[key];
  }
  if(ident==='new'&&q&&q.quantity!=='current'){
   draft.quantity=q.quantity;draft.units=q.units;draft.reference=q.reference;
   if(q.quantity==='boolean')draft.history.enabled=false;
   for(const key of ['quantity','units','reference'])root.querySelector(`[data-field="${key}"]`).value=draft[key];
   root.querySelector('[data-field="history.enabled"]').checked=draft.history.enabled;
  }
  draft.binding=el.value?{device_id:d,point_id:p,conversion:pointOf({device_id:d,point_id:p})?.quantity==='current'&&draft.quantity==='pressure'?{kind:'current_range',eu4:0,eu20:1000}:null}:null;
  const c=draft.binding?.conversion;
  $('conversion').innerHTML=c?`<div class="fields">${input('Engineering value at 4 mA','binding.conversion.eu4','number')}${input('Engineering value at 20 mA','binding.conversion.eu20','number')}</div>`:'';
 }else if(el.dataset.point){draft.selected=Array.from(root.querySelectorAll('[data-point]:checked')).map(n=>n.dataset.point);}
 else if(el.id==='deleteConfirm')draft.confirm=el.checked?'DELETE':'';
 else if(el.id==='replaceConfirm')draft.confirm=el.checked?'REPLACE':'';
 else if(el.id==='target'){draft.target_id=el.value;draft.confirm='';$('replaceConfirm').checked=false;await loadPreview();}
 updateDirty();
});
root.addEventListener('input',e=>{if(e.target.id==='search')tagRows();});
root.addEventListener('click',async e=>{
 if(e.target.id!=='reload'||busy)return;
 if(isDirty()&&!confirm('Discard unsaved changes and reload committed prototype values?'))return;
 try{state=await request('/api/prototype/state');await render();message('Reloaded committed values.');}catch(error){message(error.message,true);}
});
root.addEventListener('submit',async e=>{
 e.preventDefault();if(busy||!isDirty()||conflict)return;
 busy=true;updateDirty();root.querySelectorAll('input,select').forEach(el=>el.disabled=true);message('Saving isolated prototype state...');
 const payload={revision};
 if(path==='/prototype/tag'){payload.action=ident==='new'?'tag-create':'tag';payload.tag=clone(draft);}
 else if(path==='/prototype/replacement'){payload.action='replace';Object.assign(payload,clone(draft));}
 else if(path==='/prototype/device'){payload.action=draft.device_id?'device':'device-create';Object.assign(payload,clone(draft));}
 payload.simulate_failure=$('failSave')?.checked||false;
 try{
  state=await request('/api/prototype/save',payload);
  if(ident==='new'||(path==='/prototype/device'&&!draft.device_id)){
   baseline=JSON.stringify(draft);
   const id=path==='/prototype/tag'?state.tags.at(-1).id:state.devices.at(-1).id;
   busy=false;
   location.href=path+'?id='+id;return;
  }
  const expanded=Array.from(root.querySelectorAll('details[open]')).map(d=>d.dataset.section);
  if(path==='/prototype/replacement'){
   baseline=JSON.stringify(draft);revision=state.revision;
   $('replacementPreview').innerHTML='<p class="good">Three bindings replaced. Process identities and assignments preserved. Review Tags and the binding audit below.</p>';
   root.querySelector('.audit').innerHTML=audit();preview=null;
  }else await render();
  for(const key of expanded){const section=root.querySelector(`[data-section="${CSS.escape(key)}"]`);if(section)section.open=true;}
  message('Saved. Simulated configuration only; not hardware validation.');
 }catch(error){message('Save failed: '+error.message+' Draft retained.',true);}
 finally{
  busy=false;
  root.querySelectorAll('input,select').forEach(el=>el.disabled=false);
  root.querySelectorAll('[data-point]').forEach(el=>{el.disabled=state.tags.some(t=>t.binding?.device_id===draft.device_id&&t.binding.point_id===el.dataset.point);});
  if(path==='/prototype/tag')root.querySelectorAll('[data-field="quantity"],[data-field="units"],[data-field="reference"]').forEach(el=>el.disabled=true);
  updateDirty();
 }
});
document.addEventListener('click',e=>{
 if(!e.target.closest('a[href]')||(!isDirty()&&!busy))return;
 if(busy||!confirm('Discard unsaved prototype changes and leave this page?')){e.preventDefault();return;}
 baseline=JSON.stringify(draft);
});
window.addEventListener('beforeunload',e=>{if(isDirty()||busy){e.preventDefault();e.returnValue='';}});
const theme=document.getElementById('theme');
theme.value=sessionStorage.getItem('riot-p0-theme')||'dark';
document.documentElement.dataset.theme=theme.value;
theme.addEventListener('change',e=>{document.documentElement.dataset.theme=e.target.value;sessionStorage.setItem('riot-p0-theme',e.target.value);});
document.querySelectorAll('.nav a').forEach(a=>{if(a.getAttribute('href')===path)a.setAttribute('aria-current','page');});
(async()=>{try{state=await request('/api/prototype/state');await render();setInterval(refresh,3000);}catch(error){root.innerHTML=`<p class="bad" role="alert">Cannot load isolated prototype: ${esc(error.message)}</p>`;}})();
