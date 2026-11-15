'use strict';
/* GlassCapture 中控台前端（1.1）。
 * 每 700 毫秒问一次 /api/state，update(s) 按状态刷新界面；控件只负责发请求。
 * 不含硬件逻辑：允许做什么、参数范围，后端（transport.py）和板上程序都会再检查一遍。 */
const $ = id => document.getElementById(id);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
let token = '', lastSeq = 0, latest = {}, files = [], sourcePath = '';
let toastTimer, serviceDisconnected = false, dismissedJob = '', displayedFrame = '', liveShown = '';
let initRunning = false, stopRequested = false, lastJobKey = '', modalCancel = null, hoverX = null;
/* 内容没变就不重建节点，否则按钮会在你点下去的瞬间被换掉。 */
const signatures = new Map();
function changed(key, value) { const text = typeof value === 'string' ? value : JSON.stringify(value); if (signatures.get(key) === text) return false; signatures.set(key, text); return true; }
function store(key, value) { try { localStorage.setItem(key, value); } catch {} }
function recall(key, fallback) { try { return localStorage.getItem(key) ?? fallback; } catch { return fallback; } }

function toast(message, error = false) {
  $('toast').textContent = message; $('toast').className = error ? 'error' : ''; $('toast').hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').hidden = true, error ? 9000 : 3500);
}
async function api(path, data) {
  const response = await fetch('/api/' + path, data === undefined ? {} : {method: 'POST', headers: {'Content-Type': 'application/json', 'X-SG-Token': token}, body: JSON.stringify(data)});
  const result = await response.json(); if (!response.ok) throw new Error(result.error || '请求失败'); return result;
}
async function post(path, data = {}) { try { await api(path, data); await poll(); return true; } catch (e) { toast(e.message, true); return false; } }
const act = (action, options) => post('action', options ? {action, options} : {action});

/* 确认对话框 */
function ask(title, text, okLabel = '确定', danger = false) {
  return new Promise(resolve => {
    $('modal-title').textContent = title; $('modal-text').textContent = text; $('modal-ok').textContent = okLabel;
    $('modal-ok').className = 'btn ' + (danger ? 'danger' : 'primary'); $('modal').hidden = false; $('modal-ok').focus();
    const done = value => { $('modal').hidden = true; modalCancel = null; resolve(value); };
    $('modal-ok').onclick = () => done(true); $('modal-cancel').onclick = () => done(false); modalCancel = () => done(false);
  });
}

/* ---------- 工作区、弹出面板、设置 ---------- */
const VIEWS = ['acquire', 'run', 'offline'];
function showView(name) {
  if (!VIEWS.includes(name)) return;
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.id === name));
  document.querySelectorAll('.mode').forEach(n => n.classList.toggle('active', n.dataset.view === name));
  document.body.dataset.view = name; store('sg-view', name);
  if (name === 'acquire') requestAnimationFrame(drawAll);
}
function showPane(id) {
  document.querySelectorAll('.pane').forEach(p => p.classList.toggle('active', p.id === id));
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.pane === id));
  if (id === 'profile-pane') requestAnimationFrame(drawProfile);
}
const POPS = {'device-pop': 'device-btn', 'cam-menu': 'cam-more'};
function closePops(except) { for (const [pop, trigger] of Object.entries(POPS)) if (pop !== except) { $(pop).hidden = true; $(trigger).setAttribute('aria-expanded', 'false'); } }
function togglePop(pop) {
  const open = $(pop).hidden; closePops(); if (!open) return;
  const r = $(POPS[pop]).getBoundingClientRect(), box = $(pop);
  box.hidden = false; $(POPS[pop]).setAttribute('aria-expanded', 'true');
  box.style.top = Math.round(r.bottom + 6) + 'px';
  box.style.right = Math.max(8, Math.round(window.innerWidth - r.right)) + 'px';
  if (pop === 'device-pop' && !latest.connected) refreshPorts();
}
function openSettings(section = 'sec-card') {
  closePops(); $('settings').hidden = false;
  document.querySelectorAll('.set-sec').forEach(s => s.classList.toggle('active', s.id === section));
  document.querySelectorAll('.set-nav button').forEach(b => b.classList.toggle('active', b.dataset.sec === section));
}
function setLog(open) {
  $('logdock').hidden = !open; $('log-toggle').setAttribute('aria-expanded', String(open));
  if (open) $('terminal').scrollTop = $('terminal').scrollHeight;
  requestAnimationFrame(drawAll);
}
function applyTheme() {
  const pref = recall('sg-theme', 'dark');
  const dark = pref === 'system' ? matchMedia('(prefers-color-scheme: dark)').matches : pref !== 'light';
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  document.querySelectorAll('#theme-seg button').forEach(b => b.classList.toggle('on', b.dataset.themeValue === pref));
  drawAll();
}

/* ---------- 相机属性：一张表描述每个参数怎么显示、怎么改 ---------- */
const OBJECT_PIXEL_UM = 80000 / 16384;
const LOCK_SVG = '<svg viewBox="0 0 16 16"><rect x="3.5" y="7" width="9" height="6.5" rx="1.2"/><path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2"/></svg>';
const CAM_PARAMS = [
  {group: 'imaging', name: 'ExposureTime', label: '曝光时间', unit: 'µs', kind: 'float', digits: 2, hint: '不能超过一行的时间；上限随行频变化'},
  {group: 'imaging', name: 'LineRate', label: '行频', unit: 'Hz', kind: 'float', digits: 1, hint: '内触发时可改。行频 × 4.88 µm = 玻璃速度', extra: v => `≈ ${(v * OBJECT_PIXEL_UM / 1000).toFixed(1)} mm/s`},
  {group: 'imaging', name: 'DigitalShift', label: '数字位移', kind: 'enum', options: [[0, '×1'], [1, '×2'], [2, '×4'], [3, '×8'], [4, '×16']], hint: '取内部数据的哪 8 位；×2 相当于亮一倍'},
  {group: 'imaging', name: 'DigitalGain', label: '数字增益', unit: '×', kind: 'float', digits: 3, min: 0.1, max: 15.99, hint: '0.10 – 15.99；只是乘法，不提高信噪比'},
  {group: 'advanced', name: 'BlackLevel', label: '黑电平', kind: 'int', hint: '整数，范围由相机给出'},
  {group: 'advanced', name: 'TDIStage', label: 'TDI 级数', kind: 'enum', options: [[0, '1 级'], [1, '2 级']], hint: '2 级：两行叠加，信号翻倍；行频必须和玻璃速度对上'},
  {group: 'advanced', name: 'TDIMode', label: 'TDI 方式', kind: 'enum', options: [[0, '求和'], [1, '平均']], hint: '求和更亮，平均噪声更低'},
  {group: 'advanced', name: 'AnalogGain', label: '模拟增益', kind: 'enum', options: [[0, '1.25×'], [1, '1.75×'], [2, '2×'], [3, '3×'], [4, '4×']], lockedHint: '相机 XML 规定当前锁定；用数字位移或 TDI 代替'},
  {group: 'advanced', name: 'TestPattern', label: '测试图', kind: 'enum', options: [[0, '关闭'], [1, '全黑'], [2, '全白'], [3, '水平渐变'], [5, '移动渐变']], hint: '相机自己生成的图，用来检查链路有没有丢位'},
  {group: 'advanced', name: 'ClConfiguration', label: 'CL 配置', kind: 'enum', options: [[0, 'Base（单线）'], [3, 'EightyBit（双线）']], hint: '相机和采集卡必须一致'},
  {group: 'advanced', name: 'DeviceTapGeometry', label: 'Tap 数', kind: 'enum', options: [[2, '2 Tap'], [3, '3 Tap']], lockedHint: '只有 Base + Mono8 时能改'},
];
const CAM_INFO = [
  ['Width', '宽度', p => `${p.raw} px`], ['PixelFormat', '像素格式', p => p.raw === 0x01080001 ? 'Mono8' : '0x' + p.raw.toString(16)],
  ['ClConfiguration', '输出', (p, all) => `${p.raw === 0 ? 'Base' : p.raw === 3 ? 'EightyBit' : p.raw} · ${all.DeviceTapGeometry?.raw ?? '?'} Tap`],
  ['MeasuredLineRate', '实测行频', p => `${p.value?.toFixed(1)} Hz`], ['LineTriggerMode', '行触发', p => p.raw ? '外触发' : '内触发'],
  ['FFCEnable', '平场校正', p => p.raw ? '开' : '关'], ['Temperature', '温度', p => `${p.value?.toFixed(1)} ℃`],
];
function camParam(name) { return latest.camera?.params?.[name]; }
function currentValue(spec, p) { return !p || p.raw === undefined ? undefined : spec.kind === 'enum' ? p.raw : spec.kind === 'int' ? (p.value ?? p.raw) : p.value; }
function buildCameraRows() {
  for (const spec of CAM_PARAMS) {
    const row = el('div', 'prop'); row.dataset.param = spec.name;
    const label = el('label', '', spec.label), note = el('small'); label.append(note);
    label.title = (spec.hint ? spec.hint + '\n' : '') + spec.name;
    const ctl = el('div', 'ctl');
    let input;
    if (spec.kind === 'enum') { input = el('select'); spec.options.forEach(([v, t]) => input.add(new Option(t, v))); }
    else { input = el('input'); input.type = 'number'; input.step = spec.kind === 'int' ? '1' : 'any'; }
    input.id = 'cam-' + spec.name; label.htmlFor = input.id; input.title = '回车或离开输入框即写入相机';
    const lock = el('span'); lock.innerHTML = LOCK_SVG; lock.className = 'lock'; lock.hidden = true;
    ctl.append(input); if (spec.unit) ctl.append(el('span', 'unit', spec.unit)); ctl.append(lock);
    row.append(label, ctl); $('cam-' + spec.group).append(row);
    input.addEventListener('change', () => commitParam(spec));
    Object.assign(spec, {row, note, input, lock});
  }
}
function revertParam(spec) { const v = currentValue(spec, camParam(spec.name)); spec.input.value = v === undefined ? '' : v; }
function commitParam(spec) {
  const p = camParam(spec.name), raw = spec.input.value;
  let value = Number(raw);
  if (!uiReady()) { revertParam(spec); return toast('设备忙或未就绪，未写入。', true); }
  if (raw === '' || !Number.isFinite(value)) { revertParam(spec); return toast('请输入数值。', true); }
  if (spec.kind !== 'float') value = Math.round(value);
  const now = currentValue(spec, p);
  if (now !== undefined && Math.abs(value - now) < 1e-9) return;
  const min = p?.min ?? spec.min, max = p?.max ?? spec.max;
  if (min !== undefined && max !== undefined && (value < min || value > max)) { revertParam(spec); return toast(`${spec.label}超出范围 ${min} – ${max}。`, true); }
  spec.row.classList.add('pending');
  act('camera_set', {name: spec.name, value});
}
function updateCamera(s, ready) {
  const cam = s.camera || {}, params = cam.params || {};
  $('cam-at').textContent = cam.at ? `读取于 ${cam.at.slice(11)}${cam.partial ? ' · 不完整' : ''}` : '未读取';
  if (changed('cam-info', cam.at || '')) $('cam-info').replaceChildren(...CAM_INFO.map(([name, label, show]) => {
    const p = params[name], cell = el('div'); cell.append(el('dt', '', label), el('dd', '', !p ? '—' : p.error !== undefined ? '读不出' : show(p, params))); return cell;
  }));
  const settling = s.busy && s.job?.action === 'camera_set';
  for (const spec of CAM_PARAMS) {
    const p = params[spec.name], locked = !!p?.locked;
    let note = '';
    if (locked) note = '锁定';
    else if (spec.extra && p?.value) note = spec.extra(p.value);
    else if (p && p.min !== undefined && p.max !== undefined) note = `${p.min} – ${p.max}`;
    spec.note.textContent = note;
    spec.row.classList.toggle('locked', locked); spec.lock.hidden = !locked;
    spec.lock.title = locked ? (spec.lockedHint || '相机当前锁定此项') : '';
    spec.input.disabled = !ready || locked || !p;
    if (!settling) spec.row.classList.remove('pending');
    if (document.activeElement !== spec.input && p && p.raw !== undefined && spec.shown !== cam.at) { revertParam(spec); spec.shown = cam.at; }
  }
  const pattern = params.TestPattern?.raw;
  $('adv-state').textContent = pattern ? '测试图开启' : '';
  // 平场校正
  const ffcOn = params.FFCEnable?.raw, set = params.FFCUserSetSelector?.raw, factory = params.FFCFactory?.raw, flip = params.ReverseX?.raw;
  const known = set !== undefined && factory !== undefined && flip !== undefined;
  const calLocked = known && ((factory === 0 && set === 0) || flip === 1);
  $('ffc-state').textContent = ffcOn === undefined ? '—' : `${ffcOn ? '开' : '关'} · ${set === 0 ? '出厂组' : set === undefined ? '?' : '用户 ' + set}`;
  $('ffc-lock').hidden = !calLocked;
  $('ffc-lock').textContent = flip === 1 ? '相机开着水平镜像（ReverseX），此时不能校正。' : '出厂组不能重新校正，先选用户组 1–3。';
  if (cam.at && $('ffc-set').dataset.shown !== cam.at) {
    if (set !== undefined) $('ffc-set').value = set;
    if (ffcOn !== undefined) $('ffc-enable').checked = !!ffcOn;
    if (params.FFCCalTargetMode?.raw !== undefined) $('ffc-mode').value = params.FFCCalTargetMode.raw;
    if (params.FFCCalTargetValue?.value !== undefined && params.FFCCalTargetMode?.raw === 2) $('ffc-target').value = params.FFCCalTargetValue.value;
    if (params.FFCSampleFrames?.raw) $('ffc-frames').value = params.FFCSampleFrames.raw;
    $('ffc-set').dataset.shown = cam.at;
  }
  const have = ffcOn !== undefined;
  for (const id of ['ffc-set', 'ffc-enable', 'ffc-mode', 'ffc-frames', 'ffc-load']) $(id).disabled = !ready || !have;
  $('ffc-target').disabled = !ready || !have || $('ffc-mode').value !== '2';
  $('ffc-dark').disabled = $('ffc-bright').disabled = !ready || !have || calLocked;
  $('cam-refresh').disabled = !(s.connected && s.shell_ready && s.identity_ok && !s.busy);
}

/* ---------- 初始化清单 ---------- */
function buildSteps(s) {
  const params = s.camera?.params || {}, card = s.checks?.card || {}, pre = s.checks?.preflight, fields = s.checks?.fields || {};
  const base3 = params.ClConfiguration?.raw === 0 && params.DeviceTapGeometry?.raw === 3 && card.CameraWidth === 16384;
  const shell = s.connected && s.shell_ready, idle = !s.busy && !initRunning;
  return [
    {title: '连接串口', done: s.connected, text: s.connected ? s.port : '选择串口后点「连接」'},
    {title: '唤醒终端', done: shell, text: shell ? 'root 提示符' : '等待 Linux 提示符', run: () => post('wake'), can: s.connected && idle},
    {title: '检查板卡', done: !!s.identity_ok, text: s.identity_ok ? `内核 ${fields.KERNEL || ''}` : '板型、内核、PCIe 设备', run: () => act('probe'), can: shell && idle},
    {title: '加载程序与驱动', done: !!s.checks?.prepared, text: s.checks?.prepared ? '已加载到板上内存' : '从 SD 卡只读拷贝并校验', run: () => act('prepare'), can: shell && idle && s.identity_ok},
    {title: '采集卡体检', done: !!pre?.passed, fail: pre?.known && !pre.passed, text: pre?.passed ? `预检通过 · ${card.LineRateHz ? card.LineRateHz + ' Hz' : '行频未知'}` : pre?.known ? '预检未过（相机可能仍是双线）' : '驱动、时钟、卡参数', run: () => act('status'), can: shell && idle && s.identity_ok},
    {title: '单线模式', done: base3, text: base3 ? 'Base · 3 Tap · 16384' : '相机 Base 3 Tap + 卡宽 16384', run: () => act('camera_preset'), can: shell && idle && s.identity_ok},
    {title: '读取相机参数', done: !!s.camera?.at, text: s.camera?.at ? `读取于 ${s.camera.at.slice(11)}` : '只读', run: () => act('camera_get'), can: shell && idle && s.identity_ok},
  ];
}
function updateSteps(s) {
  const steps = buildSteps(s), current = steps.findIndex(x => !x.done), all = current < 0;
  $('init').disabled = !s.connected || s.busy || initRunning || all;
  $('init').textContent = initRunning ? '正在初始化…' : all ? '已就绪' : '一键初始化';
  $('init-progress').textContent = `${steps.filter(x => x.done).length} / ${steps.length}`;
  if (!changed('steps', steps.map(x => [x.done, x.fail, x.text, x.can]).concat(current))) return;
  $('steps').replaceChildren(...steps.map((x, i) => {
    const li = el('li', x.done ? 'done' : x.fail ? 'fail' : i === current ? 'current' : '');
    const mark = el('span', 'mk', x.done ? '✓' : x.fail ? '!' : ''), body = el('div');
    body.append(el('b', '', x.title), el('small', '', x.text)); li.append(mark, body);
    if (x.run) { const b = el('button', 'btn xs' + (i === current ? ' primary' : ''), x.done ? '重做' : '执行'); b.disabled = !x.can; b.onclick = x.run; li.append(b); }
    else li.append(el('span'));
    return li;
  }));
}
async function runJob(action, options, allowFail = false) {
  await api('action', options ? {action, options} : {action});
  do { await sleep(400); await poll(); } while (latest.busy);
  const job = latest.job;
  if (job?.status !== 'success') {
    if (allowFail) { dismissedJob = job ? job.started + job.status : ''; return false; }
    throw Object.assign(new Error(`${ACTION_NAMES[action] || action}失败：${job?.message || '未知原因'}`), {shown: true});   // 横幅里已经显示
  }
  return true;
}
async function runInit() {
  if (initRunning) return; initRunning = true; update(latest);
  try {
    if (!latest.connected) throw new Error('先选择串口并连接。');
    if (!latest.shell_ready) {
      await api('wake', {});
      for (let i = 0; i < 12 && !latest.shell_ready; i++) { await sleep(400); await poll(); }
      if (!latest.shell_ready) throw new Error('没有等到 root 提示符：确认板子已启动并已登录 root。');
    }
    if (!latest.identity_ok) await runJob('probe');
    if (!latest.checks?.prepared) await runJob('prepare');
    if (!latest.checks?.preflight?.passed) {
      await runJob('status', undefined, true);
      if (!latest.checks?.preflight?.passed) await runJob('camera_preset');
    }
    await runJob('camera_get');
    toast('设备已就绪'); closePops();
  } catch (e) { if (!e.shown) toast(e.message, true); }
  finally { initRunning = false; update(latest); }
}

/* ---------- 图表 ---------- */
function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
function fitCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1, w = canvas.clientWidth, h = canvas.clientHeight;
  if (!w || !h) return null;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) { canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr); }
  const ctx = canvas.getContext('2d'); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, w, h);
  return {ctx, w, h};
}
const PLOT = {left: 44, right: 16, top: 14, bottom: 26};
function drawProfile() {
  const fit = fitCanvas($('profile')); if (!fit) return;
  const {ctx, w, h} = fit, profile = latest.live?.profile, width = profile?.width || 16384;
  const x0 = PLOT.left, x1 = w - PLOT.right, y0 = PLOT.top, y1 = h - PLOT.bottom;
  const X = px => x0 + (x1 - x0) * px / width, Y = v => y1 - (y1 - y0) * v / 255;
  ctx.font = '10.5px ' + cssVar('--mono'); ctx.lineWidth = 1;
  ctx.fillStyle = cssVar('--plot-target'); ctx.fillRect(x0, Y(200), x1 - x0, Y(150) - Y(200));
  ctx.strokeStyle = cssVar('--plot-grid'); ctx.fillStyle = cssVar('--plot-axis');
  for (const v of [0, 50, 100, 150, 200, 255]) { ctx.beginPath(); ctx.moveTo(x0, Math.round(Y(v)) + .5); ctx.lineTo(x1, Math.round(Y(v)) + .5); ctx.stroke(); ctx.textAlign = 'right'; ctx.fillText(v, x0 - 8, Y(v) + 3.5); }
  ctx.textAlign = 'center';
  for (let px = 0; px <= width; px += width / 8) { ctx.beginPath(); ctx.moveTo(Math.round(X(px)) + .5, y0); ctx.lineTo(Math.round(X(px)) + .5, y1); ctx.stroke(); ctx.fillText(Math.round(px), Math.min(Math.max(X(px), x0 + 10), x1 - 14), h - 9); }
  if (!profile) { ctx.fillStyle = cssVar('--muted'); ctx.font = '12.5px ' + cssVar('--sans'); ctx.fillText('点击「单帧」或「实时」开始采集', (x0 + x1) / 2, (y0 + y1) / 2); return; }
  const n = profile.bins, center = i => X((i + .5) * width / n);
  ctx.beginPath();
  for (let i = 0; i < n; i++) ctx.lineTo(center(i), Y(profile.max[i]));
  for (let i = n - 1; i >= 0; i--) ctx.lineTo(center(i), Y(profile.min[i]));
  ctx.closePath(); ctx.fillStyle = cssVar('--plot-band'); ctx.fill();
  ctx.beginPath();
  for (let i = 0; i < n; i++) ctx.lineTo(center(i), Y(profile.mean[i]));
  ctx.strokeStyle = cssVar('--accent'); ctx.lineWidth = 1.5; ctx.stroke();
  const edge = latest.live?.focus?.edge;
  if (edge) {
    ctx.setLineDash([3, 3]); ctx.strokeStyle = cssVar('--warn'); ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(Math.round(X(edge.x)) + .5, y0); ctx.lineTo(Math.round(X(edge.x)) + .5, y1); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = cssVar('--warn'); ctx.textAlign = X(edge.x) > (x0 + x1) / 2 ? 'right' : 'left'; ctx.fillText(` 边缘 x=${edge.x} `, X(edge.x), y0 + 10);
  }
  if (hoverX !== null && hoverX >= x0 && hoverX <= x1) {
    const i = Math.max(0, Math.min(n - 1, Math.floor((hoverX - x0) / (x1 - x0) * n)));
    ctx.strokeStyle = cssVar('--ink-2'); ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(Math.round(center(i)) + .5, y0); ctx.lineTo(Math.round(center(i)) + .5, y1); ctx.stroke();
    const a = Math.round(i * width / n), b = Math.round((i + 1) * width / n) - 1;
    $('profile-readout').textContent = `x ${a}–${b}   均 ${profile.mean[i].toFixed(1)}   最小 ${profile.min[i].toFixed(1)}   最大 ${profile.max[i].toFixed(1)}`;
  }
}
function drawHistory() {
  if ($('focus-dock').hidden) return;
  const fit = fitCanvas($('focus-history')); if (!fit) return;
  const {ctx, w, h} = fit, points = (latest.live?.history || []).filter(p => p.sharpness !== null && p.sharpness !== undefined);
  ctx.font = '10.5px ' + cssVar('--mono'); ctx.fillStyle = cssVar('--muted');
  ctx.strokeStyle = cssVar('--plot-grid'); ctx.lineWidth = 1;
  for (const f of [.25, .5, .75]) { ctx.beginPath(); ctx.moveTo(0, Math.round(h * f) + .5); ctx.lineTo(w, Math.round(h * f) + .5); ctx.stroke(); }
  if (points.length < 2) { ctx.textAlign = 'center'; ctx.fillText('实时采集几帧后显示曲线；拧对焦环找最高点', w / 2, h / 2 + 4); return; }
  const values = points.map(p => p.sharpness), hi = Math.max(...values), lo = Math.min(...values), span = (hi - lo) || 1;
  const X = i => 6 + (w - 12) * i / (points.length - 1), Y = v => h - 8 - (h - 22) * (v - lo) / span;
  ctx.beginPath(); values.forEach((v, i) => ctx.lineTo(X(i), Y(v))); ctx.strokeStyle = cssVar('--accent'); ctx.lineWidth = 1.5; ctx.stroke();
  const best = values.indexOf(hi);
  ctx.fillStyle = cssVar('--warn'); ctx.beginPath(); ctx.arc(X(best), Y(hi), 3, 0, 7); ctx.fill();
  ctx.textAlign = best > points.length / 2 ? 'right' : 'left'; ctx.fillText(` 最高 ${hi.toFixed(2)} `, X(best), Math.max(11, Y(hi) - 5));
  ctx.fillStyle = cssVar('--accent'); ctx.beginPath(); ctx.arc(X(values.length - 1), Y(values[values.length - 1]), 2.5, 0, 7); ctx.fill();
}
function drawAll() { drawProfile(); drawHistory(); }

function setRo(id, cls) { const e = $(id); e.className = 'ro' + (cls ? ' ' + cls : ''); }
function updateAcquire(s, ready) {
  const live = s.live || {}, st = live.stats, focus = live.focus, running = !!live.running || (s.busy && s.job?.action === 'live');
  const pattern = s.camera?.params?.TestPattern?.raw;
  $('snap-at').textContent = live.at ? `#${live.count} · ${live.at.slice(11)} · ${live.width} × ${live.height}${running ? ' · 实时' : ''}${pattern ? ' · 测试图' : ''}` : '';
  const edge = focus?.edge;
  $('t-edge').textContent = edge ? edge.width_px.toFixed(2) + ' px' : focus ? '无边缘' : '—';
  setRo('tile-edge', edge ? (edge.width_px <= 2 ? 'ok' : edge.width_px <= 4 ? 'warn' : 'bad') : '');
  $('tile-edge').title = '最强边缘 10–90% 的宽度，目标 ≤ 2 px';
  $('t-edge-note').textContent = edge ? `边缘 x=${edge.x} · 对比 ${edge.contrast.toFixed(0)} · 目标 ≤ 2 px` : focus ? '画面里没有足够清楚的黑白直边' : '';
  $('t-sharp').textContent = focus ? focus.sharpness.toFixed(2) : '—';
  const history = (live.history || []).filter(p => p.sharpness !== null && p.sharpness !== undefined);
  if (history.length >= 2) { const d = history[history.length - 1].sharpness - history[history.length - 2].sharpness; $('t-sharp-note').textContent = `${d >= 0 ? '▲' : '▼'} ${Math.abs(d).toFixed(2)}`; }
  else $('t-sharp-note').textContent = '';
  const segments = focus?.segments || [], top = Math.max(...segments, 1e-9);
  if (changed('segbars', segments)) $('segbars').replaceChildren(...Array.from({length: 8}, (_, i) => { const bar = el('div'); bar.style.height = Math.max(3, 100 * (segments[i] || 0) / top) + '%'; bar.title = segments[i] !== undefined ? `第 ${i + 1} 段（x ${i * 2048}–${i * 2048 + 2047}）：${segments[i].toFixed(2)}` : ''; return bar; }));
  $('t-mean').textContent = st ? st.mean.toFixed(1) : '—';
  $('t-range').textContent = st ? `${st.min} – ${st.max}` : '—';
  $('t-pct').textContent = st ? `${st.p1} / ${st.p50} / ${st.p99}` : '—';
  $('t-sat').textContent = st ? st.saturated.toLocaleString() : '—';
  setRo('tile-sat', st && st.saturated ? 'warn' : '');
  const seen = live.bits?.seen;
  if (changed('bits', String(seen))) $('bits').replaceChildren(...Array.from({length: 8}, (_, i) => el('i', seen !== undefined && (seen >> (7 - i) & 1) ? 'on' : '', String(7 - i))));
  $('bits-cell').title = seen === undefined ? '出现过的位（7 → 0）' : seen === 0xff ? '8 个位都出现过' : `没出现过的位：${live.bits.missing}。图暗时高位不亮是正常的；用测试图“水平渐变”判断链路`;
  const advice = !st ? ['', ''] : st.saturated > 0.001 * live.width * live.height ? ['饱和过多：降低曝光、数字位移或光源亮度', 'warn']
    : st.p99 < 60 ? ['画面很暗：先加光或开大光圈，再加曝光 / 数字位移 / TDI', 'warn']
    : st.p50 >= 150 && st.p50 <= 200 ? ['亮度合适', ''] : st.p50 > 200 ? ['偏亮，亮点余量小', 'warn'] : ['偏暗：背景建议 150–200', 'warn'];
  $('bright-advice').textContent = advice[0]; $('bright-advice').className = 'ro advice' + (advice[1] ? ' warn' : '');
  // 空状态：未初始化时盖住显示区
  const initialized = s.connected && s.shell_ready && s.identity_ok;
  $('stage-empty').hidden = initialized || !!live.profile || initRunning;
  $('stage-empty-title').textContent = !s.connected ? '未连接设备' : '设备未初始化';
  $('stage-empty-text').textContent = !s.connected ? '选择串口连接后，一键初始化即可采图' : '完成初始化后即可采图';
  $('stage-connect').textContent = !s.connected ? '连接设备' : '初始化';
  if (liveShown !== live.at) { liveShown = live.at; drawAll(); }
}

/* ---------- 主刷新 ---------- */
const ACTION_NAMES = {probe: '检查板卡', prepare: '加载程序与驱动', status: '采集卡体检', preflight: '检查采集条件', capture: '采集', snap: '单帧采集',
  live: '实时采集', fetch_last: '保存到电脑', dmesg: '读取内核日志', release: '卸载驱动', stream: '连续运行', replay: '离线回放', network: '配置网口',
  card_xml: '导出采集卡 XML', card_set: '写采集卡参数', camera_probe: '读取相机型号', camera_xml: '导出相机 XML', camera_regs: '读取寄存器表',
  camera_set: '写相机参数', camera_get: '读取相机参数', camera_preset: '切换单线模式', board_session: '一键上板测试', driver: '重新加载驱动', bench: 'CPU 基准', prepare_stream: '加载连续程序'};
function sb(id, cls, text, title = '') { const e = $(id); e.firstElementChild.className = 'dot' + (cls ? ' ' + cls : ''); e.lastElementChild.textContent = text; e.title = title; }
function uiReady() { const s = latest; return !!(s.connected && s.shell_ready && s.identity_ok && !s.busy && !initRunning); }

function update(s) {
  latest = s;
  const shell = s.connected && s.shell_ready, ready = uiReady();
  const pre = s.checks?.preflight, card = s.checks?.card || {}, params = s.camera?.params || {}, fields = s.checks?.fields || {};
  const job = s.job, running = s.busy && job?.status === 'running';
  if (!running) stopRequested = false;

  // 作业结束：失败时撤回属性面板里没写进去的值
  const jobKey = job ? job.started + job.status + job.action : '';
  if (jobKey !== lastJobKey) { lastJobKey = jobKey; if (job && job.status !== 'running' && job.action === 'camera_set') { CAM_PARAMS.forEach(p => p.shown = null); $('ffc-set').dataset.shown = ''; } }

  // 顶栏
  const initialized = shell && s.identity_ok;
  $('device-label').textContent = !s.connected ? '未连接' : `${s.port} · ${initialized ? '就绪' : s.shell_ready ? '未初始化' : '等待终端'}`;
  $('device-dot').className = 'dot' + (!s.connected ? '' : initialized ? ' ok' : ' warn');
  const liveRunning = !!s.live?.running || (running && job.action === 'live');
  $('snap').disabled = !ready; $('live-start').disabled = !ready; $('live-start').classList.toggle('on', liveRunning);
  $('stop').disabled = !running || stopRequested;
  $('stop').title = running ? (job.action === 'live' ? '停止实时采集（当前这一帧采完后结束）' : `中断：${ACTION_NAMES[job.action] || job.action}`) : '停止当前操作';
  $('fetch').disabled = !shell || s.busy || !s.last_frame;
  $('nav-run').hidden = !(running && job.action === 'stream');

  // 状态栏：当前作业
  const box = $('sb-job');
  if (running) {
    box.className = 'sb-job running';
    $('sb-job-text').textContent = job.action === 'live' ? (stopRequested ? '正在停止实时采集，等待当前帧完成…' : `实时采集中 · 第 ${s.live?.count ?? 0} 帧`) : `正在${ACTION_NAMES[job.action] || job.action}…`;
    box.title = '';
  } else if (job) {
    const name = ACTION_NAMES[job.action] || job.action;
    box.className = 'sb-job ' + (job.status === 'error' ? 'error' : 'done');
    $('sb-job-text').textContent = `${name}${job.status === 'error' ? '失败' : '完成'} · ${job.started.slice(11)}${job.message && job.status !== 'error' ? ' — ' + job.message : ''}`;
    box.title = job.message || '';
  } else { box.className = 'sb-job idle'; $('sb-job-text').textContent = s.connected ? '就绪' : '未连接设备'; box.title = ''; }

  // 错误横幅
  const showBanner = job && job.status === 'error' && dismissedJob !== job.started + job.status;
  $('banner').hidden = !showBanner;
  if (showBanner) { $('banner-title').textContent = `${ACTION_NAMES[job.action] || job.action}失败`; $('banner-text').textContent = job.message || ''; }

  // 状态栏：设备
  sb('sb-serial', s.connected ? (s.shell_ready ? 'ok' : 'warn') : '', s.connected ? s.port : '串口未连接');
  sb('sb-card', pre?.passed ? 'ok' : pre?.known ? 'bad' : '', pre?.passed ? `采集卡 ${card.CameraWidth ?? ''}${card.LineRateHz ? ' · ' + (card.LineRateHz / 1000).toFixed(1) + ' kHz' : ''}` : pre?.known ? '采集卡预检未过' : '采集卡', pre?.at ? '上次体检 ' + pre.at.slice(11) : '');
  const exp = params.ExposureTime?.value, rate = params.LineRate?.value, temp = params.Temperature?.value;
  sb('sb-camera', exp !== undefined ? 'ok' : '', exp !== undefined ? `相机 ${exp.toFixed(1)} µs${rate ? ' · ' + (rate / 1000).toFixed(2) + ' kHz' : ''}${temp !== undefined ? ' · ' + temp.toFixed(1) + ' ℃' : ''}` : '相机');
  sb('sb-net', s.net?.pc_ip ? 'ok' : '', s.net?.pc_ip ? `网口 ${s.net.pc_ip}` : '网口未配置', s.net?.pc_ip ? '' : '未配置时整图回传走串口（约 7 分钟）');

  // 设备面板
  $('connect').disabled = s.connected || s.busy; $('disconnect').disabled = !s.connected || s.busy || initRunning;
  $('port').disabled = s.connected; $('refresh').disabled = s.connected; $('shutdown').disabled = s.busy;
  $('device-info').textContent = fields.VENDOR ? `ZCU102 · ${fields.KERNEL || ''} · PCIe ${fields.VENDOR}:${fields.DEVICE}` : '板卡信息在「检查板卡」后显示';
  updateSteps(s);

  // 通用按钮可用性
  document.querySelectorAll('[data-action]').forEach(b => { const basic = ['probe', 'dmesg', 'fetch_last'].includes(b.dataset.action); b.disabled = !shell || s.busy || initRunning || (!basic && !s.identity_ok); });
  for (const id of ['net-setup', 'card-set', 'start-stream']) $(id).disabled = !ready;
  $('stream-stop').disabled = !(running && job.action === 'stream') || stopRequested;
  $('replay').disabled = s.busy; $('stream-seconds').disabled = $('stream-processor').disabled = s.busy;

  updateCamera(s, ready); updateAcquire(s, ready); updateStream(s); updateSystem(s, ready); updateImage(s);

  // 日志
  for (const e of s.events) {
    const text = e.kind === 'rx' ? e.text : '\n[' + e.time.slice(11) + ' / ' + e.kind + '] ' + e.text + '\n';
    $('terminal').append(document.createTextNode(text)); lastSeq = Math.max(lastSeq, e.seq);
  }
  if ($('terminal').textContent.length > 180000) $('terminal').textContent = '[只显示最近的日志；完整记录可导出]\n' + $('terminal').textContent.slice(-150000);
  if ($('autoscroll').checked && s.events.length && !$('logdock').hidden) $('terminal').scrollTop = $('terminal').scrollHeight;
}

function updateStream(s) {
  const stream = s.stream || {}, result = stream.result || {}, ended = stream.ended;
  const replay = stream.channel === 'recorded-replay', unknown = stream.status === 'unknown';
  const failed = stream.status === 'error' || [stream, ended, result].some(r => r && ['error', 'source_error', 'processing_error', 'output_error', 'stop_error'].some(k => r[k] !== undefined && r[k] !== null && r[k] !== 0 && r[k] !== '0' && r[k] !== '' && r[k] !== false));
  $('stream-badge').textContent = unknown ? '停止未确认' : failed ? '异常' : replay ? '离线回放 · 合成数据' : stream.status === 'running' ? '运行中' : stream.status === 'ended' ? '已结束' : '未开始';
  $('stream-badge').className = 'tag' + (unknown || failed ? ' bad' : stream.status === 'running' ? ' ok' : replay ? ' warn' : '');
  $('stream-count').textContent = unknown ? '—' : ended?.processed ?? result.processed_total ?? '—';
  $('stream-block').textContent = !unknown && result.sequence ? `${result.block_id} / ${result.width} × ${result.height}` : '—';
  $('stream-gray').textContent = !unknown && result.pixel_count ? `${result.min}–${result.max} / ${(Number(result.sum) / Number(result.pixel_count)).toFixed(1)}` : '—';
  const rate = ended && Number(ended.elapsed_ms) > 0 ? Number(ended.bytes) / 1e6 / (Number(ended.elapsed_ms) / 1000) : null;
  $('stream-rate').textContent = rate === null || unknown ? '—' : rate.toFixed(0) + ' MB/s';
  $('tile-rate').className = 'kpi primary' + (rate === null || replay ? '' : rate >= 240 ? ' ok' : rate >= 160 ? ' warn' : ' bad');
  $('stream-decision').textContent = unknown ? '不可确认' : failed ? '需核查' : result.processor === 'candidates' && result.sequence ? `候选 ${result.above_threshold}` : '—';
  const full = ended && Number(ended.queue_full_events) > 0;
  $('stream-summary').textContent = unknown ? '没有确认连续程序已经停止，之前的结果不能算数。查看日志和板上进程。'
    : ended ? `${replay ? '离线回放' : '本次运行'}：接收 ${ended.received ?? '—'} 块，处理 ${ended.processed ?? '—'} 块，块号不连续 ${ended.id_discontinuities ?? '—'} 次，队列峰值 ${ended.queue_peak ?? '—'}，队列满 ${ended.queue_full_events ?? '—'} 次，错误码 ${ended.error ?? '—'}。`
      + (full ? '\n队列满 = 处理线程跟不上 DMA（不是采集卡的问题）。到 设置 → 诊断 运行 CPU 基准，看 stats 一行。' : '')
      + (ended.dma_ms !== undefined && !replay ? `\nDMA 用时 ${ended.dma_ms} ms，等数据 ${ended.wait_ms} ms，每次搬 ${ended.dma_chunk} 字节。` : '')
    : failed ? '会话报告了输入、处理、输出或停止错误，查看日志。'
    : result.sequence ? `${replay ? '离线合成记录，不代表板上运行。' : ''}本块起始于累计第 ${result.received_line_start} 行。界面按周期抽样显示，每一块都参与处理。`
    : '尚未运行。开始后这里显示接收 / 处理的块数、块号是否连续、队列峰值和 DMA 用时。接收与处理各占一个线程，队列满时停止并报告，不会悄悄丢块。';
}

const CARD_ROWS = [['CameraWidth', '宽度'], ['CameraHeight', '每块行数'], ['ClConfiguration', 'CL 配置'], ['DeviceTapGeometry', 'Tap'], ['CameraScanType', '扫描类型'],
  ['LineRateHz', '行频（卡测得）'], ['CameraClockMHz', '相机时钟 MHz'], ['max_transfer', '单次 DMA 字节'], ['poisoned', '隔离标志'], ['hdr_ready', '待取数据块']];
function updateSystem(s, ready) {
  const card = s.checks?.card || {}, pre = s.checks?.preflight, cells = [];
  if (pre?.known) cells.push(['预检', pre.passed ? '通过' : '未通过'], ['像素时钟', (pre.clocks & 1 ? '锁定' : '未锁定') + ' · 0x' + (pre.clocks ?? 0).toString(16)], ['DDR', pre.ddr === 2 ? '2 · 已校准' : pre.ddr === 0 ? '0 · 正常' : String(pre.ddr)]);
  for (const [key, label] of CARD_ROWS) if (card[key] !== undefined) cells.push([label, String(card[key])]);
  if (!cells.length) cells.push(['状态', '尚未体检']);
  if (changed('card-info', cells)) $('card-info').replaceChildren(...cells.map(([k, v]) => { const d = el('div'); d.append(el('dt', '', k), el('dd', '', v)); return d; }));
  $('card-at').textContent = pre?.at ? `上次体检 ${pre.at.slice(11)}` : '尚未体检';
  const drv = s.driver || {}, fast = drv.dma_fast === 'Y';
  const drvText = drv.cam_serial ? `${fast ? '快速（1 MiB）' : drv.dma_fast === 'MISSING' ? '旧版驱动（无快速模式）' : '标准（64 KiB）'} · 相机串口${drv.cam_serial === 'Y' ? '已放开' : '未放开'}` : s.checks?.prepared ? '已加载，模式未知' : '未知';
  $('driver-state').textContent = drvText;
  $('run-dma').textContent = drv.cam_serial ? (fast ? '快速' : drv.dma_fast === 'MISSING' ? '旧版驱动' : '标准') : '未知';
  $('driver-fast').classList.toggle('on', drv.cam_serial && fast); $('driver-slow').classList.toggle('on', drv.cam_serial && !fast);
  $('driver-fast').disabled = $('driver-slow').disabled = !ready;
  $('net-state').textContent = s.net?.pc_ip ? `已连通 → ${s.net.pc_ip}:${s.net.port}` : '未配置，回传走串口';
  $('local-dir').textContent = s.captures || '—';
  $('prepared').textContent = s.checks?.prepared ? '已加载到板上内存' : '尚未加载';
  const rows = s.bench || [], fastest = Math.max(...rows.map(r => r.x15k), 1);
  if (rows.length && changed('bench', rows)) $('bench-table').replaceChildren(...rows.map(r => {
    const tr = el('tr'), bar = el('div', 'bar' + (r.x15k < 1 ? ' bad' : '')), fill = el('i'); fill.style.width = Math.max(3, 100 * r.x15k / fastest) + '%'; bar.append(fill);
    const cell = el('td'); cell.append(bar);
    tr.append(el('td', '', r.name), el('td', '', r.mbps.toFixed(0)), el('td', '', r.x15k.toFixed(2) + ' ×'), cell); return tr;
  }));
}

function updateImage(s) {
  const frame = s.last_frame;
  $('board-frame').hidden = !frame; $('view-latest-btn').disabled = !frame?.local_path;
  if (!frame) return;
  const mb = frame.size_bytes ? ` · ${(frame.size_bytes / 1048576).toFixed(1)} MB` : '';
  $('board-frame').textContent = frame.local_path ? `电脑：${frame.local_path}${mb}` : `板上内存：${frame.path}${mb}（未保存到电脑，断电会丢）`;
  if (frame.local_path && displayedFrame !== frame.name) { displayedFrame = frame.name; loadCapturedPreview(frame); }
}
function loadCapturedPreview(frame) {
  const img = new Image();
  img.onload = () => {
    const canvas = $('image'), scale = Math.min(1, 2048 / img.width, 1024 / img.height);
    canvas.width = Math.max(1, Math.round(img.width * scale)); canvas.height = Math.max(1, Math.round(img.height * scale));
    canvas.getContext('2d').drawImage(img, 0, 0, canvas.width, canvas.height);
    canvas.hidden = false; $('image-empty').hidden = true;
    $('image-info').textContent = `${frame.name} · ${frame.width || img.width} × ${frame.height || img.height} · 预览已缩小`;
  };
  img.onerror = () => { $('image-info').textContent = '预览读取失败：' + frame.name; };
  img.src = '/api/captures/preview?name=' + encodeURIComponent(frame.name) + '&t=' + Date.now();
}
function parsePGM(bytes) {
  let i = 0; const space = c => [9, 10, 13, 32].includes(c);
  function word() { while (i < bytes.length) { if (space(bytes[i])) { i++; continue; } if (bytes[i] === 35) { while (i < bytes.length && bytes[i] !== 10) i++; continue; } break; }
    let out = ''; while (i < bytes.length && !space(bytes[i])) { if (out.length > 32) throw new Error('PGM 头部过长'); out += String.fromCharCode(bytes[i++]); } return out; }
  if (word() !== 'P5') throw new Error('仅支持 P5 二进制灰度 PGM。');
  const w = Number(word()), h = Number(word()), max = Number(word());
  if (!Number.isInteger(w) || !Number.isInteger(h) || w < 1 || h < 1 || w > 32768 || h > 32768 || w * h > 34000000 || max !== 255) throw new Error('需要 8 位 Mono8，不超过 3400 万像素，每边不超过 32768。');
  if (!space(bytes[i])) throw new Error('PGM 头部缺少分隔符'); i++;
  if (bytes.length - i !== w * h) throw new Error('像素长度与宽高不一致，可能是不完整的图。');
  return {w, h, pixels: bytes.subarray(i)};
}

/* ---------- 轮询 ---------- */
async function refreshPorts() {
  try {
    const previous = $('port').value, ports = await api('ports');
    $('port').replaceChildren(new Option(ports.length ? '选择串口' : '没有发现串口', ''));
    ports.sort((a, b) => Number(b.suggested) - Number(a.suggested)).forEach(p => $('port').add(new Option(p.device + ' · ' + p.description, p.device)));
    if (ports.some(p => p.device === previous)) $('port').value = previous;
    else { const hint = ports.find(p => p.suggested); if (hint && ports.filter(p => p.suggested).length === 1) $('port').value = hint.device; }
    $('port-note').textContent = ports.some(p => p.suggested) ? 'J83 USB · 115200 8N1 · 选 Linux 控制台对应的端口' : 'J83 USB · 115200 8N1 · 未发现 CP2108 串口';
  } catch (e) { toast(e.message, true); }
}
/* 串行轮询：一次只有一个请求在途，保证 runJob 等到的是动作之后的新状态，日志也不会重复。 */
let pollQueue = Promise.resolve(), pollQueued = 0;
function poll() {
  if (pollQueued > 1) return pollQueue;
  pollQueued++;
  pollQueue = pollQueue.then(pollOnce).finally(() => pollQueued--);
  return pollQueue;
}
async function pollOnce() {
  try {
    const state = await api('state?after=' + lastSeq);
    if (serviceDisconnected) { token = (await api('bootstrap')).token; serviceDisconnected = false; }
    update(state); sb('service', 'ok', '服务'); $('service').title = '本机服务在线';
  } catch (e) {
    if (!serviceDisconnected) { serviceDisconnected = true; document.dispatchEvent(new Event('workbench-disconnected')); }
    sb('service', 'bad', '服务断开', '本机服务没有响应：重新从桌面入口启动');
    $('stream-badge').textContent = '连接中断'; $('stream-badge').className = 'tag bad';
  }
}
async function showSource(id, open = true) {
  try {
    const data = await api('source?id=' + encodeURIComponent(id)); if (open) openSettings('sec-source'); sourcePath = data.path;
    $('source-title').textContent = files.find(f => f.id === id)?.title || id; $('source-path').textContent = data.path; $('source-guide').textContent = data.guide;
    const fragment = document.createDocumentFragment();
    data.text.split('\n').forEach((text, i) => { const line = el('span', 'code-line' + (i + 1 === data.line ? ' target' : '')); line.append(el('span', 'line-num', i + 1), document.createTextNode(text.replace(/\r$/, '') || ' ')); fragment.append(line); });
    $('code').replaceChildren(fragment);
    document.querySelectorAll('.source-file').forEach(e => e.classList.toggle('active', e.dataset.id === id));
    requestAnimationFrame(() => { $('code-scroll').scrollTop = Math.max(0, (data.line - 4) * 20); });
  } catch (e) { toast(e.message, true); }
}

/* ---------- 事件 ---------- */
const CONFIRM = {
  release: ['卸载驱动？', '采集卡会被释放，采图和连续运行都要重新加载驱动后才能用。', '卸载', true],
  board_session: ['开始一键上板测试？', '大约 4–6 分钟，期间会改相机和采集卡的运行时参数（断电恢复）。\n拍实拍图时请保持场景不动、画面有亮有暗。', '开始'],
  camera_preset: ['切换到单线模式？', '相机 ClConfiguration = Base、DeviceTapGeometry = 3；采集卡 CameraWidth = 16384；然后体检。\n只改运行时的值，断电恢复。', '切换'],
};
function bind() {
  document.querySelectorAll('.mode').forEach(b => b.onclick = () => showView(b.dataset.view));
  document.querySelectorAll('.tab').forEach(b => b.onclick = () => showPane(b.dataset.pane));
  document.querySelectorAll('[data-file]').forEach(b => b.onclick = () => showSource(b.dataset.file));
  document.querySelectorAll('[data-action]').forEach(b => b.onclick = async () => {
    closePops(); const c = CONFIRM[b.dataset.action];
    if (c && !(await ask(...c))) return;
    act(b.dataset.action);
  });
  document.querySelectorAll('.set-nav button').forEach(b => b.onclick = () => openSettings(b.dataset.sec));
  // 弹出面板
  $('device-btn').onclick = () => togglePop('device-pop');
  $('cam-more').onclick = () => togglePop('cam-menu');
  document.addEventListener('mousedown', e => { for (const [pop, trigger] of Object.entries(POPS)) if (!$(pop).hidden && !$(pop).contains(e.target) && !$(trigger).contains(e.target)) closePops(); });
  document.addEventListener('keydown', e => {
    if (e.key !== 'Escape') return;
    if (modalCancel) return modalCancel();
    if (!$('settings').hidden) { $('settings').hidden = true; return; }
    closePops();
  });
  $('settings-btn').onclick = () => openSettings(document.querySelector('.set-sec.active')?.id || 'sec-card');
  $('help-btn').onclick = () => openSettings('sec-guide');
  $('settings-close').onclick = () => { $('settings').hidden = true; };
  $('settings').addEventListener('mousedown', e => { if (e.target === $('settings')) $('settings').hidden = true; });
  $('stage-connect').onclick = e => { e.stopPropagation(); togglePop('device-pop'); };
  // 设备
  $('connect').onclick = async () => { if (!$('port').value) return toast('先选择串口。', true); if (await post('connect', {port: $('port').value})) runInit(); };
  $('disconnect').onclick = () => post('disconnect');
  $('refresh').onclick = refreshPorts;
  $('init').onclick = runInit;
  // 工具栏
  $('snap').onclick = () => act('snap', {fetch: $('snap-fetch').checked});
  $('live-start').onclick = () => act('live', {});
  $('fetch').onclick = () => act('fetch_last');
  $('stop').onclick = async () => {
    const job = latest.job; if (!latest.busy || !job) return;
    if (job.action === 'live') { stopRequested = true; update(latest); return post('live_stop'); }
    if (!(await ask(`中断${ACTION_NAMES[job.action] || job.action}？`, '向板子发送 Ctrl+C。正在进行的写入或传输会停在中途。', '中断', true))) return;
    stopRequested = true; post('interrupt');
  };
  $('stream-stop').onclick = () => $('stop').onclick();
  $('banner-close').onclick = () => { if (latest.job) dismissedJob = latest.job.started + latest.job.status; $('banner').hidden = true; };
  $('banner-log').onclick = () => setLog(true);
  // 相机
  $('cam-refresh').onclick = () => act('camera_get');
  $('ffc-set').onchange = () => act('camera_set', {name: 'FFCUserSetSelector', value: Number($('ffc-set').value)});
  $('ffc-enable').onchange = () => act('camera_set', {name: 'FFCEnable', value: $('ffc-enable').checked ? 1 : 0});
  $('ffc-mode').onchange = () => { $('ffc-target').disabled = $('ffc-mode').value !== '2'; act('camera_set', {name: 'FFCCalTargetMode', value: Number($('ffc-mode').value)}); };
  $('ffc-target').onchange = () => {
    const v = Math.round(Number($('ffc-target').value));
    if (!Number.isFinite(v) || v < 0 || v > 250) return toast('目标灰度 0 – 250。', true);
    act('camera_set', {name: 'FFCCalTargetValue', value: v});
  };
  $('ffc-frames').onchange = () => act('camera_set', {name: 'FFCSampleFrames', value: Number($('ffc-frames').value)});
  $('ffc-dark').onclick = async () => { if (await ask('暗场校正', '镜头盖已盖好，或光源已关闭？\n相机会把现在看到的画面当作“全黑”。结果不保存，断电恢复。', '开始校正')) act('camera_set', {name: 'FFCCalFPN', value: 1}); };
  $('ffc-bright').onclick = async () => { if (await ask('亮场校正', '光源均匀照满整条线，没有玻璃和靶标，画面不饱和？\n相机会把现在看到的画面当作“均匀的白”。结果不保存，断电恢复。', '开始校正')) act('camera_set', {name: 'FFCCalPRNU', value: 1}); };
  $('ffc-load').onclick = async () => { if (await ask('从闪存恢复校正数据？', '把所选组在相机闪存里的校正数据重新读入内存，刚才做的（未保存的）校正会丢失。', '恢复')) act('camera_set', {name: 'FFCUserSetLoad', value: 1}); };
  document.querySelectorAll('details.group').forEach(d => {
    const saved = recall('sg-group-' + d.dataset.group, null); if (saved !== null) d.open = saved === '1';
    d.addEventListener('toggle', () => store('sg-group-' + d.dataset.group, d.open ? '1' : '0'));
  });
  // 显示区
  $('focus-toggle').onclick = () => { const on = $('focus-toggle').getAttribute('aria-pressed') !== 'true'; $('focus-toggle').setAttribute('aria-pressed', String(on)); $('focus-dock').hidden = !on; store('sg-focus', on ? '1' : '0'); requestAnimationFrame(drawAll); };
  if (recall('sg-focus', '1') === '0') { $('focus-toggle').setAttribute('aria-pressed', 'false'); $('focus-dock').hidden = true; }
  $('history-clear').onclick = async () => { await post('live_clear'); drawHistory(); };
  $('profile').onmousemove = e => { hoverX = e.offsetX; drawProfile(); };
  $('profile').onmouseleave = () => { hoverX = null; $('profile-readout').textContent = ''; drawProfile(); };
  $('view-latest-btn').onclick = () => { if (latest.last_frame?.local_path) loadCapturedPreview(latest.last_frame); };
  $('pgm').onchange = async e => {
    try {
      const file = e.target.files[0]; if (!file) return; if (file.size > 35000000) throw new Error('本地查看限制为 35 MB。');
      const img = parsePGM(new Uint8Array(await file.arrayBuffer()));
      const scale = Math.min(1, 2048 / img.w, 2048 / img.h), w = Math.max(1, Math.round(img.w * scale)), h = Math.max(1, Math.round(img.h * scale));
      const canvas = $('image'); canvas.width = w; canvas.height = h; const ctx = canvas.getContext('2d'), data = ctx.createImageData(w, h); let min = 255, max = 0, sum = 0;
      for (const v of img.pixels) { if (v < min) min = v; if (v > max) max = v; sum += v; }
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const v = img.pixels[Math.min(img.h - 1, Math.floor(y / scale)) * img.w + Math.min(img.w - 1, Math.floor(x / scale))], o = (y * w + x) * 4; data.data[o] = data.data[o + 1] = data.data[o + 2] = v; data.data[o + 3] = 255; }
      ctx.putImageData(data, 0, 0); canvas.hidden = false; $('image-empty').hidden = true;
      $('image-info').textContent = `${file.name} · ${img.w} × ${img.h} · 最小 ${min} / 最大 ${max} / 平均 ${(sum / img.pixels.length).toFixed(1)}`;
    } catch (err) { toast(err.message, true); } finally { e.target.value = ''; }
  };
  // 连续运行
  $('start-stream').onclick = () => act('stream', {seconds: Number($('stream-seconds').value), processor: $('stream-processor').value});
  $('replay').onclick = () => act('replay');
  $('run-dma-link').onclick = () => openSettings('sec-driver');
  // 设置
  $('card-set').onclick = () => act('card_set', {name: $('card-name').value, value: Number($('card-value').value)});
  for (const [id, fast] of [['driver-fast', true], ['driver-slow', false]]) $(id).onclick = async () => {
    if ($(id).classList.contains('on')) return;
    if (await ask(`切换到${fast ? '快速' : '标准'} DMA？`, '会卸载并重新加载驱动，同时放开相机串口。正在进行的采集会被打断。', '切换')) act('driver', {dma_fast: fast});
  };
  $('net-pc').value = recall('sg-net-pc', ''); $('net-board').value = recall('sg-net-board', '');
  $('net-setup').onclick = () => { const pc = $('net-pc').value.trim(), board = $('net-board').value.trim(); store('sg-net-pc', pc); store('sg-net-board', board); act('network', {pc_ip: pc, board_ip: board}); };
  $('snap-fetch').checked = recall('sg-snap-fetch', '0') === '1';
  $('snap-fetch').onchange = () => store('sg-snap-fetch', $('snap-fetch').checked ? '1' : '0');
  document.querySelectorAll('#theme-seg button').forEach(b => b.onclick = () => { store('sg-theme', b.dataset.themeValue); applyTheme(); });
  matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', applyTheme);
  $('shutdown').onclick = async () => { if (!(await ask('关闭本机服务？', '下次需要从桌面入口重新启动。', '关闭服务', true))) return; try { await api('shutdown', {}); toast('本机服务已关闭。'); } catch (e) { toast(e.message, true); } };
  $('copy-path').onclick = async () => { try { await navigator.clipboard.writeText(sourcePath); toast('路径已复制'); } catch { toast(sourcePath, true); } };
  $('export').onclick = async () => { try { const r = await fetch('/api/log'); if (!r.ok) throw new Error('导出失败'); const a = el('a'); a.href = URL.createObjectURL(await r.blob()); a.download = 'sg3000-session-' + new Date().toISOString().replace(/[:.]/g, '-') + '.jsonl'; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000); } catch (e) { toast(e.message, true); } };
  // 日志
  $('log-toggle').onclick = () => setLog($('logdock').hidden);
  $('log-close').onclick = () => setLog(false);
  $('log-open').onclick = () => { $('settings').hidden = true; setLog(true); };
  $('log-size').onclick = () => { $('logdock').classList.toggle('tall'); $('log-size').textContent = $('logdock').classList.contains('tall') ? '恢复' : '加高'; requestAnimationFrame(drawAll); };
  if (window.ResizeObserver) { const ro = new ResizeObserver(() => drawAll()); ro.observe($('profile-pane')); ro.observe($('focus-dock')); }
  else window.addEventListener('resize', drawAll);
}

async function init() {
  applyTheme(); buildCameraRows(); bind();
  showView(recall('sg-view', 'acquire'));
  try {
    const data = await api('bootstrap'); token = data.token; files = data.files; $('project-root').textContent = data.root;
    for (const f of files) { const b = el('button', 'source-file'); b.dataset.id = f.id; b.append(el('span', '', f.title), el('small', '', f.path)); b.onclick = () => showSource(f.id); $('source-files').append(b); }
    await showSource('architecture', false);
    document.dispatchEvent(new Event('workbench-ready'));
    await refreshPorts(); await poll(); setInterval(poll, 700);
    if (!latest.connected) togglePop('device-pop');
  } catch (e) { toast('中控台初始化失败：' + e.message, true); }
}
init();
