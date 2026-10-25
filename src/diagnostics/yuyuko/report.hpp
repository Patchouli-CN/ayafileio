#pragma once

#include "memory_record.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>

namespace ayafileio::diagnostics::yuyuko {

inline void generate_html_report(const char* filepath = "yuyuko_report.html",
                                 const char* title = "幽幽子·内存魂簿报告") {
    std::shared_lock<std::shared_mutex> lk(g_soul_mtx);

    FILE* f = fopen(filepath, "w");
    if (!f) return;

    size_t total = g_soul_alive.size() + g_soul_released.size();
    size_t alive = g_soul_alive.size();
    size_t permanent = g_soul_permanent.size();
    size_t released = g_soul_released.size();
    size_t leaked_bytes = 0;
    for (const auto& kv : g_soul_alive) {
        if (!g_soul_permanent.count(kv.first)) leaked_bytes += kv.second.size;
    }
    uint64_t now = current_time_us();

    fprintf(f,
R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>%s</title>
<style>
  *,*::before,*::after{margin:0;padding:0;box-sizing:border-box;}
  body{font-family:'Segoe UI','PingFang SC','Noto Serif SC','Microsoft YaHei',serif;background:linear-gradient(170deg,#0d0d1a 0%%,#1a0a2e 30%%,#0d1117 70%%);color:#cdd6f4;padding:20px;min-height:100vh;overflow-x:hidden;position:relative;}
  #sakura-canvas{position:fixed;top:0;left:0;width:100%%;height:100%%;pointer-events:none;z-index:0;}
  .content-wrap{position:relative;z-index:1;}
  .header{text-align:center;padding:40px 30px;margin-bottom:24px;overflow:hidden;position:relative;background:rgba(26,10,46,0.55);backdrop-filter:blur(16px) saturate(140%%);-webkit-backdrop-filter:blur(16px) saturate(140%%);border:1px solid rgba(255,121,198,0.18);border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.4);}
  .header::before{content:'';position:absolute;top:-50%%;left:-50%%;width:200%%;height:200%%;background:radial-gradient(ellipse at center,rgba(255,121,198,0.08) 0%%,transparent 60%%);animation:headerGlow 4s ease-in-out infinite;}
  @keyframes headerGlow{0%%,100%%{transform:scale(1);opacity:0.6;}50%%{transform:scale(1.15);opacity:1;}}
  .header h1{font-size:2.4em;font-weight:300;letter-spacing:.05em;background:linear-gradient(135deg,#ff79c6,#bd93f9,#ff79c6);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;position:relative;z-index:1;animation:titleShimmer 3s ease-in-out infinite;background-size:200%% 100%%;}
  @keyframes titleShimmer{0%%,100%%{background-position:0%% 50%%;}50%%{background-position:100%% 50%%;}}
  .header .subtitle{color:rgba(205,214,244,.6);font-size:.9em;position:relative;z-index:1;margin-top:6px;}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:14px;margin-bottom:24px;}
  .card{background:rgba(26,10,46,.5);backdrop-filter:blur(12px) saturate(130%%);-webkit-backdrop-filter:blur(12px) saturate(130%%);border:1px solid rgba(255,121,198,.12);border-radius:14px;padding:22px 16px;text-align:center;transition:transform .25s,border-color .25s,box-shadow .25s;cursor:default;}
  .card:hover{transform:translateY(-3px);border-color:rgba(255,121,198,.35);box-shadow:0 8px 28px rgba(255,121,198,.15);}
  .card .number{font-size:2.3em;font-weight:700;letter-spacing:.02em;}
  .card .label{color:rgba(205,214,244,.55);margin-top:4px;font-size:.85em;}
  .card.total .number{color:#89b4fa;} .card.alive .number{color:#a6e3a1;} .card.released .number{color:#cba6f7;}
  .card.permanent .number{color:#f9e2af;} .card.leaked .number{color:#f38ba8;} .card.bytes .number{color:#fab387;font-size:1.6em;}
  .toolbar{display:flex;gap:8px;margin-bottom:18px;flex-wrap:wrap;}
  .toolbar button{padding:8px 18px;border:1px solid rgba(255,121,198,.2);border-radius:20px;background:rgba(26,10,46,.4);backdrop-filter:blur(8px);color:#cdd6f4;cursor:pointer;font-size:.88em;transition:all .25s;}
  .toolbar button:hover{background:rgba(255,121,198,.15);border-color:rgba(255,121,198,.45);}
  .toolbar button.active{background:rgba(203,166,247,.2);border-color:#cba6f7;color:#cba6f7;}
  .toolbar input{padding:8px 16px;border:1px solid rgba(255,121,198,.2);border-radius:20px;background:rgba(13,13,26,.6);backdrop-filter:blur(8px);color:#cdd6f4;font-size:.88em;flex:1;min-width:200px;outline:none;transition:border-color .25s;}
  .toolbar input:focus{border-color:rgba(255,121,198,.5);}
  .toolbar input::placeholder{color:rgba(205,214,244,.3);}
  .table-wrap{background:rgba(26,10,46,.42);backdrop-filter:blur(14px) saturate(120%%);-webkit-backdrop-filter:blur(14px) saturate(120%%);border:1px solid rgba(255,121,198,.14);border-radius:16px;overflow:hidden;}
  table{width:100%%;border-collapse:collapse;}
  th{background:rgba(30,12,50,.7);padding:13px 14px;text-align:left;font-weight:600;color:rgba(205,214,244,.65);font-size:.8em;cursor:pointer;user-select:none;letter-spacing:.03em;text-transform:uppercase;border-bottom:1px solid rgba(255,121,198,.1);}
  th:hover{color:#cdd6f4;} th .arrow{font-size:.65em;margin-left:3px;opacity:.5;}
  td{padding:11px 14px;border-bottom:1px solid rgba(255,121,198,.05);font-size:.88em;transition:background .15s;}
  tbody tr{transition:background .15s;}
  tbody tr:hover{background:rgba(255,121,198,.06)!important;}
  tbody tr.alive{border-left:3px solid #a6e3a1;}
  tbody tr.released_row{border-left:3px solid rgba(205,214,244,.15);opacity:.6;}
  .badge{display:inline-block;padding:3px 10px;border-radius:14px;font-size:.78em;font-weight:600;letter-spacing:.02em;}
  .badge-new{background:rgba(137,180,250,.15);color:#89b4fa;}
  .badge-malloc{background:rgba(249,226,175,.15);color:#f9e2af;}
  .badge-alive{background:rgba(166,227,161,.15);color:#a6e3a1;}
  .badge-released{background:rgba(205,214,244,.08);color:rgba(205,214,244,.5);}
  .badge-leak{background:rgba(243,139,168,.18);color:#f38ba8;}
  .badge-permanent{background:rgba(249,226,175,.2);color:#f9e2af;}
  .addr{font-family:'Cascadia Code','Fira Code','JetBrains Mono',monospace;color:#94e2d5;font-size:.9em;}
  .location{color:#89b4fa;cursor:pointer;} .location:hover{text-decoration:underline;color:#b4d0fb;}
  .thread-tag{color:#cba6f7;font-size:.83em;}
  .footer{text-align:center;color:rgba(205,214,244,.25);margin-top:28px;font-size:.8em;letter-spacing:.04em;}
  @keyframes petalFall{0%%{transform:translateY(-10vh) rotate(0deg) translateX(0);opacity:0;}10%%{opacity:.8;}90%%{opacity:.6;}100%%{transform:translateY(105vh) rotate(720deg) translateX(120px);opacity:0;}}
  @keyframes petalSway{0%%,100%%{transform:translateX(0);}25%%{transform:translateX(30px);}75%%{transform:translateX(-25px);}}
</style>
</head>
<body>
<canvas id="sakura-canvas"></canvas>
<div class="content-wrap">
)HTML", title);

    fprintf(f,
R"HTML(<div class="header">
  <h1>%s</h1>
  <div class="subtitle">白玉楼内存魂簿 · 幽幽子大人巡视中</div>
</div>
)HTML", title);

    fprintf(f,
R"HTML(<div class="cards">
  <div class="card total">   <div class="number">%zu</div><div class="label">总计注册</div></div>
  <div class="card alive">   <div class="number">%zu</div><div class="label">存活中</div></div>
  <div class="card released"> <div class="number">%zu</div><div class="label">已安息</div></div>
  <div class="card permanent"><div class="number">%zu</div><div class="label">常驻标记</div></div>
  <div class="card leaked">  <div class="number">%zu</div><div class="label">可能泄漏</div></div>
  <div class="card bytes">   <div class="number">%zu B</div><div class="label">泄漏内存</div></div>
</div>
)HTML", total, alive, released, permanent, alive - permanent, leaked_bytes);

    fprintf(f,
R"HTML(<div class="toolbar">
  <button class="active" onclick="filter('all',this)">全部</button>
  <button onclick="filter('alive',this)">存活</button>
  <button onclick="filter('released',this)">已安息</button>
  <button onclick="filter('permanent',this)">常驻</button>
  <input type="text" placeholder="搜索文件名、函数名或地址..." oninput="search(this.value)">
</div>
)HTML");

    fprintf(f,
R"HTML(<div class="table-wrap"><table>
<thead><tr>
  <th onclick="sortTable(0)">状态<span class="arrow"></span></th>
  <th onclick="sortTable(1)">地址<span class="arrow"></span></th>
  <th onclick="sortTable(2)">大小<span class="arrow"></span></th>
  <th onclick="sortTable(3)">来源<span class="arrow"></span></th>
  <th onclick="sortTable(4)">分配位置<span class="arrow"></span></th>
  <th onclick="sortTable(5)">线程<span class="arrow"></span></th>
  <th onclick="sortTable(6)">存续时间<span class="arrow"></span></th>
</tr></thead><tbody>
)HTML");

    auto emit_row = [&](const SoulRecord& soul, bool is_released, bool is_permanent) {
        const char* row_class = is_permanent ? "alive" : (is_released ? "released_row" : "alive");
        const char* status_badge = is_permanent ? "badge-new badge-permanent"
                                 : is_released ? "badge-released" : "badge-leak";
        const char* status_text = is_permanent ? "常驻"
                                 : is_released ? "已安息" : "存活";
        const char* source_badge = (soul.source == 0) ? "badge-new" : "badge-malloc";
        const char* source_text = (soul.source == 0) ? "new" : "malloc";
        const char* elapsed_str = format_duration_us(now - soul.timestamp);

        fprintf(f,
R"HTML(<tr class="%s" data-status="%s">
  <td><span class="badge %s">%s</span></td>
  <td><span class="addr">%p</span></td>
  <td>%zu B</td>
  <td><span class="badge %s">%s</span></td>
  <td><span class="location" title="%s:%d">%s:%d</span><br><small style="color:rgba(205,214,244,.4)">%s()</small></td>
  <td><span class="thread-tag">%s</span></td>
  <td>%s</td>
</tr>
)HTML",
            row_class, is_permanent ? "permanent" : (is_released ? "released" : "alive"),
            status_badge, status_text,
            soul.ptr, soul.size,
            source_badge, source_text,
            soul.file, soul.line, soul.file, soul.line,
            soul.func, soul.thread_name, elapsed_str);
    };
    for (const auto& kv : g_soul_alive)
        emit_row(kv.second, false, g_soul_permanent.count(kv.first));
    for (const auto& kv : g_soul_released) emit_row(kv.second, true, false);

    fprintf(f,
R"HTML(</tbody></table></div><!-- .table-wrap -->

<div class="footer">🦋 幽幽子内存魂簿报告 · 白玉楼出品</div>

<script>
(function sakura(){
  var c=document.getElementById('sakura-canvas'),x=c.getContext('2d'),w,h,p=[],t=0;
  function R(){w=c.width=window.innerWidth;h=c.height=window.innerHeight;}
  R();window.addEventListener('resize',R);
  for(var i=0;i<50;i++)p.push({x:Math.random()*w,y:Math.random()*h-h,r:3+Math.random()*6,s:.3+Math.random()*.5,o:Math.random()*9});
  function D(e){
    x.clearRect(0,0,w,h);t=e;
    for(var i=0;i<p.length;i++){var P=p[i];P.y+=P.s;P.x+=Math.sin(t/1000+P.o)*.4;if(P.y>h+20){P.y=-20;P.x=Math.random()*w;}
    x.beginPath();x.arc(P.x,P.y,P.r,0,Math.PI*2);
    x.fillStyle='rgba(255,182,193,'+(.35+.25*Math.sin(t/800+P.o)).toFixed(2)+')';x.fill();}
    requestAnimationFrame(D);
  }
  requestAnimationFrame(D);
})();
function filter(s,b){
  document.querySelectorAll('.toolbar button').forEach(function(x){x.classList.remove('active');});
  b.classList.add('active');
  document.querySelectorAll('tbody tr').forEach(function(r){
    r.style.display=s==='all'?'':r.dataset.status===s?'':'none';
  });
}
function search(q){
  q=q.toLowerCase();
  document.querySelectorAll('tbody tr').forEach(function(r){
    r.style.display=r.textContent.toLowerCase().includes(q)?'':'none';
  });
  if(!q){var b=document.querySelectorAll('.toolbar button');if(b.length)b[0].classList.add('active');}
}
var d={};
function sortTable(c){
  var t=document.querySelector('tbody'),r=Array.from(t.querySelectorAll('tr'));
  d['c'+c]=!(d['c'+c]||false);var o=d['c'+c]?1:-1;
  r.sort(function(a,b){var va=a.cells[c].textContent.trim(),vb=b.cells[c].textContent.trim();var na=parseFloat(va),nb=parseFloat(vb);if(!isNaN(na)&&!isNaN(nb))return(na-nb)*o;return va.localeCompare(vb)*o;});
  document.querySelectorAll('th .arrow').forEach(function(a){a.textContent='';});
  document.querySelectorAll('th')[c].querySelector('.arrow').textContent=o>0?'▲':'▼';
  r.forEach(function(x){t.appendChild(x);});
}
</script>
</body>
</html>
)HTML");

    fclose(f);

    if (!detail::g_async_mode) {
        fprintf(stderr, "[Yuyuko] HTML 报告已生成: %s\n", filepath);
    }
}

} // namespace ayafileio::diagnostics::yuyuko
