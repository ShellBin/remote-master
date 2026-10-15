#pragma once
#include <Arduino.h>

// LittleFS 未部署前端时下发的内置页面：确认设备存活、查看实时状态、
// 整目录上传前端产物（也可不开 node 直接浏览器拖整个 dist 目录）。
static const char kFallbackHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Remote Master — 设备就绪</title>
<style>
  :root{--bg:#0f1115;--panel:#1a1d24;--line:#2a2e38;--fg:#e6e6e6;--muted:#8a8f99;--accent:#4f8cff;--ok:#34c759;--warn:#ff9f0a;--err:#ff6b6b}
  *{box-sizing:border-box}
  body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--fg);line-height:1.5}
  .wrap{max-width:760px;margin:0 auto;padding:24px 16px 64px}
  h1{font-size:22px;margin:0 0 4px}
  .sub{color:var(--muted);font-size:13px;margin-bottom:24px}
  .card{background:var(--panel);border-radius:12px;padding:16px 18px;margin-bottom:16px}
  .card h2{font-size:15px;margin:0 0 12px;color:var(--muted);font-weight:600;letter-spacing:.02em}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--muted);margin-right:6px;vertical-align:middle}
  .dot.ok{background:var(--ok)}
  .dot.warn{background:var(--warn)}
  .badge{font-size:12px;padding:2px 8px;border-radius:10px;background:var(--line);color:var(--muted);white-space:nowrap}
  .badge.ok{background:rgba(52,199,89,.15);color:var(--ok)}
  .badge.warn{background:rgba(255,159,10,.15);color:var(--warn)}
  .hint{font-size:13px;color:var(--muted);margin:8px 0}
  code{background:var(--line);padding:2px 6px;border-radius:4px;font-size:12px}
  .upload{display:flex;flex-direction:column;gap:10px}
  button{padding:8px 14px;border:none;border-radius:8px;font-size:14px;cursor:pointer;background:var(--accent);color:#fff}
  button.ghost{background:var(--line);color:var(--fg)}
  button:disabled{opacity:.45;cursor:not-allowed}
  .bar{height:6px;border-radius:3px;background:var(--line);overflow:hidden;margin-top:6px}
  .bar i{display:block;height:100%;background:var(--accent)}
  .log{font-size:12px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;margin-top:8px;max-height:220px;overflow:auto}
  .log div{padding:2px 0;color:var(--muted);word-break:break-all}
  .log div.ok{color:var(--ok)}
  .log div.err{color:var(--err)}
  .hidden-file{display:none}
</style>
</head>
<body>
<div class="wrap">
  <h1>Remote Master</h1>
  <div class="sub">ESP32-C3 BLE/IR 多功能遥控平台 — 固件运行正常，前端尚未部署</div>

  <div class="card">
    <h2>设备状态</h2>
    <div id="status"><span class="dot"></span>正在读取…</div>
    <div id="storage"></div>
  </div>

  <div class="card">
    <h2>部署前端</h2>
    <p class="hint">在项目目录构建前端包：<code>cd web &amp;&amp; npm run build</code>，产物输出到 <code>data/frontend</code>。点击下方按钮选择该目录，检测到 <code>index.html</code> 后自动上传。</p>
    <p class="hint">命令行方式：<code>node deploy.mjs http://192.168.4.1 --dir data/frontend --remote /frontend</code>。</p>
    <div class="upload">
      <button id="btn-up">选择并上传前端目录…</button>
      <input type="file" id="dir" class="hidden-file" webkitdirectory directory multiple>
      <div class="bar" id="bar-wrap" style="display:none"><i id="bar" style="width:0"></i></div>
      <div class="log" id="log"></div>
    </div>
  </div>
</div>
<script>
const $=id=>document.getElementById(id);
const FS_ROOT='/frontend';
const logBox=$('log');
function say(text,cls){const d=document.createElement('div');if(cls)d.className=cls;d.textContent=text;logBox.appendChild(d);logBox.scrollTop=logBox.scrollHeight;}
async function loadStatus(){
  try{
    const d=await (await fetch('/api/status')).json();
    const rows=[
      ['文件系统',d.filesystemMounted?'已挂载':'未挂载',d.filesystemMounted?'ok':'warn'],
      ['配置存储',d.storageReady?'就绪':'不可用',d.storageReady?'ok':'warn'],
      ['WiFi 热点',d.apSsid||'-','ok'],
      ['蓝牙名称',d.bleName||'-','ok'],
      ['BLE 连接',d.ble&&d.ble.connected?'已连接':'未连接',d.ble&&d.ble.connected?'ok':'warn'],
      ['已配对主机',String(d.ble?d.ble.bondCount:0),'ok'],
      ['空闲堆',Math.round(d.heap/1024)+' KB','ok']
    ];
    $('status').innerHTML=rows.map(([k,v,c])=>
      '<div class="row" style="display:flex;justify-content:space-between;gap:12px;padding:6px 0;border-bottom:1px solid #2a2e38;font-size:14px"><span style="color:var(--muted)">'+k+'</span><span class="badge '+c+'"><span class="dot '+c+'"></span>'+v+'</span></div>').join('');
    const total=d.fsTotal||0,used=d.fsUsed||0;
    if(total){
      const free=total-used,pct=Math.round(used*100/total);
      $('storage').innerHTML='<div class="row" style="display:flex;justify-content:space-between;gap:12px;padding:6px 0;border-bottom:1px solid #2a2e38;font-size:14px"><span style="color:var(--muted)">存储占用</span><span class="badge">'
        +Math.round(used/1024)+' / '+Math.round(total/1024)+' KB · 剩余 '+Math.round(free/1024)+' KB</span></div>'
        +'<div class="bar"><i style="width:'+pct+'%"></i></div>';
    }
  }catch(e){$('status').innerHTML='<span class="dot warn"></span>无法读取 /api/status：'+e.message;}
}
function collect(){
  const picked=[];
  const dir=$('dir').files;
  for(const f of dir){
    const rel=(f.webkitRelativePath||f.name).split('/').slice(1).join('/');
    if(rel) picked.push({file:f,path:rel});
  }
  return picked;
}
function hasIndexHtml(picked){
  return picked.some(p=>p.path==='index.html');
}
async function startUpload(){
  const picked=collect();
  if(!picked.length){say('没有选择任何文件','err');return;}
  if(!hasIndexHtml(picked)){
    say('所选目录不包含 index.html，请选择 web 构建产物（data/frontend）目录','err');
    $('dir').value='';
    return;
  }
  $('btn-up').disabled=true;
  $('bar-wrap').style.display='block';
  say('检测到 index.html，先清空 '+FS_ROOT+' …');
  try{
    const r=await fetch('/api/files?path='+encodeURIComponent(FS_ROOT),{method:'DELETE'});
    if(!r.ok){say('清空失败，已中止上传: '+await r.text(),'err');resetUI();return;}
  }catch(e){say('清空失败: '+e.message,'err');resetUI();return;}
  let failed=0,bytes=0;
  for(let i=0;i<picked.length;i++){
    const {file,path}=picked[i];
    const target=FS_ROOT+'/'+path;
    const fd=new FormData();
    fd.append('file',file);
    try{
      const r=await fetch('/api/files?path='+encodeURIComponent(target),{method:'POST',body:fd});
      const body=await r.text();
      if(!r.ok){failed++;say('✗ '+path+' — '+body,'err');}
      else{bytes+=file.size;say('✓ '+path+' ('+file.size+' B)','ok');}
    }catch(e){failed++;say('✗ '+path+' — '+e.message,'err');}
    $('bar').style.width=Math.round((i+1)*100/picked.length)+'%';
  }
  resetUI();
  if(!failed){
    say('完成：'+picked.length+' 个文件，'+bytes+' 字节。正在刷新页面…','ok');
    setTimeout(()=>location.reload(),1200);
  }else{
    say('有 '+failed+' 个文件失败，可在修好后重新上传。','err');
  }
}
function resetUI(){
  $('btn-up').disabled=false;
  $('bar').style.display='none';
}
$('btn-up').addEventListener('click',()=>$('dir').click());
$('dir').addEventListener('change',startUpload);
loadStatus();
</script>
</body>
</html>)HTML";
