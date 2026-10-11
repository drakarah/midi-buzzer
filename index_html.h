// Web UI served by the ESP32. The MIDI file is parsed in the browser and sent
// to the board as a compact event stream (4 bytes/event, see midi_buzzer.ino).
#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MIDI Buzzer</title>
<style>
:root{--bg:#14161a;--card:#1e2127;--fg:#e6e8eb;--dim:#8b929c;--acc:#ffb020;--line:#2d323b}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;max-width:900px;margin-inline:auto}
h1{font-size:22px;margin:4px 0 14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
#drop{border:2px dashed var(--line);border-radius:10px;padding:34px 10px;text-align:center;cursor:pointer;color:var(--dim);transition:.15s}
#drop.over{border-color:var(--acc);color:var(--acc);background:#2a2518}
canvas{width:100%;height:140px;background:#101216;border-radius:6px;display:block}
button{background:var(--acc);color:#111;border:0;border-radius:6px;padding:9px 18px;font-weight:600;font-size:15px;cursor:pointer}
button.sec{background:#323843;color:var(--fg)}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:12px}
.bar{height:8px;background:#101216;border-radius:4px;overflow:hidden;flex:1;min-width:120px}
.bar>div{height:100%;width:0;background:var(--acc)}
.ch{display:flex;align-items:center;gap:8px;padding:3px 0}
.sw{width:12px;height:12px;border-radius:3px;flex:none}
.ch small,.hint{color:var(--dim)}
.set{display:grid;grid-template-columns:1fr 1fr;gap:10px 22px}
@media(max-width:640px){.set{grid-template-columns:1fr}}
.set label{display:block}
.set input[type=range]{width:100%}
.hint{font-size:12.5px}
#msg{min-height:20px;color:var(--dim)}
#msg.err{color:#ff6b6b}
</style></head><body>
<h1>&#128276; MIDI Buzzer</h1>

<div id="drop" class="card">Drop a .mid file here<br><small>or click to browse</small>
<input type="file" id="file" accept=".mid,.midi,audio/midi" hidden></div>

<div id="song" class="card" hidden>
  <div><b id="sname"></b> <span class="hint" id="sinfo"></span></div>
  <canvas id="roll" width="1000" height="160" style="margin-top:10px"></canvas>
  <div id="chans" style="margin-top:10px"></div>
  <div class="row">
    <button id="play">&#9654; Play</button>
    <button id="stop" class="sec">&#9632; Stop</button>
    <label><input type="checkbox" id="loop"> Loop</label>
    <div class="bar"><div id="prog"></div></div>
    <span class="hint" id="time">0:00</span>
  </div>
</div>

<div class="card">
  <div class="set">
    <label>Arpeggio step: <b id="v_arp">20</b> ms
      <input type="range" id="arp" min="6" max="50" value="20">
      <span class="hint">Faster = smoother chords, but buzzier. 16&ndash;25 is the classic sweet spot.</span></label>
    <label>Max notes in the arpeggio: <b id="v_maxn">4</b>
      <input type="range" id="maxn" min="1" max="6" value="4">
      <span class="hint">1 = melody (top voice) only.</span></label>
    <label>Lowest note: <b id="v_minn">48</b> (<span id="n_minn">C3</span>)
      <input type="range" id="minn" min="40" max="60" value="48">
      <span class="hint">Notes below this jump up whole octaves (tiny buzzers can't do bass).</span></label>
    <div>
      <label><input type="checkbox" id="boost" checked> Melody weight (top note gets every other slot)</label>
      <label><input type="checkbox" id="drums" checked> Drums (pitch-sweep kick, noise snare/hat)</label>
      <label><input type="checkbox" id="dyn"> Dynamics (velocity &rarr; PWM duty = quieter)</label>
    </div>
  </div>
</div>
<div id="msg"></div>

<script>
const $=id=>document.getElementById(id);
const NN=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const nn=n=>NN[n%12]+(Math.floor(n/12)-1);
const FAM=['Piano','Chromatic perc.','Organ','Guitar','Bass','Strings','Ensemble','Brass','Reed','Pipe','Synth lead','Synth pad','Synth FX','Ethnic','Percussive','Sound FX'];

//--MIDI-BEGIN
// Returns {notes:[{ch,note,vel,start,end}] (ms), length, chans:{ch:{program,name,count,lo,hi}}}
function parseMidi(buf){
  const d=new DataView(buf);let p=0;
  const str=n=>{let s='';for(let i=0;i<n;i++)s+=String.fromCharCode(d.getUint8(p++));return s;};
  const vlq=()=>{let v=0,b;do{b=d.getUint8(p++);v=(v*128)+(b&127);}while(b&128);return v;};
  if(str(4)!=='MThd')throw new Error('Not a MIDI file');
  const hl=d.getUint32(p);p+=4;
  const nt=d.getUint16(p+2),div=d.getUint16(p+4);p+=hl;
  const raw=[],tempos=[],prog={},names={};let seq=0;
  for(let t=0;t<nt&&p+8<=d.byteLength;t++){
    const id=str(4),len=d.getUint32(p);p+=4;
    const end=Math.min(p+len,d.byteLength);
    if(id!=='MTrk'){p=end;continue;}
    let tick=0,run=0,tname='';
    while(p<end){
      tick+=vlq();
      let st=d.getUint8(p);
      if(st<0x80)st=run;else{p++;if(st<0xF0)run=st;}
      if(st===0xFF){
        const type=d.getUint8(p++),l=vlq();
        if(type===0x51&&l===3)tempos.push({tick,us:(d.getUint8(p)<<16)|(d.getUint8(p+1)<<8)|d.getUint8(p+2)});
        else if(type===3){tname='';for(let i=0;i<l;i++)tname+=String.fromCharCode(d.getUint8(p+i));}
        p+=l;
      }else if(st===0xF0||st===0xF7){run=0;p+=vlq();}
      else{
        const hi=st&0xF0,ch=st&15;
        if(hi===0xC0){if(!(ch in prog))prog[ch]=d.getUint8(p);p+=1;}
        else if(hi===0xD0)p+=1;
        else{
          const a=d.getUint8(p),b=d.getUint8(p+1);p+=2;
          if(hi===0x90||hi===0x80){
            const on=hi===0x90&&b>0;
            if(on&&!(ch in names))names[ch]=tname;
            raw.push({tick,ch,note:a,vel:b,on,seq:seq++});
          }
        }
      }
    }
    p=end;
  }
  // tick -> ms
  let msPerTick;const smpte=div&0x8000;
  if(smpte){const fps=256-(div>>8),tpf=div&255;msPerTick=()=>1000/(fps*tpf);}
  tempos.sort((a,b)=>a.tick-b.tick);
  raw.sort((a,b)=>a.tick-b.tick||(a.on-b.on)||a.seq-b.seq);   // offs before ons on the same tick
  let ti=0,curUs=500000,baseTick=0,baseMs=0;
  const toMs=tick=>{
    if(smpte)return tick*msPerTick();
    while(ti<tempos.length&&tempos[ti].tick<=tick){
      baseMs+=(tempos[ti].tick-baseTick)*curUs/1000/div;baseTick=tempos[ti].tick;curUs=tempos[ti].us;ti++;
    }
    return baseMs+(tick-baseTick)*curUs/1000/div;
  };
  const open={},notes=[];let length=0;
  for(const e of raw){
    const ms=toMs(e.tick),k=e.ch*128+e.note;
    if(e.on){(open[k]=open[k]||[]).push({ch:e.ch,note:e.note,vel:e.vel,start:ms});}
    else if(open[k]&&open[k].length){const n=open[k].shift();n.end=Math.max(ms,n.start+10);notes.push(n);}
    length=Math.max(length,ms);
  }
  for(const k in open)for(const n of open[k]){n.end=Math.max(length,n.start+10);notes.push(n);}
  const chans={};
  for(const n of notes){
    const c=chans[n.ch]||(chans[n.ch]={program:prog[n.ch]||0,name:names[n.ch]||'',count:0,lo:127,hi:0});
    c.count++;c.lo=Math.min(c.lo,n.note);c.hi=Math.max(c.hi,n.note);
  }
  return{notes,length,chans};
}

// Event stream: u16 dt(ms) | u8 on<<7|note | u8 ch<<4|vel4.  0x7F/0xFF = no-op (long gaps).
function encodeSong(song,enabled){
  const ev=[];
  for(const n of song.notes){
    if(!enabled[n.ch])continue;
    const v=Math.max(1,n.vel>>3);
    ev.push({t:Math.round(n.start),on:1,ch:n.ch,note:n.note,v});
    if(n.ch!==9)ev.push({t:Math.round(n.end),on:0,ch:n.ch,note:n.note,v});   // drums are one-shots
  }
  ev.sort((a,b)=>a.t-b.t||a.on-b.on);
  const out=new Uint8Array(ev.length*4+Math.ceil((ev.length?ev[ev.length-1].t:0)/65535)*4+4);
  let o=0,last=0;
  for(const e of ev){
    let dt=e.t-last;
    while(dt>65535){out[o++]=255;out[o++]=255;out[o++]=0x7F;out[o++]=0xFF;dt-=65535;}
    out[o++]=dt&255;out[o++]=dt>>8;
    out[o++]=(e.on?0x80:0)|e.note;out[o++]=(e.ch<<4)|e.v;
    last=e.t;
  }
  return out.subarray(0,o);
}
//--MIDI-END

let song=null,enabled={},cap=1e9,playing=false,lastLen=0;

function msg(t,err){const m=$('msg');m.textContent=t;m.className=err?'err':'';}
const col=ch=>ch===9?'#9aa0a6':`hsl(${(ch*47)%360} 70% 60%)`;
const fmt=ms=>{const s=Math.floor(ms/1000);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');};

function drawRoll(){
  const c=$('roll'),g=c.getContext('2d'),W=c.width,H=c.height;
  g.clearRect(0,0,W,H);
  let lo=127,hi=0;
  for(const n of song.notes)if(n.ch!==9&&enabled[n.ch]){lo=Math.min(lo,n.note);hi=Math.max(hi,n.note);}
  if(lo>hi){lo=48;hi=72;}
  const rows=hi-lo+1,rh=Math.max(1,(H-14)/rows),sx=W/Math.max(1,song.length);
  for(const n of song.notes){
    if(!enabled[n.ch])continue;
    g.fillStyle=col(n.ch);
    if(n.ch===9)g.fillRect(n.start*sx,H-12,2,12);
    else g.fillRect(n.start*sx,(hi-n.note)*rh,Math.max(1,(n.end-n.start)*sx),Math.max(1,rh-0.5));
  }
}

function drawChans(){
  const box=$('chans');box.innerHTML='';
  for(const ch of Object.keys(song.chans).map(Number).sort((a,b)=>a-b)){
    const c=song.chans[ch],row=document.createElement('label');row.className='ch';
    const cb=document.createElement('input');cb.type='checkbox';cb.checked=!!enabled[ch];
    cb.onchange=()=>{enabled[ch]=cb.checked;drawRoll();send(true);};
    const sw=document.createElement('span');sw.className='sw';sw.style.background=col(ch);
    const lab=document.createElement('span');
    const what=ch===9?'Drums':(FAM[c.program>>3]||'?');
    lab.innerHTML=`Ch ${ch+1} &middot; ${what} <small>${c.name?'&ldquo;'+c.name.replace(/</g,'&lt;')+'&rdquo; &middot; ':''}${c.count} notes${ch===9?'':' &middot; '+nn(c.lo)+'&ndash;'+nn(c.hi)}</small>`;
    row.append(cb,sw,lab);box.append(row);
  }
}

async function loadFile(f){
  try{
    song=parseMidi(await f.arrayBuffer());
  }catch(e){msg('Could not read MIDI: '+e.message,true);return;}
  enabled={};for(const ch in song.chans)enabled[ch]=true;
  $('song').hidden=false;$('sname').textContent=f.name;
  $('sinfo').textContent=`${fmt(song.length)} · ${song.notes.length} notes`;
  drawChans();drawRoll();
  await send(true);
}

let sending=false,again=false,againPlay=false;
async function send(play){
  if(sending){again=true;againPlay=againPlay||play;return;}
  sending=true;
  try{
    const bytes=encodeSong(song,enabled);
    if(bytes.length>cap){msg(`Too big for RAM: ${(bytes.length/1024).toFixed(0)} KB needed, ${(cap/1024).toFixed(0)} KB free. Mute some channels.`,true);return;}
    msg(`Uploading ${(bytes.length/1024).toFixed(1)} KB (${bytes.length/4} events)…`);
    const fd=new FormData();fd.append('song',new Blob([bytes]),'song.bin');
    const r=await fetch('/upload?n='+bytes.length,{method:'POST',body:fd});
    const j=await r.json();
    if(!j.ok){msg('Upload failed: '+(j.err||r.status),true);return;}
    lastLen=j.len;
    msg(`Loaded ${j.events} events.`);
    if(play)await fetch('/play',{method:'POST'});
  }catch(e){msg('Upload failed: '+e.message,true);}
  finally{
    sending=false;
    if(again){again=false;const p=againPlay;againPlay=false;send(p);}
  }
}

// settings
const cfgIds=['arp','maxn','minn'],cfgChk=['boost','drums','dyn','loop'];
let cfgT=0;
function pushCfg(){
  const q=new URLSearchParams();
  cfgIds.forEach(i=>q.set(i,$(i).value));
  cfgChk.forEach(i=>q.set(i,$(i).checked?1:0));
  fetch('/cfg?'+q).catch(()=>{});
}
cfgIds.forEach(i=>$(i).oninput=()=>{
  $('v_'+i).textContent=$(i).value;
  if(i==='minn')$('n_minn').textContent=nn(+$(i).value);
  clearTimeout(cfgT);cfgT=setTimeout(pushCfg,120);
});
cfgChk.forEach(i=>$(i).onchange=pushCfg);

$('play').onclick=()=>fetch('/play',{method:'POST'});
$('stop').onclick=()=>fetch('/stop',{method:'POST'});

// drop zone
const dz=$('drop');
dz.onclick=()=>$('file').click();
$('file').onchange=e=>{if(e.target.files[0])loadFile(e.target.files[0]);};
['dragenter','dragover'].forEach(t=>dz.addEventListener(t,e=>{e.preventDefault();dz.classList.add('over');}));
['dragleave','drop'].forEach(t=>dz.addEventListener(t,e=>{e.preventDefault();dz.classList.remove('over');}));
dz.addEventListener('drop',e=>{if(e.dataTransfer.files[0])loadFile(e.dataTransfer.files[0]);});
// allow dropping anywhere on the page
addEventListener('dragover',e=>e.preventDefault());
addEventListener('drop',e=>{e.preventDefault();if(e.dataTransfer.files[0])loadFile(e.dataTransfer.files[0]);});

async function poll(){
  try{
    const s=await (await fetch('/status')).json();
    cap=s.cap;playing=s.playing;
    $('prog').style.width=(s.len?Math.min(100,100*s.pos/s.len):0)+'%';
    $('time').textContent=fmt(s.pos)+' / '+fmt(s.len);
    $('play').textContent=s.playing?'↺ Restart':'▶ Play';
  }catch(e){}
}
setInterval(poll,400);poll();pushCfg();
</script></body></html>)HTML";
