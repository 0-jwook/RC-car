#pragma once
#include <pgmspace.h>

// =============================================================
// 웹 UI (HTML/CSS/JS) - PROGMEM 저장
// ESP32 플래시(ROM)에 저장되며 SRAM을 소비하지 않음
//
// 대안 (SPIFFS): data/index.html 로 저장 후
//   request->send(SPIFFS, "/index.html", "text/html");
//   빌드 후 "Upload Filesystem Image" 별도 실행 필요
// =============================================================
const char index_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Mecanum RC Car</title>
<style>
:root{
  --bg:#0d0d1a;
  --surf:#181828;
  --acc:#00d4ff;
  --red:#ff3355;
  --txt:#dde8ff;
  --dim:#445588;
  --border:#222244;
}
*{box-sizing:border-box;margin:0;padding:0}
html,body{
  height:100%;overflow:hidden;
  background:var(--bg);color:var(--txt);
  font-family:'Segoe UI',system-ui,sans-serif;
  -webkit-tap-highlight-color:transparent;
}
body{display:flex;flex-direction:column;max-width:480px;margin:0 auto}

/* ── Header ── */
header{
  display:flex;align-items:center;justify-content:space-between;
  padding:10px 14px;background:var(--surf);
  border-bottom:1px solid var(--border);flex-shrink:0;
}
h1{font-size:.9rem;letter-spacing:2px;font-weight:700;color:var(--acc)}
#cw{display:flex;align-items:center;gap:6px;font-size:.72rem;color:var(--dim)}
#cd{
  width:9px;height:9px;border-radius:50%;
  background:var(--red);transition:background .3s,box-shadow .3s;
}
#cd.on{background:#33ff88;box-shadow:0 0 8px #33ff88}

/* ── Main ── */
main{
  flex:1;display:flex;flex-direction:column;
  align-items:center;padding:8px 12px;gap:9px;overflow:hidden;
}

/* ── Joystick ── */
#jc{touch-action:none;display:block;cursor:grab;flex-shrink:0}
#jc:active{cursor:grabbing}

/* ── Value Display ── */
#vd{
  display:flex;gap:18px;justify-content:center;
  background:var(--surf);padding:6px 14px;border-radius:8px;
  font-size:.7rem;color:var(--dim);border:1px solid var(--border);
  width:100%;
}
#vd b{color:var(--acc);font-weight:700}

/* ── Rotation Row ── */
#rr{display:flex;align-items:center;gap:14px}
.rb{
  width:56px;height:56px;border-radius:50%;
  border:2px solid var(--acc);background:transparent;
  color:var(--acc);font-size:1.4rem;
  cursor:pointer;display:flex;align-items:center;justify-content:center;
  touch-action:none;user-select:none;-webkit-user-select:none;
  transition:background .1s,color .1s,box-shadow .1s;
}
.rb.on{background:var(--acc);color:#000;box-shadow:0 0 14px var(--acc)}
#rl{text-align:center;font-size:.72rem;color:var(--dim);min-width:54px}
#rl strong{display:block;color:var(--acc);font-size:.9rem;margin-top:2px}

/* ── Speed Row ── */
#sr{display:flex;align-items:center;gap:10px;width:100%}
#sr label{font-size:.78rem;min-width:48px;color:var(--dim)}
input[type=range]{flex:1;accent-color:var(--acc);height:4px}
#sv{min-width:34px;text-align:right;color:var(--acc);font-weight:700;font-size:.82rem}

/* ── Emergency Stop ── */
#es{
  width:100%;padding:13px;
  background:var(--red);border:none;color:#fff;
  font-size:1rem;font-weight:700;border-radius:12px;
  cursor:pointer;letter-spacing:3px;touch-action:manipulation;
  flex-shrink:0;
}
#es:active{opacity:.75}
</style>
</head>
<body>

<header>
  <h1>MECANUM RC CAR</h1>
  <div id="cw">
    <div id="cd"></div>
    <span id="ct">Disconnected</span>
  </div>
</header>

<main>
  <!-- Virtual Joystick Canvas -->
  <canvas id="jc"></canvas>

  <!-- Real-time Value Display -->
  <div id="vd">
    Vx:&nbsp;<b id="dx">0.00</b>
    &emsp;Vy:&nbsp;<b id="dy">0.00</b>
    &emsp;&omega;:&nbsp;<b id="dw">0.00</b>
  </div>

  <!-- Rotation Buttons (hold to rotate) -->
  <div id="rr">
    <button class="rb" id="bl" title="CCW Rotate">&#8634;</button>
    <div id="rl">Rotate<strong id="rs">STOP</strong></div>
    <button class="rb" id="br" title="CW Rotate">&#8635;</button>
  </div>

  <!-- Speed Slider -->
  <div id="sr">
    <label>Speed</label>
    <input type="range" id="sp" min="0" max="100" value="60">
    <span id="sv">60%</span>
  </div>

  <!-- Emergency Stop -->
  <button id="es">EMERGENCY STOP</button>
</main>

<script>
(function(){
'use strict';

// ────────────────────────────────────────────────
// WebSocket
// ────────────────────────────────────────────────
var ws, vx=0, vy=0, om=0, spd=60;

function connect(){
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen    = function(){ setClass('cd','on'); setText('ct','Connected'); };
  ws.onclose   = function(){ setClass('cd',''); setText('ct','Disconnected'); setTimeout(connect, 2000); };
  ws.onerror   = function(){ ws.close(); };
  ws.onmessage = function(e){ /* 서버 → 클라이언트 메시지 (향후 확장용) */ };
}

// 20 Hz 송신 (워치독 하트비트 겸용)
setInterval(function(){
  if(ws && ws.readyState === 1){
    ws.send(
      '{"vx":' + vx.toFixed(3) +
      ',"vy":' + vy.toFixed(3) +
      ',"w":'  + om.toFixed(3) +
      ',"speed":' + spd + '}'
    );
  }
}, 50);

connect();

// ────────────────────────────────────────────────
// DOM 헬퍼
// ────────────────────────────────────────────────
function g(id)       { return document.getElementById(id); }
function setClass(id,v){ g(id).className = v; }
function setText(id,v) { g(id).textContent = v; }

function updateDisplay(){
  setText('dx', vx.toFixed(2));
  setText('dy', vy.toFixed(2));
  setText('dw', om.toFixed(2));
}

// ────────────────────────────────────────────────
// Virtual Joystick (Canvas)
// ────────────────────────────────────────────────
var cv  = g('jc');
var ctx = cv.getContext('2d');
var CX, CY, OR;
var IR  = 30;           // 스틱 반지름(px)
var sx, sy;             // 스틱 현재 위치
var jOn = false;

function resizeCanvas(){
  var sz = Math.min(window.innerWidth * 0.72, 256) | 0;
  cv.width = cv.height = sz;
  CX = CY = sz >> 1;
  OR = CX - 10;
  sx = CX; sy = CY;
  drawJoystick();
}

function drawJoystick(){
  var w = cv.width, h = cv.height;
  ctx.clearRect(0, 0, w, h);

  // 외곽 원 배경
  ctx.beginPath();
  ctx.arc(CX, CY, OR, 0, 6.2832);
  ctx.fillStyle = 'rgba(18,18,36,0.88)';
  ctx.fill();
  ctx.strokeStyle = '#223355';
  ctx.lineWidth = 2;
  ctx.stroke();

  // 십자 가이드 라인
  ctx.strokeStyle = '#1a2a3a';
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(CX, CY-OR); ctx.lineTo(CX, CY+OR); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(CX-OR, CY); ctx.lineTo(CX+OR, CY); ctx.stroke();

  // 중심 → 스틱 연결선
  if(jOn){
    ctx.beginPath(); ctx.moveTo(CX, CY); ctx.lineTo(sx, sy);
    ctx.strokeStyle = 'rgba(0,212,255,0.25)';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  // 스틱 (방사형 그라데이션)
  var g2 = ctx.createRadialGradient(sx, sy, 1, sx, sy, IR);
  g2.addColorStop(0, 'rgba(0,212,255,0.95)');
  g2.addColorStop(1, 'rgba(0,212,255,0.06)');
  ctx.beginPath();
  ctx.arc(sx, sy, IR, 0, 6.2832);
  ctx.fillStyle = g2;
  ctx.fill();
  ctx.strokeStyle = '#00d4ff';
  ctx.lineWidth = 2;
  ctx.stroke();
}

function moveStick(clientX, clientY){
  var rect = cv.getBoundingClientRect();
  var scale = cv.width / rect.width;
  var dx = (clientX - rect.left) * scale - CX;
  var dy = (clientY - rect.top)  * scale - CY;
  var dist = Math.sqrt(dx*dx + dy*dy);
  var maxR = OR - IR;
  if(dist > maxR){ dx = dx/dist*maxR; dy = dy/dist*maxR; }
  sx = CX + dx;
  sy = CY + dy;
  vx =  dx / maxR;   // 우측(+) / 좌측(-)
  vy = -dy / maxR;   // 전진(+) / 후진(-)
  updateDisplay();
  drawJoystick();
}

function releaseStick(){
  jOn = false;
  sx = CX; sy = CY;
  vx = 0; vy = 0;
  updateDisplay();
  drawJoystick();
}

// Pointer Events: 터치/마우스 통합 처리
cv.addEventListener('pointerdown', function(e){
  e.preventDefault();
  cv.setPointerCapture(e.pointerId);  // 캔버스 밖으로 이동해도 추적
  jOn = true;
  moveStick(e.clientX, e.clientY);
}, {passive: false});

cv.addEventListener('pointermove', function(e){
  if(jOn) moveStick(e.clientX, e.clientY);
}, {passive: false});

cv.addEventListener('pointerup',     releaseStick);
cv.addEventListener('pointercancel', releaseStick);

// ────────────────────────────────────────────────
// Rotation Buttons (누르는 동안 회전)
// ────────────────────────────────────────────────
function setOmega(val){
  om = val;
  g('bl').className = 'rb' + (val < 0 ? ' on' : '');
  g('br').className = 'rb' + (val > 0 ? ' on' : '');
  setText('rs', val < 0 ? 'CCW' : val > 0 ? 'CW' : 'STOP');
  updateDisplay();
}

var bl = g('bl'), br = g('br');

bl.addEventListener('pointerdown', function(e){ e.preventDefault(); bl.setPointerCapture(e.pointerId); setOmega(-1); }, {passive:false});
br.addEventListener('pointerdown', function(e){ e.preventDefault(); br.setPointerCapture(e.pointerId); setOmega( 1); }, {passive:false});

['pointerup','pointercancel'].forEach(function(ev){
  bl.addEventListener(ev, function(e){ e.preventDefault(); if(om < 0) setOmega(0); }, {passive:false});
  br.addEventListener(ev, function(e){ e.preventDefault(); if(om > 0) setOmega(0); }, {passive:false});
});

// 모바일 롱프레스 컨텍스트 메뉴 방지
bl.addEventListener('contextmenu', function(e){ e.preventDefault(); });
br.addEventListener('contextmenu', function(e){ e.preventDefault(); });

// ────────────────────────────────────────────────
// Speed Slider
// ────────────────────────────────────────────────
var spEl = g('sp');
spEl.addEventListener('input', function(){
  spd = +spEl.value;
  setText('sv', spd + '%');
});

// ────────────────────────────────────────────────
// Emergency Stop
// ────────────────────────────────────────────────
function doStop(e){
  e.preventDefault();
  vx = 0; vy = 0;
  sx = CX; sy = CY;
  setOmega(0);
  drawJoystick();
  // 즉시 전송
  if(ws && ws.readyState === 1){
    ws.send('{"vx":0,"vy":0,"w":0,"speed":0}');
  }
}

g('es').addEventListener('click',      doStop);
g('es').addEventListener('touchstart', doStop, {passive:false});

// ────────────────────────────────────────────────
// Init
// ────────────────────────────────────────────────
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

})();
</script>
</body>
</html>
)=====";
