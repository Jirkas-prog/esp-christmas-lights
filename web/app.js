'use strict';
const $ = s => document.querySelector(s);
let state = null, busy = false, editingBrightness = false, uploading = false, pendingRestart = false;
const modes = {all:'All (Celý řetěz)',first:'First Half (První polovina)',second:'Second Half (Druhá polovina)',alternate:'Alternate (Střídání)',crossfade:'Linear Crossfade (Lineární prolínání)',soft_crossfade:'Soft Crossfade (Jemné prolínání)',breathe:'Breathe (Dýchání)',tide:'Tide (Přelévání)',candlelight:'Candlelight (Svíčka)'};
const modeDescriptions = {all:'Obě poloviny trvale svítí',first:'Svítí pouze první skupina LED',second:'Svítí pouze druhá skupina LED',alternate:'Pravidelné přepínání skupin',crossfade:'Rovnoměrný přechod z jedné skupiny do druhé',soft_crossfade:'Měkké rozjezdy a dojezdy, stálý součet světla',breathe:'Obě skupiny společně dýchají mezi 20 a 100 % jasu',tide:'Pomalé přelévání, druhá skupina zůstává alespoň na 25 %',candlelight:'Dvě jemně odlišné, pomalé vlny jako světlo svíčky'};
const gentleFactors={soft_crossfade:4,breathe:6,tide:8,candlelight:12};
const modeOptions=Object.entries(modes).map(([v,t])=>`<option value="${v}">${t}</option>`).join('');
$('.mode-list').innerHTML=Object.entries(modes).map(([key,label],i)=>`<button data-mode="${key}" aria-pressed="${i===0}" disabled><span class="mode-icon">${['● ●','● ○','○ ●','↔','◐ ◑','∿','◉','≈','✧'][i]}</span><span><strong>${label}</strong><small>${modeDescriptions[key]}</small></span></button>`).join('');
for(const id of ['preset-effect','night-effect']) $('#'+id).innerHTML=modeOptions;
let editingInterval = false, polling = false;
let editingProtection = false, editingLimits = false, editingPreset = false, editingNight = false, editingWindowPresets = false;
function notice(text, error = false) { $('#notice').textContent = text; $('#notice').hidden = !text; $('#notice').classList.toggle('error', error); }
function navigate() {
  const page = ['light','schedule','wifi','mqtt','system'].includes(location.hash.slice(1)) ? location.hash.slice(1) : 'light';
  document.querySelectorAll('[data-page]').forEach(el => { el.hidden = el.dataset.page !== page; });
  document.querySelectorAll('nav a').forEach(el => { if (el.hash === '#'+page) el.setAttribute('aria-current','page'); else el.removeAttribute('aria-current'); });
}
async function api(path, body) {
  const response = await fetch(path, body === undefined ? {cache:'no-store',signal:AbortSignal.timeout(12000)} : {method:'POST',signal:AbortSignal.timeout(12000),headers:{'Content-Type':'application/json','X-Requested-With':'Svetylka'},body:JSON.stringify(body)});
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'Zařízení neodpovídá.');
  return result;
}
function controlsDisabled(disabled) { document.querySelectorAll('#power,#brightness,#dimmer,#brighter,[data-mode],#interval,#interval-save').forEach(el => { el.disabled = disabled; }); }
function render(next) {
  state = next;
  $('#device-name').textContent = next.alias || next.hostname;
  document.title = (next.alias || next.hostname) + ' · Světýlka';
  $('#connection').textContent = next.version.includes('náhled') ? 'Náhled · bez zařízení' : next.updating ? 'Aktualizace…' : 'Zařízení online'; $('#connection').classList.remove('offline');
  $('#power').textContent = next.state === 'ON' ? 'Vypnout' : 'Zapnout'; $('#power').setAttribute('aria-pressed', String(next.state === 'ON'));
  $('#light-state').textContent = next.protection?.calibrating ? 'Kalibrace' : next.protection?.blocked ? 'Blokováno ochranou' : next.state === 'ON' && next.brightness ? modes[next.effect] : 'Zhasnuto';
  if (!editingBrightness) { $('#brightness').value = next.brightness; $('#brightness-value').textContent = next.brightness+' %'; }
  $('#brighter').textContent=next.brightness<=1 ? '+ 1 % (zapnout 2 %)' : '+ 10 %';
  if (!editingPreset && next.on_preset) {$('#preset-brightness').value=next.on_preset.brightness;$('#preset-effect').value=next.on_preset.effect;}
  for (const el of $('#preset-form').elements) el.disabled=busy || next.updating || pendingRestart || !next.on_preset;
  document.querySelectorAll('[data-mode]').forEach(el => el.setAttribute('aria-pressed', String(el.dataset.mode === next.effect)));
  $('#interval-form').hidden = ['all','first','second'].includes(next.effect);
  $('#interval-title').textContent = next.effect === 'crossfade' ? 'Doba jednoho prolnutí' : 'Interval střídání';
  $('#interval-hint').textContent = next.effect === 'crossfade' ? 'Doba přechodu z jedné skupiny na druhou. Zpáteční přechod trvá stejně dlouho. Rozsah 0,1–10 s.' : 'Doba svícení jedné skupiny před přepnutím. Rozsah 0,1–10 s.';
  if (gentleFactors[next.effect]) {
    $('#interval-title').textContent='Tempo efektu';
    $('#interval-hint').textContent='Pomalý cyklus trvá '+(next.effect_cycle_ms/1000).toLocaleString('cs-CZ')+' s ('+gentleFactors[next.effect]+' × tempo). Menší tempo zrychluje, větší zpomaluje; pro jemný pohyb se používá nejméně 0,5 s. Rozsah nastavení 0,1–10 s.';
  }
  if (!editingInterval) $('#interval').value = next.interval_ms / 1000;
  const preview = $('.string-preview'); preview.classList.toggle('on',next.output_active);
  preview.classList.toggle('alternate',next.effect === 'alternate'); preview.style.setProperty('--blink',(next.effect_cycle_ms || next.interval_ms*2)+'ms');
  preview.classList.toggle('crossfade',next.effect === 'crossfade');
  for(const key of Object.keys(gentleFactors)) preview.classList.toggle(key,next.effect===key);
  [...preview.children].forEach((el,i) => el.classList.toggle('lit',next.effect === 'all' || (next.effect === 'first' && i%2 === 0) || (next.effect === 'second' && i%2 === 1)));
  $('#mqtt-status').textContent = next.mqtt ? 'MQTT připojeno' : 'MQTT nepřipojeno · kód '+next.mqtt_error;
  $('#topic').textContent = next.topic+'/set'; $('#version').textContent = next.version;
  $('#ip').textContent = next.wifi ? next.ip : '192.168.4.1 (nastavení)'; $('#hostname').textContent = next.hostname+'.local'; $('#dhcp-hostname').textContent=next.dhcp_hostname || '—';
  $('#storage').textContent = next.storage_error ? 'Chyba zápisu' : 'V pořádku';
  const h=next.health;
  $('#health-uptime').textContent=Math.floor(next.uptime_ms/86400000)+' d '+Math.floor(next.uptime_ms/3600000)%24+' h '+Math.floor(next.uptime_ms/60000)%60+' min';
  $('#health-reset').textContent=h ? ({power_on:'Zapnutí napájení',software:'Softwarový restart',task_watchdog:'Watchdog úloh',interrupt_watchdog:'Watchdog přerušení',watchdog:'Watchdog',panic:'Chyba programu',brownout:'Pokles napájení'}[h.reset_reason] || h.reset_reason)+' · start č. '+h.boot_count : '—';
  $('#health-recovery').textContent=h ? ({none:'Žádný',control_stall:'Zotavení řídicí úlohy',http_stall:'Zotavení webového serveru'}[h.last_recovery] || h.last_recovery) : '—';
  $('#health-wifi').textContent=next.wifi_rssi_dbm+' dBm'+(h?' · výpadky '+h.wifi_disconnects+' · poslední kód '+h.wifi_disconnect_reason:'');
  $('#health-retries').textContent=h ? h.wifi_retries+' / '+h.wifi_station_resets+' / '+h.wifi_radio_resets : '—';
  $('#health-memory').textContent=h ? Math.round(next.heap_free_bytes/1024)+' / '+Math.round(h.heap_min_bytes/1024)+' kB' : '—';
  controlsDisabled(busy || next.updating || pendingRestart);
  renderProtection(next.protection);
  renderSchedule(next.schedule);
}
function protectionLabels() {
  $('#protection-delay-value').textContent = Number($('#protection-delay').value).toLocaleString('cs-CZ')+' s';
  $('#protection-threshold-value').textContent = $('#protection-threshold').value+' %';
}
function renderProtection(p) {
  if (!p) return;
  const status = p.fault ? (p.latched ? 'Zablokováno ochranou: ' : 'Vypnuto ochranou: ')+p.reason+(p.latched ? '' : ' · Lze znovu zapnout.') : p.calibrating ? 'Probíhá automatická kalibrace' : !p.enabled ? 'Ochrana je vypnutá' : !p.calibrated ? 'Výstup čeká na kalibraci' : p.pending_ms ? 'Podezření na závadu: '+(p.pending_ms/1000).toFixed(1)+' s' : p.monitoring_required === false && state.output_active ? 'Nízký jas: napěťová ochrana se nevyhodnocuje' : 'Ochrana je zapnutá';
  $('#protection-status').textContent = status;
  $('#protection-status').classList.toggle('fault',!!p.fault);
  if (p.fault || (p.enabled && !p.calibrated)) $('#protection-panel').open = true;
  if (!editingProtection) {
    $('#protection-enabled').checked = p.enabled;
    $('#protection-action').value = p.latch_fault ? 'latch' : 'off';
    $('#protection-delay').value = p.delay_ms/1000;
    $('#protection-threshold').value = p.threshold_percent;
    protectionLabels();
  }
  $('#protection-voltage').textContent = p.calibrating ? 'Měření kalibrace · šum '+Math.round(p.adc_noise_mv || 0)+' mV' : p.samples ? 'A: '+Math.round(p.gain_a*100)+' % · B: '+Math.round(p.gain_b*100)+' % kalibrace' : state.output_active && p.monitoring_required === false ? 'Pod nastavenou hranicí' : 'Čekám na svícení';
  $('#protection-limit').textContent = p.calibrated ? p.threshold_percent+'–'+(200-p.threshold_percent)+' % kalibrovaného signálu' : 'Po kalibraci';
  $('#protection-calibrated').textContent = p.calibrated ? 'Uložená v zařízení' : 'Dosud neprovedená';
  $('#calibration-message').textContent = p.message;
  $('#calibration-progress').hidden = !p.calibrating; $('#calibration-progress').value = p.progress;
  $('#cancel-calibration').hidden = !p.calibrating;
  const locked = busy || uploading || pendingRestart || state.updating;
  $('#calibrate').disabled = locked || p.calibrating;
  $('#cancel-calibration').disabled = locked;
  $('#clear-fault').disabled = locked || p.calibrating || !p.fault;
  $('#clear-fault').hidden = !p.latch_fault;
  for (const el of $('#protection-form').elements) el.disabled = locked || p.calibrating;
  for (const el of $('#limits-form').elements) el.disabled = locked || p.calibrating || !p.limits;
  if (p.limits) {
    if (!editingLimits) for (const key of ['first','second','all']) $('#limit-'+key).value = p.limits[key].ignored;
    $('#limits-quality').textContent = ['first','second','all'].map(key => modes[key]+': automatika '+p.limits[key].automatic+' %, šum '+p.limits[key].noise_mv+' mV, chyba průměrovaného průběhu '+p.limits[key].fit_error_mv+' mV').join(' · ');
    limitsLabels();
    thermalWarning($('#thermal-warning'),p.ignored_short_power_mw,['first','second','all'].some(key => p.limits[key].ignored===100));
  }
  if (p.calibrating) controlsDisabled(true);
}
function shortPower(percent, both) { return Math.ceil(5250*5250*(both?10:5)*percent/(5320*1024)); }
function thermalWarning(el, milliwatts, disabled) {
  el.hidden = milliwatts <= 2000;
  el.textContent = '';
  if (!el.hidden) {
    const heading = document.createElement('strong'); heading.textContent = 'POZOR: překročena tepelná mez 2 W'; el.append(heading);
    el.append('Při zkratu může odpor topit až '+(milliwatts/1000).toFixed(2)+' W, aniž ochrana vypne. V zalitém pouzdře hrozí přehřátí. '+(disabled ? 'Hranice 100 % vypíná napěťovou ochranu daného režimu v celém rozsahu. ' : '')+'Sniž hranici, pokud tento výkon není přijatelný.');
  }
}
function limitsLabels() {
  const powers=['first','second','all'].map(key => {
    const value=Number($('#limit-'+key).value); $('#limit-'+key+'-value').textContent=value+' %';
    return shortPower(value,key==='all');
  });
  $('#limits-power').textContent='Zkratový výkon na hranici: první '+(powers[0]/1000).toFixed(2)+' W · druhá '+(powers[1]/1000).toFixed(2)+' W · celý řetěz '+(powers[2]/1000).toFixed(2)+' W. Do 2 W vychází nejvýše 79 / 79 / 39 %.';
  thermalWarning($('#limits-warning'),Math.max(...powers),['first','second','all'].some(key => Number($('#limit-'+key).value)===100));
}
$('#limits-form').oninput=()=>{editingLimits=true;limitsLabels();};
async function saveLimits(automatic) {
  if (busy) return;
  busy=true;
  try {
    const body=automatic ? {automatic:true} : Object.fromEntries(['first','second','all'].map(key=>[key,Number($('#limit-'+key).value)]));
    await api('/api/protection/limits',body); editingLimits=false; notice('Hranice ochrany uloženy.');
  } catch(error) { notice(error.message,true); }
  finally { busy=false; await poll(); }
}
$('#limits-form').onsubmit=event=>{event.preventDefault();saveLimits(false);};
$('#limits-auto').onclick=()=>saveLimits(true);
$('#protection-form').oninput = () => { editingProtection = true; protectionLabels(); };
$('#protection-form').onsubmit = async event => {
  event.preventDefault();
  if (busy) return;
  busy = true;
  try {
    await api('/api/protection',{enabled:$('#protection-enabled').checked,latch_fault:$('#protection-action').value === 'latch',delay_ms:Math.round(Number($('#protection-delay').value)*1000),threshold_percent:Number($('#protection-threshold').value)});
    editingProtection = false; notice('Nastavení ochrany uloženo.');
  } catch (error) { notice(error.message,true); }
  finally { busy = false; await poll(); }
};
async function protectionAction(action) {
  if (busy) return;
  busy = true;
  try { await api('/api/protection/action',{action}); notice(''); }
  catch (error) { notice(error.message,true); }
  finally { busy = false; await poll(); }
}
$('#calibrate').onclick = () => protectionAction('calibrate');
$('#cancel-calibration').onclick = () => protectionAction('cancel');
$('#clear-fault').onclick = () => protectionAction('clear');
async function poll() {
  if (busy || uploading || editingBrightness || pendingRestart || polling) return;
  polling=true;
  try { const next = await api('/api/state'); if (!busy && !editingBrightness) render(next); }
  catch (error) { $('#connection').textContent = 'Zařízení není dostupné'; $('#connection').classList.add('offline'); controlsDisabled(true); }
  finally {polling=false;}
}
async function command(body) {
  if (busy || !state) return;
  busy = true; controlsDisabled(true);
  try { render(await api('/api/light',body)); notice(''); }
  catch (error) { notice(error.message,true); }
  finally { busy = false; if (state) render(state); }
}
$('#power').onclick = () => command({state:state.state === 'ON' ? 'OFF' : 'ON'});
document.querySelectorAll('[data-mode]').forEach(el => { el.onclick = () => { editingInterval = false; command({effect:el.dataset.mode}); }; });
$('#brightness').oninput = event => { editingBrightness = true; $('#brightness-value').textContent = event.target.value+' %'; };
$('#brightness').onchange = event => { editingBrightness = false; command({brightness:Number(event.target.value)}); };
$('#brightness').onblur = () => { editingBrightness = false; };
$('#dimmer').onclick = () => command({brightness:Math.max(state.schedule?.one_percent_off?1:0,state.brightness-10)});
$('#brighter').onclick = () => command({brightness:state.brightness<=1?2:Math.min(100,state.brightness+10)});
$('#preset-form').oninput=()=>{editingPreset=true;};
$('#preset-form').onsubmit=async event=>{
  event.preventDefault();if(busy)return;busy=true;
  try {await api('/api/preset',{brightness:Number($('#preset-brightness').value),effect:$('#preset-effect').value});editingPreset=false;notice('Předvolba při zapnutí uložena.');}
  catch(e){notice(e.message,true);}finally{busy=false;await poll();}
};
$('#interval').oninput = () => { editingInterval = true; };
$('#interval-form').onsubmit = async event => {
  event.preventDefault();
  if (!$('#interval').reportValidity()) return;
  const interval = Math.round($('#interval').valueAsNumber * 1000);
  if (!Number.isFinite(interval) || interval < 100 || interval > 10000) { notice('Interval musí být 0,1–10 sekund.',true); return; }
  editingInterval = false; await command({interval_ms:interval});
};
function restarting(message) {
  pendingRestart = true; controlsDisabled(true); notice(message);
  $('#connection').textContent = 'Restart…';
  setTimeout(() => { pendingRestart = false; poll(); },10000);
}
async function loadSettings() {
  try {
    const config = await api('/api/settings');
    for (const form of [$('#wifi-form'),$('#mqtt-form'),$('#alias-form')]) for (const el of form.elements) {
      if (!(el.name in config)) continue;
      if (el.type === 'checkbox') el.checked = config[el.name]; else el.value = config[el.name];
    }
  } catch (error) { notice('Nastavení se nepodařilo načíst. Obnov stránku po připojení k zařízení.',true); }
}
for (const id of ['wifi-form','mqtt-form','alias-form']) $('#'+id).onsubmit = async event => {
  event.preventDefault(); const form = event.currentTarget; const button = form.querySelector('button'); button.disabled = true;
  const body = {};
  for (const el of form.elements) if (el.name) body[el.name] = el.type === 'checkbox' ? el.checked : el.type === 'number' ? Number(el.value) : el.value;
  try {
    await api(id === 'alias-form' ? '/api/alias' : '/api/settings',body);
    form.querySelectorAll('input[type=password]').forEach(el => { el.value = ''; });
    restarting(id === 'alias-form' ? 'Název uložen. Po restartu: http://'+body.alias+'.local/. Záložní Wi-Fi i MQTT použijí nový název.' : 'Nastavení uloženo. Zařízení se restartuje; při změně sítě se připoj k nové síti a otevři jeho novou adresu.');
    if (id === 'alias-form' && location.hostname.endsWith('.local'))
      setTimeout(() => { location.href = 'http://'+body.alias+'.local/#system'; },10000);
  } catch (error) { notice(error.message,true); }
  finally { button.disabled = false; }
};
$('#restart').onclick = async () => {
  try { await api('/api/restart',{}); restarting('Zařízení se restartuje.'); } catch (error) { notice(error.message,true); }
};
$('#ota-form').onsubmit = event => {
  event.preventDefault(); const file = $('#firmware').files[0];
  if (!file || !file.name.endsWith('.bin') || file.size < 1024 || file.size > 0x1e0000) { notice('Vyber platný firmware.bin pro tuto desku.',true); return; }
  uploading = true; const button = $('#ota-form button'); button.disabled = true; controlsDisabled(true);
  const progress = $('#upload-progress'); progress.hidden = false; progress.value = 0; $('#upload-status').textContent = 'Nahrávám…';
  const xhr = new XMLHttpRequest(); xhr.open('POST','/api/update'); xhr.timeout = 180000;
  xhr.setRequestHeader('Content-Type','application/octet-stream'); xhr.setRequestHeader('X-Requested-With','Svetylka');
  xhr.upload.onprogress = e => { if (e.lengthComputable) progress.value = e.loaded/e.total*100; };
  const finish = error => { uploading = false; button.disabled = false; if (error) { notice(error,true); $('#upload-status').textContent = 'Přenos selhal; ověř stav zařízení.'; poll(); } };
  xhr.onload = () => {
    if (xhr.status !== 200) { let message = 'Aktualizace nebyla přijata.'; try { message = JSON.parse(xhr.responseText).error || message; } catch {} finish(message); return; }
    finish(); $('#upload-status').textContent = 'Firmware ověřen. Restartuji…'; restarting('Aktualizace dokončena. Zařízení se restartuje.');
  };
  xhr.onerror = () => finish('Spojení se přerušilo. Ověř stav zařízení před dalším pokusem.');
  xhr.ontimeout = () => finish('Vypršel čas přenosu. Ověř stav zařízení.');
  xhr.send(file);
};
let scheduleLoaded=false;
for (const [key,label] of [['evening','Večerní'],['morning','Ranní']]) {
  const fieldset=document.createElement('fieldset');fieldset.className='schedule-window';
  fieldset.innerHTML=`<legend>${label} svícení</legend><div class="form-grid"><label>${label} jas (%) · 0 = vypnuto<input id="${key}-brightness" type="number" min="0" max="100" step="1" value="100" required></label><label>${label} režim<select id="${key}-effect">${modeOptions}</select></label></div>`;
  $('#window-preset-fields').append(fieldset);
}
for (let i=0;i<2;i++) {
  const windowEl=document.createElement('fieldset'); windowEl.className='schedule-window';
  windowEl.innerHTML=`<legend>${i===0?'Večerní interval':'Ranní interval'}</legend><label class="check"><input id="window-${i}-enabled" type="checkbox">Používat tento interval</label><label class="check"><input id="window-${i}-overnight" type="checkbox">Přes půlnoc, pokud konec předchází začátku</label><div class="schedule-points"></div>`;
  for (const [key,title] of [['start','Zapnout'],['end','Vypnout']]) {
    const prefix=`window-${i}-${key}`, point=document.createElement('div');point.className='schedule-point';
    point.innerHTML=`<h3>${title}</h3><label>Podle čeho<select id="${prefix}-kind"><option value="fixed">Pevný čas</option><option value="sunrise">Východ slunce</option><option value="sunset">Západ slunce</option></select></label><label id="${prefix}-time-label">Čas<input id="${prefix}-time" type="time" required value="00:00"></label><label id="${prefix}-offset-label">Posun v minutách (− před / + po)<input id="${prefix}-offset" type="number" min="-720" max="720" step="1" value="0" required></label>`;
    windowEl.querySelector('.schedule-points').append(point);
  }
  $('#schedule-windows').append(windowEl);
}
function scheduleFields() {
  for(let i=0;i<2;i++) for(const key of ['start','end']) {
    const p=`#window-${i}-${key}`, fixed=$(p+'-kind').value==='fixed';
    $(p+'-time-label').hidden=!fixed;$(p+'-offset-label').hidden=fixed;
    $(p+'-time').disabled=!fixed;$(p+'-offset').disabled=fixed;
  }
}
$('#schedule-form').oninput=scheduleFields;
function renderSchedule(s) {
  if (!s) return;
  if (!editingWindowPresets && s.window_presets) for (const key of ['evening','morning']) {
    $('#'+key+'-brightness').value=s.window_presets[key].brightness;$('#'+key+'-effect').value=s.window_presets[key].effect;
  }
  for (const el of $('#window-presets-form').elements) el.disabled=busy || uploading || pendingRestart || state.protection?.calibrating || !s.window_presets;
    if (!editingNight && s.night_gap) {$('#night-brightness').value=s.night_gap.brightness;$('#night-effect').value=s.night_gap.effect;}
    for (const el of $('#night-form').elements) el.disabled=busy || uploading || pendingRestart || state.protection?.calibrating || !s.night_gap;
  $('#clock-time').textContent=s.local_time || 'Čekám na čas z internetu…';
  $('#clock-status').textContent=!s.time_valid ? 'Čas dosud není známý. Pro synchronizaci je potřeba Wi-Fi s přístupem k internetu.' : s.synced ? 'Čas synchronizován z internetu · '+s.timezone : 'Hodiny běží z uchovaného času; čekám na novou synchronizaci.';
  $('#sunrise-time').textContent=s.sunrise || (s.time_valid?'Dnes nenastává':'—');
  $('#sunset-time').textContent=s.sunset || (s.time_valid?'Dnes nenastává':'—');
  $('#schedule-next').textContent=s.enabled ? s.next_change || 'Žádná v nejbližších dnech' : 'Plán vypnutý';
  const preset=s.phase==='night'?s.night_gap:s.window_presets?.[s.phase];
  const phaseName={evening:'Večer',morning:'Ráno',night:'Noční mezera'}[s.phase];
  const message=!s.enabled ? 'Časový plán je vypnutý. Světlo ovládáš ručně.' : s.manual_override ? 'Ruční ovládání do '+(s.override_until || 'další známé změny plánu') : !s.time_valid ? 'Automatika čeká zhasnutá na platný čas.' : state.protection?.fault ? 'Plán čeká na ruční potvrzení zásahu ochrany.' : preset ? phaseName+': '+(s.scheduled_on?preset.brightness+' % · '+modes[preset.effect]:'zhasnuto.') : 'Automatika: podle plánu má být zhasnuto.';
  $('#schedule-status').textContent=message;$('#schedule-light-status').textContent=message;
  $('#schedule-resume').hidden=!s.enabled || !s.manual_override;
  $('#schedule-resume').disabled=busy || uploading || pendingRestart || state.protection?.calibrating;
  $('#schedule-save').disabled=!scheduleLoaded || busy || uploading || pendingRestart || state.protection?.calibrating;
}
function showDay(day) {
  if(!day) {$('#schedule-day').textContent='Náhled bude dostupný po získání času z internetu.';return;}
  $('#schedule-day').textContent=day.windows.map((w,i)=>(i===0?'Večer: ':'Ráno: ')+(!w.enabled?'vypnuto':w.start&&w.end?w.start+' → '+w.end:'dnes se neprovede (pořadí časů nebo chybějící slunce)')).join(' · ');
  const n=day.night_gap;
  if(n) $('#schedule-day').textContent+=' · Mezi večerem a následujícím ránem: '+(!n.enabled?'zhasnuto':n.start&&n.end?n.start+' → '+n.end:'bez platné mezery');
}
async function loadSchedule() {
  try {
    const data=await api('/api/schedule'),c=data.config;
    $('#schedule-enabled').checked=c.enabled;$('#one-percent-off').checked=c.one_percent_off;
    $('#schedule-zone').value=c.timezone;$('#schedule-lat').value=Number(c.latitude.toFixed(4));$('#schedule-lon').value=Number(c.longitude.toFixed(4));
    c.windows.forEach((w,i)=>{
      $(`#window-${i}-enabled`).checked=w.enabled;$(`#window-${i}-overnight`).checked=w.overnight;
      for(const key of ['start','end']) {
        const p=`#window-${i}-${key}`,v=w[key];
        $(p+'-kind').value=v.kind;$(p+'-time').value=String(Math.floor(v.minute/60)).padStart(2,'0')+':'+String(v.minute%60).padStart(2,'0');$(p+'-offset').value=v.offset;
      }
    });
    if(data.status.local_time) $('#schedule-date').value=data.status.local_time.slice(0,10);
    scheduleLoaded=true;scheduleFields();showDay(data.day);if(state)renderSchedule(data.status);
  } catch(e) {notice('Nastavení času se nepodařilo načíst: '+e.message,true);}
}
$('#schedule-form').onsubmit=async event=>{
  event.preventDefault();if(busy || !scheduleLoaded)return;busy=true;$('#schedule-save').disabled=true;
  try {
    const windows=[0,1].map(i=>{
      const w={enabled:$(`#window-${i}-enabled`).checked,overnight:$(`#window-${i}-overnight`).checked};
      for(const key of ['start','end']) {
        const p=`#window-${i}-${key}`,parts=$(p+'-time').value.split(':').map(Number);
        const kind=$(p+'-kind').value;
        w[key]={kind,minute:kind==='fixed'?parts[0]*60+parts[1]:0,offset:kind==='fixed'?0:Number($(p+'-offset').value)};
      }
      return w;
    });
    await api('/api/schedule',{enabled:$('#schedule-enabled').checked,one_percent_off:$('#one-percent-off').checked,timezone:$('#schedule-zone').value,latitude:Number($('#schedule-lat').value),longitude:Number($('#schedule-lon').value),windows});
    await loadSchedule();notice('Čas, plán a chování při 1 % uloženy v desce.');
  }catch(e){notice(e.message,true);}finally{busy=false;await poll();}
};
$('#schedule-resume').onclick=async()=>{if(busy)return;busy=true;try{await api('/api/schedule/resume',{});notice('Řízení vráceno časovému plánu.');}catch(e){notice(e.message,true);}finally{busy=false;await poll();}};
$('#window-presets-form').oninput=()=>{editingWindowPresets=true;};
$('#window-presets-form').onsubmit=async event=>{
  event.preventDefault();if(busy)return;busy=true;$('#window-presets-save').disabled=true;
  try {
    const presets={};for(const key of ['evening','morning']) presets[key]={brightness:Number($('#'+key+'-brightness').value),effect:$('#'+key+'-effect').value};
    await api('/api/schedule/presets',presets);editingWindowPresets=false;notice('Večerní a ranní svícení uloženo v desce.');
  }catch(e){notice(e.message,true);}finally{busy=false;await poll();}
};
$('#night-form').oninput=()=>{editingNight=true;};
$('#night-form').onsubmit=async event=>{
  event.preventDefault();if(busy)return;busy=true;$('#night-save').disabled=true;
  try {await api('/api/schedule/night',{brightness:Number($('#night-brightness').value),effect:$('#night-effect').value});editingNight=false;showDay((await api('/api/schedule')).day);notice('Noční režim uložen v desce.');}
  catch(e){notice(e.message,true);}finally{busy=false;await poll();}
};
$('#schedule-preview').onclick=async()=>{try{const date=$('#schedule-date');if(!date.value || !date.reportValidity())return;showDay((await api('/api/schedule?date='+encodeURIComponent(date.value))).day);}catch(e){notice(e.message,true);}};
window.addEventListener('hashchange',navigate); navigate(); poll(); loadSettings(); loadSchedule(); setInterval(poll,2000);
