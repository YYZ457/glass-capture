'use strict';
const $ = id => document.getElementById(id);
let token = '', lastSeq = 0, latest = {}, files = [], sourcePath = '', polling = false;
let toastTimer, serviceDisconnected=false;
function toast(message, error=false) {
  $('toast').textContent = message; $('toast').className = error ? 'error' : ''; $('toast').hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').hidden = true, error ? 12000 : 4500);
}
async function api(path, data) {
  const response = await fetch('/api/' + path, data === undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json','X-SG-Token':token},body:JSON.stringify(data)});
  const result = await response.json(); if (!response.ok) throw new Error(result.error || '请求失败'); return result;
}
function showView(name) {
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.id === name));
  document.querySelectorAll('.nav').forEach(n => n.classList.toggle('active', n.dataset.view === name));
  $('crumb').textContent = {'defect-view':'离线检测',control:'采集控制',source:'核心代码',history:'历史与验证',guide:'操作指南'}[name];
}
async function showSource(id) {
  try {
    const data = await api('source?id=' + encodeURIComponent(id));
    showView('source'); sourcePath = data.path;
    $('source-title').textContent = files.find(f => f.id === id)?.title || id;
    $('source-path').textContent = data.path; $('source-guide').textContent = data.guide;
    const fragment = document.createDocumentFragment();
    data.text.split('\n').forEach((text, i) => {
      const line = document.createElement('span'); line.className = 'code-line' + (i+1 === data.line ? ' target' : '');
      const num = document.createElement('span'); num.className = 'line-num'; num.textContent = i+1;
      line.append(num, document.createTextNode(text.replace(/\r$/, '') || ' ')); fragment.append(line);
    });
    $('code').replaceChildren(fragment);
    document.querySelectorAll('.source-file').forEach(el => el.classList.toggle('active', el.dataset.id === id));
    $('code-scroll').scrollTop = Math.max(0, (data.line - 4) * 21.6);
  } catch(e) { toast(e.message, true); }
}
async function refreshPorts() {
  try {
    const previous = $('port').value, ports = await api('ports');
    $('port').replaceChildren();
    const placeholder = new Option(ports.length ? '选择 J83 对应串口' : '未发现串口，可离线浏览', ''); $('port').add(placeholder);
    ports.sort((a,b) => Number(b.suggested)-Number(a.suggested));
    ports.forEach(p => $('port').add(new Option(p.device + ' · ' + p.description, p.device)));
    if (ports.some(p => p.device === previous)) $('port').value = previous;
    $('port-note').textContent = ports.some(p => p.suggested) ? '发现 Silicon Labs 串口。请选择 Linux 控制台对应接口，不会自动连接。' : '未发现 CP2108 板卡串口。你仍可查看源码、历史记录与本地图像。';
  } catch(e) { toast(e.message, true); }
}
function update(s) {
  latest = s;
  $('connection-chip').textContent = s.connected ? s.port + (s.shell_ready ? ' · 终端就绪' : ' · 等待终端') : '未连接硬件';
  $('connection-chip').className = 'chip' + (s.connected ? ' online' : '');
  $('connect').disabled = s.connected || s.busy; $('disconnect').disabled = !s.connected || s.busy;
  $('port').disabled = s.connected; $('wake').disabled = !s.connected || s.busy;
  $('interrupt').disabled = !s.busy; $('refresh').disabled = s.connected;
  $('shutdown').disabled = s.busy;
  $('replay').disabled = s.busy;
  $('start-stream').disabled = !s.connected || !s.shell_ready || !s.identity_ok || s.busy;
  $('stream-seconds').disabled=s.busy; $('stream-processor').disabled=s.busy;
  const stream=s.stream || {}, result=stream.result || {}, ended=stream.ended;
  const replay=stream.channel==='recorded-replay';
  const unknown=stream.status==='unknown';
  const streamError=stream.status==='error'||[stream,ended,result].some(record=>record&&['error','source_error','processing_error','output_error','stop_error'].some(key=>record[key]!==undefined&&record[key]!==null&&record[key]!==0&&record[key]!=='0'&&record[key]!==''&&record[key]!==false));
  $('stream-badge').textContent=unknown?'停止未确认':streamError?'会话异常':replay?'离线回放 · 合成数据':stream.status==='running'?'连续接收中':stream.status==='ended'?'会话结束':'等待验证';
  $('stream-badge').className='chip'+(unknown||streamError?' amber':'');
  $('stream-count').textContent=unknown?'—':ended?.processed ?? result.processed_total ?? '—';
  $('stream-block').textContent=!unknown&&result.sequence ? `${result.block_id} / ${result.width} × ${result.height}` : '—';
  $('stream-gray').textContent=!unknown&&result.pixel_count ? `${result.min}–${result.max} / ${(Number(result.sum)/Number(result.pixel_count)).toFixed(2)}` : '—';
  $('stream-decision').textContent=unknown?'停止未确认，结果不可确认':streamError?'会话异常，结果需核查':result.decision==='pass'?'算法报告合格（需验证）':result.decision==='fail'?'算法报告缺陷（需验证）':result.processor==='candidates'&&result.sequence?`本块候选 ${result.above_threshold} 个（未判定）`:'未作缺陷判定';
  if(unknown) $('stream-summary').textContent='尚未确认连续接收程序已停止。请检查终端日志与板上进程；之前的算法结论不能作为本次结果。';
  else if(ended) $('stream-summary').textContent=`${replay?'离线回放结果':'本次会话'}：接收 ${ended.received ?? '—'} 块，处理 ${ended.processed ?? '—'} 块；队列峰值 ${ended.queue_peak ?? '—'}，队列满 ${ended.queue_full_events ?? '—'} 次，编号不连续 ${ended.id_discontinuities ?? '—'} 次。错误：总计 ${ended.error ?? '未报告'} / 输入 ${ended.source_error ?? '未报告'} / 处理 ${ended.processing_error ?? '未报告'} / 输出 ${ended.output_error ?? '未报告'} / 停止 ${ended.stop_error ?? '未报告'}。`;
  else if(streamError) $('stream-summary').textContent='会话报告输入、处理、输出或停止错误，请检查终端日志；当前结果需核查。';
  else if(result.sequence) $('stream-summary').textContent=`${replay?'离线合成记录，不代表板上运行':'板上输出，仍需核实物理行连续性'}。本块起始位置为累计收到的第 ${result.received_line_start} 行；它不是编码器位置。界面按周期采样统计，每块仍参与处理。`;
  else $('stream-summary').textContent='等待本次连续接收结果。尚无可确认的质量判定。';
  document.querySelectorAll('[data-action]').forEach(b => {
    const basic = ['probe','dmesg','fetch_last'].includes(b.dataset.action);
    b.disabled = !s.connected || !s.shell_ready || s.busy || (!basic && !s.identity_ok);
  });
  $('net-setup').disabled = !s.connected || !s.shell_ready || s.busy || !s.identity_ok;
  $('net-state').textContent = s.net && s.net.pc_ip ? `网口已通：板子 → 电脑 ${s.net.pc_ip}:${s.net.port}。回传和连续候选走网口。` : '网口未配置：回传走 USB 串口。先「从 SD 加载程序」，再填 IP 测试。';
  const fields = s.checks.fields || {}, p = s.checks.preflight, live = s.connected;
  $('linux-state').textContent = fields.KERNEL ? (live ? '已读内核 · ' : '断开前 · ') + fields.KERNEL : '等待检查';
  $('pci-state').textContent = fields.VENDOR ? (live ? '' : '断开前 · ') + fields.VENDOR + ':' + fields.DEVICE : '等待检查';
  const trusted = p?.known && p.reads_confirmed;
  $('clock-state').textContent = trusted ? (live ? '' : '断开前 · ') + (p.clocks & 1 ? '上次检查已锁定' : '上次检查未锁定') : '等待信号检查';
  $('dma-state').textContent = s.last_frame ? (s.last_frame.local_path ? '已存到电脑' : '已确认板上保存图像') : '尚未确认图像接收';
  $('ddr').textContent = trusted ? (p.ddr === 0 ? '0 · 条件满足' : p.ddr === 2 ? '2 · DDR 校准完成' : String(p.ddr) + ' · 需核实') : '未确认读取';
  $('clock').textContent = trusted ? (p.clocks & 1 ? '锁定' : '未锁定') + ' / 0x' + p.clocks.toString(16) : '未确认读取';
  $('format').textContent = p?.passed && p.config ? 'Base / Mono8' : 'Mono8 (8-bit 灰度)';
  if ($('cl-mode')) $('cl-mode').textContent = p?.config ? (p.config.cl === 0 ? 'Base 模式 (单缆 Port 0)' : 'Medium/Full 模式') : 'Base 模式 (单缆 Port 0)';
  if ($('camera-geo')) $('camera-geo').textContent = s.last_frame?.width ? `${s.last_frame.width} × ${s.last_frame.height} (16K 基础)` : '4096 × 2000 (16K 线扫 / 4K ROI)';
  if ($('pci-hw')) $('pci-hw').textContent = fields.VENDOR ? `${fields.VENDOR}:${fields.DEVICE} · sg3000_rx` : '0x4c54:0x3000 · C2H0';
  $('prepared').textContent = s.checks.prepared ? '已加载到板上 /tmp' : '尚未由中控台加载';
  if ($('local-dir') && s.captures) { $('local-dir').textContent = s.captures; $('local-dir').title = s.captures; }
  $('observed-at').textContent = p?.at || s.checks.at || '尚无本次实测';
  $('terminal-status').textContent = s.busy ? '操作进行中' : s.connected ? (s.shell_ready ? '终端就绪' : '等待 root 提示符') : '串口未连接';
  if (s.job) {
    const msg = $('state-message'); msg.replaceChildren();
    const icon = document.createElement('div'); icon.className='state-icon'; icon.textContent = s.job.status === 'running' ? '◌' : s.job.status === 'success' ? '✓' : '!';
    const title=document.createElement('h3'); title.textContent = s.job.status === 'running' ? '正在执行，请看下方日志' : s.job.status === 'success' ? '操作完成' : '停在这里，先查看原因';
    const desc=document.createElement('p'); desc.textContent=s.job.message; msg.append(icon,title,desc);
  }
  if (s.last_frame) {
    $('board-frame').hidden = false;
    let text = '';
    if (s.last_frame.local_path) {
      const mb = s.last_frame.size_bytes ? ' (' + (s.last_frame.size_bytes / 1024 / 1024).toFixed(2) + ' MB)' : '';
      text = '<b>电脑保存：</b>' + s.last_frame.local_path + mb + '<br><b>板上文件：</b>' + s.last_frame.path + (s.last_frame.location && s.last_frame.location.includes('sd') ? ' (已同步备份到 SD 卡)' : '');
    } else {
      text = '<b>板上文件：</b>' + s.last_frame.path + '。位于内存，断电会丢失；尚未传回电脑。';
    }
    $('board-frame').innerHTML = text;
    if ($('view-latest-btn')) $('view-latest-btn').disabled = false;
    if (window._displayedFrame !== s.last_frame.name && s.last_frame.name) {
      window._displayedFrame = s.last_frame.name;
      loadCapturedPreview(s.last_frame.name, s.last_frame.width || 4096, s.last_frame.height || 2000, s.last_frame.size_bytes);
    }
  } else {
    $('board-frame').hidden = true;
    if ($('view-latest-btn')) $('view-latest-btn').disabled = true;
  }
  for(const e of s.events) {
    const text = e.kind === 'rx' ? e.text : '\n[' + e.time.slice(11) + ' / ' + e.kind + '] ' + e.text + '\n';
    $('terminal').append(document.createTextNode(text)); lastSeq = Math.max(lastSeq,e.seq);
  }
  if ($('terminal').textContent.length > 180000) $('terminal').textContent = '[界面仅显示最近日志；完整记录可导出]\n' + $('terminal').textContent.slice(-150000);
  if($('autoscroll').checked && s.events.length) $('terminal').scrollTop = $('terminal').scrollHeight;
}
async function poll() {
  if(polling) return; polling=true;
  try { const state=await api('state?after=' + lastSeq); if(serviceDisconnected){const bootstrap=await api('bootstrap');token=bootstrap.token;serviceDisconnected=false;} update(state); $('service').textContent='本机服务在线'; $('service').className='service ok'; }
  catch(e) { if(!serviceDisconnected){serviceDisconnected=true;document.dispatchEvent(new Event('workbench-disconnected'));} $('stream-badge').textContent='连接中断 · 状态未确认'; $('stream-badge').className='chip amber'; $('stream-decision').textContent='状态未确认，结果不可确认'; $('stream-summary').textContent='本机服务连接中断，请恢复连接后重新核实会话状态。'; $('service').textContent='本机服务断开 · 请重启入口'; $('service').className='service error'; document.querySelectorAll('[data-action],#wake,#connect,#disconnect,#interrupt,#start-stream,#replay').forEach(b=>b.disabled=true); }
  finally {polling=false;}
}
async function post(path, data={}) {try {await api(path,data); await poll();}catch(e){toast(e.message,true);}}
document.querySelectorAll('.nav').forEach(b => b.onclick=()=>showView(b.dataset.view));
document.querySelectorAll('[data-go]').forEach(b => b.onclick=()=>showView(b.dataset.go));
document.querySelectorAll('[data-file]').forEach(b => b.onclick=()=>showSource(b.dataset.file));
document.querySelectorAll('[data-action]').forEach(b => b.onclick=()=>post('action',{action:b.dataset.action}));
$('connect').onclick=()=>post('connect',{port:$('port').value}); $('disconnect').onclick=()=>post('disconnect');
$('refresh').onclick=refreshPorts; $('wake').onclick=()=>post('wake'); $('interrupt').onclick=()=>post('interrupt');
$('shutdown').onclick=async()=>{try{await api('shutdown',{});toast('本机服务已关闭；下次双击桌面入口即可重新启动。');}catch(e){toast(e.message,true);}};
$('replay').onclick=()=>post('action',{action:'replay'});
try { $('net-pc').value = localStorage.getItem('sg-net-pc') || ''; $('net-board').value = localStorage.getItem('sg-net-board') || ''; } catch {}
$('net-setup').onclick=()=>{ const pc=$('net-pc').value.trim(), board=$('net-board').value.trim();
  try { localStorage.setItem('sg-net-pc', pc); localStorage.setItem('sg-net-board', board); } catch {}
  post('action',{action:'network',options:{pc_ip:pc,board_ip:board}}); };
$('start-stream').onclick=()=>post('action',{action:'stream',options:{seconds:Number($('stream-seconds').value),processor:$('stream-processor').value}});
$('copy-path').onclick=async()=>{try{await navigator.clipboard.writeText(sourcePath);toast('源码完整路径已复制');}catch{toast(sourcePath,true);}};
$('export').onclick=async()=>{try{const r=await fetch('/api/log');if(!r.ok)throw new Error('导出失败');const blob=await r.blob(),a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='sg3000-session-'+new Date().toISOString().replace(/[:.]/g,'-')+'.jsonl';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);}catch(e){toast(e.message,true);}};
async function loadCapturedPreview(name, w, h, size) {
  try {
    const img = new Image();
    img.onload = () => {
      const canvas = $('image');
      const maxW = 2048, maxH = 1024;
      const scale = Math.min(1, maxW / (w || img.width), maxH / (h || img.height));
      const cw = Math.max(1, Math.round((w || img.width) * scale));
      const ch = Math.max(1, Math.round((h || img.height) * scale));
      canvas.width = cw;
      canvas.height = ch;
      const ctx = canvas.getContext('2d');
      ctx.drawImage(img, 0, 0, cw, ch);
      canvas.hidden = false;
      $('image-empty').hidden = true;
      const mb = size ? ' · ' + (size / 1024 / 1024).toFixed(2) + ' MB' : '';
      $('image-info').textContent = `实机采集 / ${name} · ${w || img.width} × ${h || img.height}${mb} · 已存入 D:\\SG3000_Captures`;
    };
    img.src = '/api/captures/preview?name=' + encodeURIComponent(name) + '&t=' + Date.now();
  } catch(e) {
    console.error(e);
  }
}
if ($('view-latest-btn')) {
  $('view-latest-btn').onclick = () => {
    if (latest.last_frame?.name) {
      loadCapturedPreview(latest.last_frame.name, latest.last_frame.width || 4096, latest.last_frame.height || 2000, latest.last_frame.size_bytes);
    }
  };
}
function parsePGM(bytes) {
  let i=0; const whitespace=c=>[9,10,13,32].includes(c);
  function word(){while(i<bytes.length){if(whitespace(bytes[i])){i++;continue;}if(bytes[i]===35){while(i<bytes.length && bytes[i]!==10)i++;continue;}break;}let out='';while(i<bytes.length&&!whitespace(bytes[i])){if(out.length>32)throw new Error('PGM 头部过长');out+=String.fromCharCode(bytes[i++]);}return out;}
  if(word()!=='P5')throw new Error('仅支持 P5 二进制灰度 PGM。');
  const w=Number(word()),h=Number(word()),max=Number(word());
  if(!Number.isInteger(w)||!Number.isInteger(h)||w<1||h<1||w>32768||h>32768||w*h>32000000||max!==255)throw new Error('需要 8 位 Mono8，图像不超过 3200 万像素，每边不超过 32768。');
  if(!whitespace(bytes[i]))throw new Error('PGM 头部缺少分隔符');
  if(bytes[i]===13 && bytes[i+1]===10)i+=2;else i++;
  if(bytes.length-i!==w*h)throw new Error('像素长度与宽高不一致，可能是不完整图像。');
  return {w,h,pixels:bytes.subarray(i)};
}
$('pgm').onchange=async e=>{
  try{const file=e.target.files[0];if(!file)return;if(file.size>33000000)throw new Error('本地查看器限制为 33 MB。');const img=parsePGM(new Uint8Array(await file.arrayBuffer()));
    // Downsample for display only. Keep summary statistics over all source pixels.
    const scale=Math.min(1,2048/img.w,2048/img.h),w=Math.max(1,Math.round(img.w*scale)),h=Math.max(1,Math.round(img.h*scale));
    const canvas=$('image');canvas.width=w;canvas.height=h;const ctx=canvas.getContext('2d'),data=ctx.createImageData(w,h);let min=255,max=0,sum=0;
    for(const v of img.pixels){min=Math.min(min,v);max=Math.max(max,v);sum+=v;}
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){const v=img.pixels[Math.min(img.h-1,Math.floor(y/scale))*img.w+Math.min(img.w-1,Math.floor(x/scale))],o=(y*w+x)*4;data.data[o]=data.data[o+1]=data.data[o+2]=v;data.data[o+3]=255;}
    ctx.putImageData(data,0,0);canvas.hidden=false;$('image-empty').hidden=true;$('image-info').textContent=`本地导入 / ${file.name} · ${img.w} × ${img.h} · min ${min} / max ${max} / mean ${(sum/img.pixels.length).toFixed(2)} · 非硬件验证结果`;
  }catch(err){toast(err.message,true);}finally{e.target.value='';}
};
async function init(){try{const data=await api('bootstrap');token=data.token;files=data.files;$('project-root').textContent=data.root;for(const f of files){const b=document.createElement('button');b.className='source-file';b.dataset.id=f.id;const title=document.createElement('span');title.textContent=f.title;const small=document.createElement('small');small.textContent=f.path;b.append(title,small);b.onclick=()=>showSource(f.id);$('source-files').append(b);}await showSource('architecture');showView('defect-view');document.dispatchEvent(new Event('workbench-ready'));await refreshPorts();await poll();setInterval(poll,700);}catch(e){toast('中控台初始化失败：'+e.message,true);}}
init();
