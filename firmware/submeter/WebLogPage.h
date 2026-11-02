/*
 * WebLogPage.h
 * ---------------------------------------------------------------------------
 * The log page served at http://192.168.4.1/ . One self-contained file:
 * HTML, CSS and JavaScript, no external CDN, so it works with no internet.
 * Stored in flash (PROGMEM) and sent as-is by WebLog.cpp.
 *
 * WebSocket messages from the ESP32 (/ws):
 *   {"type":"log","text":"line\nline\n"}   one or more log lines
 *   {"type":"status",...,"ch":[{"led":"G","relay":1},...]}   every 5 s
 * Messages to the ESP32 (only if WEBLOG_COMMANDS 1): a plain command word.
 * ---------------------------------------------------------------------------
 */
#pragma once
#include <Arduino.h>

const char WEBLOG_PAGE[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SubMeter log</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--line:#30363d;--fg:#c9d1d9;--dim:#6e7681}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);font:13px/1.35 ui-monospace,Menlo,Consolas,monospace}
body{display:flex;flex-direction:column}
header{background:var(--panel);border-bottom:1px solid var(--line);padding:6px 10px;display:flex;flex-wrap:wrap;gap:4px 14px;align-items:center}
header b{font-size:14px}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#f85149;margin-right:4px;vertical-align:-1px}
.dot.on{background:#3fb950}
.k{color:var(--dim)}
#chips{display:flex;gap:6px;padding:6px 10px;background:var(--panel);border-bottom:1px solid var(--line);flex-wrap:wrap}
.chip{border:1px solid var(--line);border-radius:12px;padding:2px 9px;display:flex;align-items:center;gap:5px}
.led{width:10px;height:10px;border-radius:50%;background:#6e7681}
.led.G{background:#3fb950}.led.R{background:#f85149}
.off{color:#f85149}.onr{color:#3fb950}
#bar{display:flex;flex-wrap:wrap;gap:6px;padding:6px 10px;border-bottom:1px solid var(--line);align-items:center}
button,input[type=text]{background:#21262d;color:var(--fg);border:1px solid var(--line);border-radius:5px;padding:4px 8px;font:inherit}
button:active{background:#30363d}
button.act{border-color:#d29922;color:#d29922}
label{white-space:nowrap;cursor:pointer}
#tags{display:flex;flex-wrap:wrap;gap:2px 10px}
#q{flex:1;min-width:120px}
#log{flex:1;overflow-y:auto;padding:4px 10px;white-space:pre-wrap;word-break:break-all}
#log div{min-height:1.35em}
.PZEM{color:#79c0ff}.DEDUCT{color:#7ee787}.ALLOC{color:#56d4dd}.RELAY{color:#d29922}
.LED{color:#d2a8ff}.FB{color:#58a6ff}.WIFI{color:#ff7b72}.SYS{color:#e6edf3}
.CSV{color:#8b949e}.OTHER{color:var(--fg)}.err{color:#f85149!important;font-weight:bold}
#cmdrow{display:none;gap:6px;padding:6px 10px;border-top:1px solid var(--line);background:var(--panel)}
#cmd{flex:1}
</style></head><body>
<header>
 <b id="name">SubMeter</b>
 <span><span id="wsdot" class="dot"></span><span id="wstxt">offline</span></span>
 <span><span class="k">AP</span> <span id="ip">-</span></span>
 <span><span class="k">STA</span> <span id="sta">-</span></span>
 <span><span class="k">up</span> <span id="up">-</span></span>
 <span><span class="k">heap</span> <span id="heap">-</span></span>
 <span><span class="k">RSSI</span> <span id="rssi">-</span></span>
</header>
<div id="chips"></div>
<div id="bar">
 <button id="pause">Pause</button>
 <button id="scroll" class="act">Auto-scroll on</button>
 <button id="clear">Clear</button>
 <button id="dl">Download .txt</button>
 <button id="dlcsv" title="CSV header, data rows and # lines only, for tests/analysis">Download CSV</button>
 <span id="tags"></span>
 <input id="q" type="text" placeholder="search">
</div>
<div id="log"></div>
<div id="cmdrow"><input id="cmd" type="text" placeholder="status | heap | wifi | ledtest | reboot"><button id="send">Send</button></div>
<script>
var TAGS=["PZEM","DEDUCT","ALLOC","RELAY","LED","FB","WIFI","SYS","CSV","OTHER"];
var MAX_LINES=2000;      // lines kept on screen
var MAX_CSV=50000;       // contract lines kept for "Download CSV"
var $=function(id){return document.getElementById(id)};
var logEl=$("log"),lines=[],held=[],csv=[],paused=false,auto=true,show={},ws=null;

TAGS.forEach(function(t){show[t]=true;
 var l=document.createElement("label");
 l.innerHTML='<input type="checkbox" checked> <span class="'+t+'">'+t+'</span> ';
 l.firstChild.onchange=function(e){show[t]=e.target.checked;refilter()};
 $("tags").appendChild(l)});

function tagOf(s){
 if(/^(#|\d|millis,)/.test(s)||s==="")return"CSV";
 var m=s.match(/\[(PZEM|DEDUCT|ALLOC|RELAY|LED|FB|WIFI|SYS)\]/);
 return m?m[1]:"OTHER"}
function visible(o){
 if(!show[o.tag])return false;
 var q=$("q").value.toLowerCase();
 return !q||o.text.toLowerCase().indexOf(q)>=0}
function addLine(s){
 var o={text:s,tag:tagOf(s)};
 if(o.tag==="CSV"){csv.push(s);if(csv.length>MAX_CSV)csv.shift()}
 if(paused){held.push(o);if(held.length>MAX_LINES)held.shift();return}
 render(o)}
function render(o){
 var d=document.createElement("div");
 d.textContent=o.text;
 d.className=o.tag+(/ERR|FAIL/.test(o.text)?" err":"");
 if(!visible(o))d.style.display="none";
 o.el=d;logEl.appendChild(d);lines.push(o);
 while(lines.length>MAX_LINES){logEl.removeChild(lines.shift().el)}}
function refilter(){lines.forEach(function(o){o.el.style.display=visible(o)?"":"none"});scrollEnd()}
function scrollEnd(){if(auto)logEl.scrollTop=logEl.scrollHeight}
$("q").oninput=refilter;

$("pause").onclick=function(){paused=!paused;this.textContent=paused?"Resume":"Pause";
 this.className=paused?"act":"";
 if(!paused){held.forEach(render);held=[];scrollEnd()}};
$("scroll").onclick=function(){auto=!auto;this.textContent="Auto-scroll "+(auto?"on":"off");
 this.className=auto?"act":"";scrollEnd()};
$("clear").onclick=function(){logEl.innerHTML="";lines=[];held=[]};

function stamp(){var d=new Date(),p=function(n){return(n<10?"0":"")+n};
 return d.getFullYear()+p(d.getMonth()+1)+p(d.getDate())+"-"+p(d.getHours())+p(d.getMinutes())+p(d.getSeconds())}
function save(name,text){var a=document.createElement("a");
 a.href=URL.createObjectURL(new Blob([text],{type:"text/plain"}));a.download=name;
 document.body.appendChild(a);a.click();setTimeout(function(){URL.revokeObjectURL(a.href);a.remove()},500)}
$("dl").onclick=function(){save("submeter-log-"+stamp()+".txt",
 lines.filter(visible).map(function(o){return o.text}).join("\n")+"\n")};
$("dlcsv").onclick=function(){save("submeter-csv-"+stamp()+".txt",csv.join("\n")+"\n")};

function fmtUp(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;
 return h+":"+(m<10?"0":"")+m+":"+(x<10?"0":"")+x}
function status(j){
 $("name").textContent=j.name;$("ip").textContent=j.ip;
 $("sta").textContent=j.sta?j.staip:"down";$("up").textContent=fmtUp(j.up);
 $("heap").textContent=Math.round(j.heap/1024)+" KB";$("rssi").textContent=j.sta?j.rssi+" dBm":"-";
 $("cmdrow").style.display=j.cmd?"flex":"none";
 var c=$("chips");c.innerHTML="";
 (j.ch||[]).forEach(function(x,i){var e=document.createElement("div");e.className="chip";
  e.innerHTML='<span class="led '+x.led+'"></span>T'+(i+1)+' <span class="'+(x.relay?"onr":"off")+'">'+(x.relay?"ON":"OFF")+"</span>";
  c.appendChild(e)})}

function setConn(on){$("wsdot").className="dot"+(on?" on":"");$("wstxt").textContent=on?"live":"offline"}

// On every (re)connect the ESP32 first sends its whole ring buffer (history),
// then a status message. The history is collected in hist until that status
// arrives. On a REconnect the screen is redrawn from the history, and only
// CSV lines newer than the last one already captured are added to the CSV
// capture, so the download has no duplicates.
var partial="",hist=null,firstConn=true;
function endHistory(){
 var h=hist;hist=null;
 if(!firstConn){
  var key=csv.length?csv[csv.length-1]:null,from=0;
  if(key!==null){for(var i=h.length-1;i>=0;i--){if(h[i]===key){from=i+1;break}}}
  logEl.innerHTML="";lines=[];held=[];
  var keep=csv;csv=[];
  h.forEach(function(s,i){var c=csv.length;addLine(s);if(i<from)csv.length=c});
  csv=keep.concat(csv);if(csv.length>MAX_CSV)csv=csv.slice(csv.length-MAX_CSV);
 }else{h.forEach(addLine)}
 firstConn=false;scrollEnd()}
function connect(){
 ws=new WebSocket("ws://"+location.host+"/ws");
 ws.onopen=function(){setConn(true);partial="";hist=[]};
 ws.onclose=function(){setConn(false);if(hist)endHistory();setTimeout(connect,3000)};
 ws.onerror=function(){ws.close()};
 ws.onmessage=function(ev){var j;try{j=JSON.parse(ev.data)}catch(e){return}
  if(j.type==="status"){if(hist)endHistory();status(j);return}
  if(j.type!=="log")return;
  var parts=(partial+j.text).split("\n");partial=parts.pop();
  if(hist){hist.push.apply(hist,parts);return}
  parts.forEach(addLine);scrollEnd()}}
connect();

function sendCmd(){var v=$("cmd").value.trim();if(v&&ws&&ws.readyState===1){ws.send(v);$("cmd").value=""}}
$("send").onclick=sendCmd;
$("cmd").onkeydown=function(e){if(e.key==="Enter")sendCmd()};
</script></body></html>)rawliteral";
