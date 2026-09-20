#pragma once
#include <pgmspace.h>
const char index_html[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MiniBot CAM</title><style>
body{font-family:system-ui,sans-serif;background:#161b22;color:#e6edf3;max-width:1100px;margin:auto;padding:20px}
a{color:#67d5ec}button,input,select{font:inherit;padding:8px;border-radius:5px;margin:4px;background:#263241;color:#fff;border:1px solid #586574}
button{cursor:pointer}button:disabled{opacity:.5;cursor:wait}section,details{background:#202832;padding:16px;border-radius:8px;margin:16px 0}
#preview{display:block;max-width:100%;max-height:65vh;margin:auto}
#camera-fields{min-width:0;margin:12px 0;padding:0;border:0}
#camera-settings{margin-bottom:16px}
.camera-group{padding:12px;border:1px solid #394552;border-radius:6px;margin-top:8px;background:#202832}
.camera-group:nth-child(even){background:#26313d}
.camera-group h3{font-size:.95rem;margin:0 0 10px;color:#67d5ec}
.camera-row{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px 24px}
.camera-cell{min-width:0;display:flex;flex-direction:column;gap:8px}
#camera-settings label{display:grid;grid-template-columns:minmax(0,1fr) 120px;align-items:center;gap:10px;min-height:40px}
#camera-settings input,#camera-settings select{box-sizing:border-box;width:100%;min-width:0;margin:0}
#camera-settings input[type=checkbox]{width:20px;height:20px;justify-self:start}
#camera-settings label.inactive{opacity:.45}
@media(max-width:900px){.camera-row{grid-template-columns:1fr;gap:12px}}
pre{white-space:pre-wrap;overflow:auto;max-height:320px;font-size:13px}#message{min-height:1.5em;color:#67d5ec}summary{cursor:pointer;font-size:1.2em}
#wifi-form{display:grid;gap:12px;max-width:620px}
#serial-form{display:flex;gap:8px}#serial-command{flex:1;min-width:0}
#serial-output{height:260px;background:#111820;padding:12px;overflow-wrap:anywhere}
#wifi-form label{display:grid;grid-template-columns:100px minmax(0,1fr);align-items:center;gap:16px;min-width:0}
#wifi-form input,#wifi-form select{box-sizing:border-box;width:100%;min-width:0;margin:0}
#wifi-form p{margin:0 0 0 116px;line-height:1.5;color:#bac5d1}
#wifi-form button{justify-self:start;margin:0 0 0 116px}
@media(max-width:480px){#wifi-form label{grid-template-columns:1fr;gap:6px}#wifi-form p,#wifi-form button{margin-left:0}}
</style></head><body>
<h1>MiniBot CAM <small id="info"></small></h1>
<p><a href="/api">HTTP API</a> · <a href="/capture" target="_blank">Snapshot</a></p>
<p id="message" role="status"></p>
<section><button id="toggle-stream">Start preview</button><button id="check-ota">Check firmware</button>
<p id="firmware-status" role="status" aria-live="polite"></p>
<img id="preview" alt="Camera preview" hidden></section>
<details open><summary>Camera settings</summary><fieldset id="camera-fields" disabled>
<div id="camera-settings"></div><button id="save-camera">Save camera settings</button></fieldset>
<p>Changes apply immediately. Save to keep them after restart. Lower JPEG quality numbers give better image quality and larger frames.</p></details>
<section><h2>Wi-Fi</h2><p id="wifi-status"></p>
<form id="wifi-form"><label>Mode <select id="wifi-mode"><option value="station">Station + setup AP</option><option value="ap">Access point only</option></select></label>
<label>SSID <input id="wifi-ssid" maxlength="32" autocomplete="off"></label>
<label>Password <input id="wifi-pass" type="password" maxlength="63" autocomplete="new-password"></label>
<button type="submit">Save Wi-Fi settings</button></form></section>
<details id="serial-console"><summary>S3 serial console</summary>
<p>Send commands to the S3 over UART. Try <code>help</code>, <code>wifi ip</code> or <code>batt v</code>. Up/Down recalls the last 10 commands.</p>
<pre id="serial-output" role="log" aria-live="polite"></pre>
<form id="serial-form"><input id="serial-command" aria-label="S3 command" placeholder="help" maxlength="127" autocomplete="off" required>
<button id="serial-send" type="submit">Send</button></form>
<p id="serial-status" role="status"></p>
<small>S3 sends IP reports every five seconds until discovery completes. Incoming UART text appears above; a transmitted command does not confirm a reply. Recent output is shared across browsers.</small>
</details>
<section><h2>Device log</h2><pre id="logs"></pre></section>
<script>
const $=id=>document.getElementById(id);
const message=text=>{$('message').textContent=text;};
const serialHistoryKey='minibot.cam.s3CommandHistory';
let serialHistory=[];
try{const saved=JSON.parse(localStorage.getItem(serialHistoryKey)||'[]');
  if(Array.isArray(saved))serialHistory=saved.filter(cmd=>typeof cmd==='string'&&cmd.trim()&&cmd.length<=127&&!/[^\x20-\x7e]/.test(cmd)).slice(-10);
}catch(e){}
let serialIndex=serialHistory.length,serialDraft='',serialReading=false;
$('serial-command').onkeydown=event=>{
  if(event.isComposing||!['ArrowUp','ArrowDown'].includes(event.key))return;
  event.preventDefault();
  if(!serialHistory.length)return;
  if(event.key==='ArrowUp'){
    if(serialIndex===serialHistory.length)serialDraft=event.target.value;
    serialIndex=Math.max(0,serialIndex-1);
  }else serialIndex=Math.min(serialHistory.length,serialIndex+1);
  event.target.value=serialIndex===serialHistory.length?serialDraft:serialHistory[serialIndex];
  event.target.setSelectionRange(event.target.value.length,event.target.value.length);
};
async function readSerial(){
  if(!$('serial-console').open||serialReading||document.hidden)return;
  serialReading=true;
  try{const state=JSON.parse(await request('/api/console'));
    const output=$('serial-output');
    if(output.textContent!==state.transcript){
      const atBottom=output.scrollHeight-output.scrollTop-output.clientHeight<30;
      output.textContent=state.transcript;
      if(atBottom)output.scrollTop=output.scrollHeight;
    }
    if(!state.ready)$('serial-status').textContent='UART unavailable; see device log.';
  }catch(e){$('serial-status').textContent='Console read failed: '+e.message;}
  finally{serialReading=false;}
}
$('serial-console').ontoggle=()=>{if($('serial-console').open)readSerial();};
setInterval(readSerial,500);
$('serial-form').onsubmit=async event=>{
  event.preventDefault();
  const input=$('serial-command'),send=$('serial-send'),command=input.value;
  if(input.disabled)return;
  if(!command.trim()||command.length>127||/[^\x20-\x7e]/.test(command)){
    $('serial-status').textContent='Enter one printable ASCII command, maximum 127 characters.';return;
  }
  input.disabled=send.disabled=true;
  try{
    $('serial-status').textContent=await request('/api/console',{method:'POST',headers:{'Content-Type':'text/plain'},body:command});
    serialHistory.push(command);serialHistory=serialHistory.slice(-10);serialIndex=serialHistory.length;serialDraft='';
    try{localStorage.setItem(serialHistoryKey,JSON.stringify(serialHistory));}catch(e){}
    input.value='';await readSerial();
  }catch(e){$('serial-status').textContent='Send failed: '+e.message+' The command may have been sent; check output before retrying.';}
  finally{input.disabled=send.disabled=false;input.focus();}
};
async function request(path,options={}) {
  const response=await fetch(path,{...options,signal:AbortSignal.timeout(15000)});
  const body=await response.text();
  if(!response.ok)throw new Error(body||String(response.status));
  return body;
}
const fields=[
 ['framesize','Resolution',[[0,'96×96'],[1,'160×120'],[2,'128×128'],[3,'176×144'],[4,'240×176'],[5,'240×240'],[6,'320×240'],[7,'320×320'],[8,'400×296'],[9,'480×320'],[10,'640×480'],[11,'800×600'],[12,'1024×768'],[13,'1280×720'],[14,'1280×1024'],[15,'1600×1200']]],
 ['quality','JPEG quality',4,63],['brightness','Brightness',-2,2],['contrast','Contrast',-2,2],['saturation','Saturation',-2,2],
 ['special_effect','Effect',[[0,'None'],[1,'Negative'],[2,'Grayscale'],[3,'Red'],[4,'Green'],[5,'Blue'],[6,'Sepia']]],
 ['awb','Auto white balance'],['awb_gain','White balance gain'],['wb_mode','White balance mode',[[0,'Auto'],[1,'Sunny'],[2,'Cloudy'],[3,'Office'],[4,'Home']]],
 ['aec','Auto exposure'],['aec2','AEC DSP'],['ae_level','Exposure level',-2,2],['aec_value','Manual exposure',0,1200],
 ['agc','Auto gain'],['agc_gain','Manual gain',0,30],['gainceiling','Gain ceiling',[[0,'2×'],[1,'4×'],[2,'8×'],[3,'16×'],[4,'32×'],[5,'64×'],[6,'128×']]],
 ['hmirror','Mirror'],['vflip','Vertical flip'],['dcw','Downsize'],['bpc','Black pixel correction'],['wpc','White pixel correction'],
 ['raw_gma','Raw gamma'],['lenc','Lens correction'],['colorbar','Color bar']
];
const groups=[
 ['Image format',[['framesize'],['quality'],['special_effect']]],
 ['Image adjustments',[['brightness'],['contrast'],['saturation']]],
 ['Exposure',[['aec','aec2'],['ae_level'],['aec_value']]],
 ['Gain',[['agc'],['gainceiling'],['agc_gain']]],
 ['White balance',[['awb','awb_gain'],['wb_mode'],[]]],
 ['Orientation & sizing',[['hmirror'],['vflip'],['dcw']]],
 ['Corrections',[['bpc'],['wpc'],['lenc']]],
 ['Other options',[['raw_gma'],['colorbar'],[]]]
];
const cells={},labels={},pendingSettings=new Set();
for(const [title,columns] of groups){
  const group=document.createElement('div');group.className='camera-group';
  const heading=document.createElement('h3');heading.textContent=title;group.append(heading);
  const row=document.createElement('div');row.className='camera-row';
  for(const ids of columns){const cell=document.createElement('div');cell.className='camera-cell';row.append(cell);for(const id of ids)cells[id]=cell;}
  group.append(row);$('camera-settings').append(group);
}
function syncAvailability(){
  const inactive={aec_value:$('aec').checked,ae_level:!$('aec').checked,
    agc_gain:$('agc').checked,gainceiling:!$('agc').checked,wb_mode:!$('awb_gain').checked};
  for(const [id] of fields){
    $(id).disabled=!!inactive[id]||pendingSettings.has(id);
    labels[id].className=inactive[id]?'inactive':'';
  }
}
for(const [id,label,minimum,maximum] of fields) {
  const row=document.createElement('label');row.append(document.createTextNode(label));
  const input=document.createElement(Array.isArray(minimum)?'select':'input');input.id=id;
  if(Array.isArray(minimum))for(const [value,text] of minimum){const o=document.createElement('option');o.value=value;o.textContent=text;input.append(o);}
  else if(minimum===undefined)input.type='checkbox';
  else {input.type='number';input.min=minimum;input.max=maximum;input.step=1;}
  input.onchange=async()=>{
    if(!input.checkValidity()){input.reportValidity();return;}
    const value=input.type==='checkbox'?Number(input.checked):input.value;
    pendingSettings.add(id);
    syncAvailability();
    try{await request('/config?'+new URLSearchParams({var:id,val:value}));message(label+' updated. Save to keep after restart.');}
    catch(e){message(e.message);await loadCamera();}
    finally{pendingSettings.delete(id);syncAvailability();}
  };
  labels[id]=row;row.append(input);cells[id].append(row);
}
async function loadCamera(){
  try{const state=JSON.parse(await request('/status'));
    $('camera-fields').disabled=!state.initialized;
    if(!state.initialized){message('Camera unavailable. See device log.');return;}
    for(const [id] of fields){const input=$(id);if(input.type==='checkbox')input.checked=!!state[id];else input.value=state[id];}
    syncAvailability();
  }catch(e){message(e.message);}
}
let streaming=false;
function stopPreview(){streaming=false;$('preview').removeAttribute('src');$('preview').hidden=true;$('toggle-stream').textContent='Start preview';}
$('toggle-stream').onclick=()=>{
  if(streaming){stopPreview();return;}
  const url=new URL(location.origin);url.port='81';url.pathname='/stream';url.search='t='+Date.now();
  $('preview').src=url.href;$('preview').hidden=false;streaming=true;$('toggle-stream').textContent='Stop preview';
};
$('preview').onerror=()=>{stopPreview();message('Preview stopped. Retry after the camera or firmware update is ready.');};
$('save-camera').onclick=async()=>{try{message(await request('/api/camera/save',{method:'POST'}));}catch(e){message(e.message);}};
const firmware={phase:'idle',found:0,version:null,started:null,offline:false,installing:false,posting:false,nextCheck:0,probing:false,reloading:false};
function renderFirmwareStatus(){
  const labels={idle:'',waiting:'Waiting for network time…',checking:'Checking firmware…',installing:'Installing firmware; the CAM will restart…',up_to_date:'Firmware is up to date.',invalid:'The discovered version is marked invalid; update skipped.',retry:'Update attempt failed; automatic retry in about a minute. See device log.',failed:'Firmware update failed. See device log.',time_unavailable:'Network time unavailable; firmware check stopped.'};
  const elapsed=firmware.started===null?'':' · '+Math.floor((Date.now()-firmware.started)/1000)+'s elapsed';
  const found=firmware.found?'Found V'+firmware.found+' · ':'';
  if(firmware.offline){
    const retry=firmware.probing?'Checking connection…':'Retrying in '+Math.max(0,Math.ceil((firmware.nextCheck-Date.now())/1000))+'s…';
    $('firmware-status').textContent=found+'CAM offline; waiting for it to return'+elapsed+'. '+retry;
  }else $('firmware-status').textContent=found+(labels[firmware.phase]||'Checking firmware…')+(['waiting','checking','installing','retry'].includes(firmware.phase)?elapsed:'');
}
async function checkFirmwareStatus(){
  if(firmware.reloading)return;
  firmware.probing=true;
  try{
    // Independent of logs/camera requests, with a short timeout during reboot.
    const response=await fetch('/api/info',{cache:'no-store',signal:AbortSignal.timeout(2000)});
    if(!response.ok)throw new Error('HTTP '+response.status);
    const info=await response.json();
    if(!Number.isInteger(info.version))throw new Error('Invalid firmware status');
    if((firmware.version!==null&&info.version!==firmware.version)||
       ((firmware.offline||firmware.installing)&&!info.ota_busy&&info.ota_status!=='installing')){
      firmware.reloading=true;
      $('firmware-status').textContent='CAM is back on V'+info.version+'. Reloading…';
      location.reload();return;
    }
    firmware.version=info.version;
    firmware.offline=false;
    firmware.found=info.ota_found_version||firmware.found;
    firmware.phase=info.ota_status||'idle';
    if(info.ota_pending&&['idle','up_to_date','failed','invalid','time_unavailable'].includes(firmware.phase))firmware.phase='waiting';
    if(firmware.phase==='installing'){firmware.installing=true;stopPreview();}
    const active=!!(info.ota_busy||info.ota_pending);
    if(active&&firmware.started===null)firmware.started=Date.now();
    $('check-ota').disabled=active||firmware.posting;
    $('info').textContent='V'+info.version+(info.invalid_version?' — invalid target V'+info.invalid_version:'');
  }catch(e){
    firmware.offline=true;
    if(firmware.started===null)firmware.started=Date.now();
    $('check-ota').disabled=true;
  }finally{
    firmware.probing=false;
    firmware.nextCheck=Date.now()+(firmware.offline?2000:1000);
    if(!firmware.reloading){renderFirmwareStatus();setTimeout(checkFirmwareStatus,firmware.offline?2000:1000);}
  }
}
setInterval(()=>{if(!firmware.reloading)renderFirmwareStatus();},1000);
$('check-ota').onclick=async()=>{
  stopPreview();firmware.started=Date.now();firmware.found=0;firmware.phase='waiting';firmware.posting=true;
  $('check-ota').disabled=true;renderFirmwareStatus();
  try{message(await request('/api/ota',{method:'POST'}));}
  catch(e){message('Firmware check request: '+e.message);}
  finally{firmware.posting=false;}
};
$('wifi-form').onsubmit=async event=>{
  event.preventDefault();
  try{message(await request('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:$('wifi-mode').value,ssid:$('wifi-ssid').value,password:$('wifi-pass').value})}));$('wifi-pass').value='';}
  catch(e){message(e.message);}
};
function showWifi(state){$('wifi-status').textContent=(state.connected?'Connected at '+state.ip:'Setup AP '+state.ap_ssid+' at '+state.ap_ip)+' · S3 Wi-Fi IP: '+(state.s3_ip||'Waiting for UART response…');}
async function loadWifi(){try{const state=JSON.parse(await request('/api/wifi'));
  $('wifi-mode').value=state.mode;$('wifi-ssid').value=state.ssid;
  showWifi(state);
}catch(e){message(e.message);}}
let offline=false;
async function poll(){
  if(firmware.offline||firmware.phase==='installing'){setTimeout(poll,2000);return;}
  try{const text=await request('/api/logs');if($('logs').textContent!==text){$('logs').textContent=text;$('logs').scrollTop=$('logs').scrollHeight;}
    showWifi(JSON.parse(await request('/api/wifi')));
    if(offline){loadCamera();loadWifi();}offline=false;
  }catch(e){offline=true;}
  setTimeout(poll,2000);
}
checkFirmwareStatus();loadCamera();loadWifi();poll();
</script></body></html>)HTML";
