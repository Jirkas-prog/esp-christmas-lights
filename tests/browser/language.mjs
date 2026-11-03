import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {chromium} from 'playwright';

const web = new URL('../../web/',import.meta.url);
const assets = {'/':['index.html','text/html'],'/app.js':['app.js','application/javascript'],'/i18n.js':['i18n.js','application/javascript'],'/style.css':['style.css','text/css']};
const server = createServer(async (req,res) => {
  const asset = assets[new URL(req.url,'http://localhost').pathname];
  if (!asset) {res.writeHead(404);res.end();return;}
  res.setHeader('Content-Type',asset[1]+'; charset=utf-8');res.end(await readFile(new URL(asset[0],web)));
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({headless:true});
let passed=0;
function fixture() {
  const limits=Object.fromEntries(['first','second','all'].map(key=>[key,{ignored:100,automatic:5,noise_mv:1,fit_error_mv:2}]));
  const schedule={enabled:true,one_percent_off:true,time_valid:true,synced:true,local_time:'2026-11-03 18:00:00',timezone:'Europe/Prague',sunrise:'07:00',sunset:'17:00',next_change:'2026-11-03 23:00:00',manual_override:false,phase:'evening',scheduled_on:true,window_presets:{evening:{brightness:70,effect:'all'},morning:{brightness:20,effect:'first'}},night_gap:{brightness:2,effect:'all'}};
  const config={enabled:true,one_percent_off:true,timezone:'Europe/Prague',latitude:0,longitude:0,windows:[{enabled:true,overnight:false,start:{kind:'fixed',minute:1080,offset:0},end:{kind:'fixed',minute:1380,offset:0}},{enabled:true,overnight:false,start:{kind:'fixed',minute:300,offset:0},end:{kind:'sunrise',minute:0,offset:0}}]};
  const day={windows:[{enabled:true,start:'18:00',end:'23:00'},{enabled:true,start:'05:00',end:'07:00'}],night_gap:{enabled:true,start:'23:00',end:'05:00'}};
  const state={alias:'vypnuto',hostname:'vypnuto',dhcp_hostname:'vypnuto',version:'0.6.2',updating:false,state:'ON',brightness:50,effect:'soft_crossfade',effect_cycle_ms:1200,interval_ms:300,output_active:true,on_preset:{brightness:70,effect:'all'},mqtt:false,mqtt_error:-2,topic:'Světlo/Zapnout',wifi:true,ip:'192.0.2.1',storage_error:false,uptime_ms:90000,wifi_rssi_dbm:-55,heap_free_bytes:100000,health:{reset_reason:'power_on',boot_count:2,last_recovery:'none',wifi_disconnects:1,wifi_disconnect_reason:3,wifi_retries:1,wifi_station_resets:0,wifi_radio_resets:0,heap_min_bytes:90000},schedule,protection:{enabled:true,calibrated:true,calibrating:false,blocked:false,fault:false,latched:false,latch_fault:false,pending_ms:0,monitoring_required:true,delay_ms:500,threshold_percent:80,samples:10,gain_a:1,gain_b:1,threshold_mv:1000,reverse_threshold_mv:1500,first_threshold_direction:'below',second_threshold_direction:'above',message:'Uložená kalibrace pasivního měření PWM načtena.',sense_output:'AOUT1',limits,ignored_short_power_mw:5100,progress:0}};
  return {state,config,day,settings:{alias:'vypnuto',ssid:'Zapnout',mqtt_host:'mqtt.example.com',mqtt_user:'Vypnout',mqtt_port:1883,mqtt_enabled:true}};
}
async function session(locale='en-US',blockedStorage=false) {
  const context=await browser.newContext({locale});const page=await context.newPage();const errors=[];const requests=[];const data=fixture();
  page.on('pageerror',e=>errors.push(e.message));
  if(blockedStorage) await page.addInitScript(()=>{Object.defineProperty(window,'localStorage',{get(){throw new DOMException('Blocked','SecurityError');}});});
  await page.route('**/*',async route=>{
    const request=route.request(),url=new URL(request.url());
    assert.equal(url.origin,origin,'No device or internet requests are permitted');
    if(!url.pathname.startsWith('/api/')) return route.continue();
    const path=url.pathname;
    if(request.method()==='POST') {
      const binary=path==='/api/update';const body=binary?request.postDataBuffer():request.postDataJSON();requests.push({path,body,headers:request.headers()});
      if(data.failure) return route.fulfill({status:400,json:{error:data.failure}});
      if(path==='/api/light') Object.assign(data.state,body);
      if(path==='/api/preset') data.state.on_preset=body;
      if(path==='/api/protection/action'&&body.action==='calibrate') Object.assign(data.state.protection,{calibrating:true,message:'Měřím normální PWM: první, druhá a obě poloviny, jas 1–100 %.',progress:42});
      return route.fulfill({json:path==='/api/light'?data.state:{ok:true}});
    }
    return route.fulfill({json:path==='/api/state'?data.state:path==='/api/settings'?data.settings:path==='/api/schedule'?{config:data.config,status:data.state.schedule,day:data.day}:{}});
  });
  await page.goto(origin);await page.waitForFunction(()=>document.querySelector('#device-name').textContent==='vypnuto'&&!document.querySelector('#schedule-save').disabled);
  async function language(value){await page.selectOption('#language',value);await page.waitForFunction(value=>document.documentElement.lang===value,value);await page.waitForTimeout(40);}
  async function close(){assert.deepEqual(errors,[]);await context.close();}
  return {page,context,data,requests,language,close};
}
async function check(name,run){await run();passed++;console.log('PASS '+name);}
try {
  await check('English and Czech across every page, errors, thermal limits and protected late text',async()=>{
    const s=await session();const {page}=s;
    assert.equal(await page.locator('html').getAttribute('lang'),'en');
    for(const hash of ['light','schedule','wifi','mqtt','system']) {
      await page.locator(`nav a[href="#${hash}"]`).click();
      const residual=await page.locator(`section[data-page="${hash}"]`).evaluate(root=>{
        const walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);const found=[];
        while(walker.nextNode()){const n=walker.currentNode;if(!n.parentElement.closest('pre,code,[data-no-translate]')&&/[čďěňřšťůž]/i.test(n.data))found.push(n.data.trim());}return found;
      });
      assert.deepEqual(residual,[],`Untranslated ${hash} text`);
    }
    const raw=await page.locator('pre,code').allTextContents();
    assert.equal(await page.title(),'vypnuto · Lights');
    assert.equal(await page.locator('#topic').textContent(),'Světlo/Zapnout/set');
    assert.match(await page.locator('#limits-power').textContent(),/Short-circuit power/);
    await page.evaluate(()=>{
      const data=document.createElement('div');data.id='literal';data.setAttribute('data-no-translate','');document.body.append(data);data.append(document.createTextNode('Zapnout Světlo vypnuto'));
      const editable=document.createElement('div');editable.id='editable';editable.contentEditable='true';document.body.append(editable);editable.append(document.createTextNode('Zapnout'));
    });
    await s.language('cs');assert.equal(await page.title(),'vypnuto · Světýlka');assert.equal(await page.locator('#power').textContent(),'Vypnout');
    await s.language('en');assert.equal(await page.locator('#power').textContent(),'Turn off');
    assert.equal(await page.locator('#literal').textContent(),'Zapnout Světlo vypnuto');assert.equal(await page.locator('#editable').textContent(),'Zapnout');assert.deepEqual(await page.locator('pre,code').allTextContents(),raw);assert.equal(s.requests.length,0);
    await s.close();
  });
  await check('Form drafts, control values, files, route and disclosure survive language changes',async()=>{
    const s=await session();const {page}=s;await page.locator('nav a[href="#wifi"]').click();
    await page.locator('[name="ssid"]').fill('Světlo Zapnout');await page.locator('[name="wifi_password"]').fill('test-only');
    await page.locator('#firmware').setInputFiles({name:'vypnuto.bin',mimeType:'application/octet-stream',buffer:Buffer.alloc(2048,7)});
    await page.evaluate(()=>{document.querySelector('#protection-panel').open=true;const input=document.querySelector('[name=ssid]');input.setSelectionRange(2,7);});
    const values=()=>page.evaluate(()=>Array.from(document.querySelectorAll('input,select:not(#language)')).map(el=>[el.id||el.name,el.value,el.checked,el.disabled]));
    const before=await values();await s.language('cs');await s.language('en');assert.deepEqual(await values(),before);
    assert.equal(new URL(page.url()).hash,'#wifi');assert.equal(await page.locator('#protection-panel').evaluate(el=>el.open),true);
    assert.deepEqual(await page.locator('[name="ssid"]').evaluate(el=>[el.selectionStart,el.selectionEnd]),[2,7]);assert.equal(s.requests.length,0);
    await s.close();
  });
  await check('Only explicit saves issue original device payloads and headers',async()=>{
    const s=await session();const {page}=s;await s.language('cs');await page.locator('[data-mode="candlelight"]').click();await page.waitForFunction(()=>document.querySelector('[data-mode=candlelight]').getAttribute('aria-pressed')==='true');
    assert.deepEqual(s.requests[0].body,{effect:'candlelight'});assert.equal(s.requests[0].headers['x-requested-with'],'Svetylka');
    await page.locator('details').filter({has:page.locator('#preset-form')}).locator('summary').click();await page.locator('#preset-brightness').fill('42');await page.selectOption('#preset-effect','second');await s.language('en');await page.locator('#preset-form button').click();
    await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('saved'));
    assert.deepEqual(s.requests.find(x=>x.path==='/api/preset').body,{brightness:42,effect:'second'});
    await page.locator('nav a[href="#schedule"]').click();await page.selectOption('#window-0-start-kind','sunset');await page.locator('#window-0-start-offset').fill('-25');await s.language('cs');await s.language('en');await page.locator('#schedule-save').click();
    await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('behaviour'));
    assert.deepEqual(s.requests.find(x=>x.path==='/api/schedule').body,{enabled:true,one_percent_off:true,timezone:'Europe/Prague',latitude:0,longitude:0,windows:[{enabled:true,overnight:false,start:{kind:'sunset',minute:0,offset:-25},end:{kind:'fixed',minute:1380,offset:0}},{enabled:true,overnight:false,start:{kind:'fixed',minute:300,offset:0},end:{kind:'sunrise',minute:0,offset:0}}]});
    await s.close();
  });
  await check('Calibration messages remain live in both languages',async()=>{
    const s=await session();await s.page.locator('#protection-panel > summary').click();await s.page.locator('#calibrate').click();await s.page.waitForFunction(()=>document.querySelector('#calibration-progress').value===42);
    assert.match(await s.page.locator('#calibration-message').textContent(),/^Measuring normal PWM/);await s.language('cs');assert.match(await s.page.locator('#calibration-message').textContent(),/^Měřím/);assert.deepEqual(s.requests.map(x=>x.body),[{action:'calibrate'}]);await s.close();
  });
  await check('Alias acknowledgements and browser title never translate the alias',async()=>{
    const s=await session();await s.page.locator('nav a[href="#system"]').click();await s.page.locator('#alias-form button').click();await s.page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('.local/'));
    for(const lang of ['cs','en']){await s.language(lang);assert.match(await s.page.locator('#notice').textContent(),/http:\/\/vypnuto\.local\//);assert.match(await s.page.title(),/^vypnuto · /);}
    assert.deepEqual(s.requests[0].body,{alias:'vypnuto'});await s.close();
  });
  await check('Device rejection and OTA failures translate without changing upload bytes',async()=>{
    const s=await session();s.data.failure='Výstup blokuje ochrana. Otevři nastavení ochrany a kalibraci.';
    await s.page.locator('#power').click();await s.page.waitForFunction(()=>document.querySelector('#notice').textContent.startsWith('Protection is blocking'));
    await s.language('cs');assert.match(await s.page.locator('#notice').textContent(),/^Výstup blokuje/);
    await s.page.locator('nav a[href="#system"]').click();
    const bytes=Buffer.alloc(2048,7);await s.page.locator('#firmware').setInputFiles({name:'Vypnout.bin',mimeType:'application/octet-stream',buffer:bytes});
    s.data.failure='Aktualizace selhala. Původní firmware zůstává aktivní.';await s.language('en');await s.page.locator('#ota-form button').click();
    await s.page.waitForFunction(()=>document.querySelector('#notice').textContent.startsWith('Update failed.'));
    const request=s.requests.find(x=>x.path==='/api/update');assert.deepEqual(request.body,bytes);assert.equal(request.headers['content-type'],'application/octet-stream');assert.equal(request.headers['x-requested-with'],'Svetylka');
    assert.equal(await s.page.locator('#upload-status').textContent(),'Transfer failed; check the device status.');await s.language('cs');assert.match(await s.page.locator('#notice').textContent(),/^Aktualizace selhala/);await s.close();
  });
  await check('Browser preference, persistence, cross-tab changes and blocked storage',async()=>{
    const s=await session('cs-CZ');assert.equal(await s.page.locator('html').getAttribute('lang'),'cs');await s.language('en');await s.page.reload();await s.page.waitForFunction(()=>document.documentElement.lang==='en');
    const second=await s.context.newPage();await second.goto(origin);await second.evaluate(()=>localStorage.setItem('christmas-lights-language','cs'));await s.page.waitForFunction(()=>document.documentElement.lang==='cs');await second.close();await s.close();
    const blocked=await session('en-US',true);await blocked.language('cs');assert.equal(await blocked.page.locator('#power').textContent(),'Vypnout');await blocked.close();
  });
  await check('Mobile language control and navigation stay within the viewport',async()=>{
    const s=await session();await s.page.setViewportSize({width:390,height:844});
    for(const lang of ['en','cs']){await s.language(lang);for(const hash of ['light','schedule','wifi','mqtt','system']){await s.page.locator(`nav a[href="#${hash}"]`).click();assert.ok(await s.page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1),`${lang}/${hash} overflow`);}}
    await s.close();
  });
  console.log(`PASS ${passed} browser language checks (mock device only)`);
} finally {await browser.close();await new Promise(resolve=>server.close(resolve));}
