'use strict';
// A file import is evidence of a local file only, never of a camera or board capture.
let defectRun='', defectPage=null, defectSelected=null, defectOffset=0;
let defectEpoch=0, cropEpoch=0, layerEpoch=0, defectPolling=false, defectBusy=false, pendingRun='';
let overviewImage=null, defectOnline=false, defectCommandBusy=false, desiredDefectRun='';
let defectRunsSignature='', defectInputsSignature='', defectCommandEpoch=0, defectFailure='';
const defectColors={bright:'#ffca58',dark:'#69c8ee'};
const defectService=id=>id.startsWith('frame-')?'frames':'defects';
function defectImageUrl(run,kind,box){
  const q=new URLSearchParams({id:run,kind});
  if(box)for(const [k,v] of Object.entries(box))q.set(k,String(v));
  return '/api/'+defectService(run)+'/image?'+q.toString();
}
function loadDefectImage(url){return new Promise((resolve,reject)=>{const image=new Image();image.onload=()=>resolve(image);image.onerror=()=>reject(new Error('图像读取失败；显示已清空，请重新选择记录。'));image.src=url;});}
function candidateName(c){return `${c.id} · ${c.polarity==='bright'?'亮':'暗'}`;}
function setDefectStatus(text,error=false){$('defect-status').textContent=text;$('defect-status').className='chip'+(error?' amber':'');}
function clearDefectCrop(){++cropEpoch;const c=$('defect-crop');c.getContext('2d').clearRect(0,0,c.width,c.height);}
function resetDefectDisplay(message='选择记录或开始一次新任务'){
  ++defectEpoch;++layerEpoch;clearDefectCrop();defectRun='';defectPage=null;defectSelected=null;overviewImage=null;defectOffset=0;
  $('defect-runs').value='';$('defect-export').disabled=true;$('defect-rows').replaceChildren();
  for(const id of ['defect-prev','defect-next','defect-layer'])$(id).disabled=true;
  for(const id of ['defect-dimensions','defect-count','defect-cache','defect-page'])$(id).textContent='—';
  $('defect-axis').textContent='x = —';$('defect-provenance').textContent='尚无当前结果 · 文件导入和合成演示均不证明硬件采集。';
  $('defect-selected').textContent='选择候选查看局部';$('defect-coordinates').textContent=message;
  $('defect-empty').hidden=false;$('defect-empty').textContent=message;drawOverview();
}
function updateDefectControls(){
  const blocked=!defectOnline||defectBusy||defectCommandBusy||!!pendingRun;
  for(const id of ['defect-source','defect-input','defect-upload','defect-replay','defect-radius','defect-threshold','defect-polarity'])$(id).disabled=blocked;
  $('defect-start').disabled=blocked||($('defect-source').value==='frames'&&!$('defect-input').value);
  $('defect-input-group').hidden=$('defect-source').value!=='frames';
  $('defect-start').textContent=$('defect-source').value==='frames'?'处理导入图像':'运行 16K 合成演示';
  $('defect-upload-label').classList.toggle('disabled',blocked);
  $('defect-runs').disabled=!defectOnline||defectCommandBusy;
}
function invalidateDefects(message){
  defectOnline=false;resetDefectDisplay('服务连接或结果读取中断；当前显示已失效');
  setDefectStatus('离线服务不可用',true);$('defect-message').textContent=message;updateDefectControls();
}
function drawOverview(){
  const canvas=$('defect-overview'),ctx=canvas.getContext('2d');
  ctx.fillStyle='#162a24';ctx.fillRect(0,0,canvas.width,canvas.height);
  if(!defectPage||!overviewImage)return;
  // Preview sampling uses an integer step. Crop any padded edge cell to the
  // actual source extent before stretching, so full-resolution boxes align.
  const run=defectPage.run,step=run.images?.corrected?.downsample_step||Math.max(1,Math.ceil(run.width/2048));
  ctx.imageSmoothingEnabled=false;ctx.drawImage(overviewImage,0,0,run.width/step,run.height/step,0,0,canvas.width,canvas.height);
  const sx=canvas.width/defectPage.run.width,sy=canvas.height/defectPage.run.height;
  for(const c of defectPage.components){const [x0,y0,x1,y1]=c.bbox;ctx.strokeStyle=defectColors[c.polarity];ctx.lineWidth=c.id===defectSelected?.id?4:2;ctx.strokeRect(x0*sx,y0*sy,Math.max(2,(x1-x0)*sx),Math.max(2,(y1-y0)*sy));}
}
async function selectCandidate(c){
  if(!defectPage)return;
  clearDefectCrop();defectSelected=c;const epoch=cropEpoch,runId=defectRun,pageEpoch=defectEpoch,layer=$('defect-layer').value;
  document.querySelectorAll('#defect-rows tr').forEach(row=>row.classList.toggle('selected',row.dataset.id===c.id));
  $('defect-selected').textContent=candidateName(c);drawOverview();
  const run=defectPage.run,[x0,y0,x1,y1]=c.bbox;
  const width=Math.min(run.width,1024,Math.max(96,x1-x0+48)),height=Math.min(run.height,512,Math.max(80,y1-y0+48));
  const x=Math.max(0,Math.min(run.width-width,Math.floor((x0+x1-width)/2))),y=Math.max(0,Math.min(run.height-height,Math.floor((y0+y1-height)/2)));
  $('defect-coordinates').textContent=`范围 [${x0}, ${x1}) × [${y0}, ${y1}) · 面积 ${c.area} 像素 · ${c.polarity==='bright'?'+':'−'}${c.peak} DN · 帧 ${c.frame_id} · 正在读取局部…`;
  try{
    const image=await loadDefectImage(defectImageUrl(runId,layer,{x,y,width,height}));
    if(epoch!==cropEpoch||pageEpoch!==defectEpoch||runId!==defectRun||layer!==$('defect-layer').value)return;
    const canvas=$('defect-crop');canvas.width=width*3;canvas.height=height*3;
    const ctx=canvas.getContext('2d');ctx.imageSmoothingEnabled=false;ctx.drawImage(image,0,0,canvas.width,canvas.height);
    ctx.strokeStyle='#ffffff8c';ctx.lineWidth=1;ctx.setLineDash([6,6]);
    for(let gx=Math.ceil(x/256)*256;gx<x+width;gx+=256){ctx.beginPath();ctx.moveTo((gx-x)*3,0);ctx.lineTo((gx-x)*3,canvas.height);ctx.stroke();}
    for(let gy=Math.ceil(y/64)*64;gy<y+height;gy+=64){ctx.beginPath();ctx.moveTo(0,(gy-y)*3);ctx.lineTo(canvas.width,(gy-y)*3);ctx.stroke();}
    ctx.setLineDash([]);ctx.lineWidth=2;ctx.strokeStyle=defectColors[c.polarity];ctx.strokeRect((x0-x)*3,(y0-y)*3,(x1-x0)*3,(y1-y0)*3);
    $('defect-coordinates').textContent=$('defect-coordinates').textContent.replace(' · 正在读取局部…','')+(width<x1-x0||height<y1-y0?' · 大候选仅显示中心局部，完整范围见坐标':'');
  }catch(e){if(epoch===cropEpoch&&pageEpoch===defectEpoch){desiredDefectRun='';invalidateDefects(e.message);toast(e.message,true);}}
}
function populateDefectRows(){
  const fragment=document.createDocumentFragment();
  for(const c of defectPage.components){const row=document.createElement('tr');row.dataset.id=c.id;const first=document.createElement('td'),button=document.createElement('button');button.textContent=candidateName(c);button.onclick=()=>selectCandidate(c);first.append(button);row.append(first);for(const value of [`${c.bbox[0]} / ${c.bbox[1]}`,c.area,`${c.polarity==='bright'?'+':'−'}${c.peak}`]){const cell=document.createElement('td');cell.textContent=value;row.append(cell);}fragment.append(row);}
  $('defect-rows').replaceChildren(fragment);const page=defectPage.pagination;
  $('defect-page').textContent=page.total?`${page.offset+1}–${page.offset+page.returned} / ${page.total}`:'没有达到阈值的候选';
  $('defect-prev').disabled=page.offset===0;$('defect-next').disabled=!page.has_more;
}
async function reloadDefectLayer(){
  if(!defectPage)return;
  const epoch=++layerEpoch,pageEpoch=defectEpoch,id=defectRun,layer=$('defect-layer').value;
  overviewImage=null;drawOverview();$('defect-empty').hidden=false;$('defect-empty').textContent='正在读取图层…';
  if(defectSelected)selectCandidate(defectSelected);else clearDefectCrop();
  try{const image=await loadDefectImage(defectImageUrl(id,layer));if(epoch!==layerEpoch||pageEpoch!==defectEpoch||id!==defectRun)return;overviewImage=image;$('defect-empty').hidden=true;drawOverview();}
  catch(e){if(epoch===layerEpoch&&pageEpoch===defectEpoch){desiredDefectRun='';invalidateDefects(e.message);toast(e.message,true);}}
}
async function showDefectRun(id,offset=0){
  desiredDefectRun=id;resetDefectDisplay('正在读取结果…');const epoch=defectEpoch;defectRun=id;defectOffset=offset;
  try{
    const result=await api(defectService(id)+'/results?'+new URLSearchParams({id,offset:String(offset),limit:'100'}));
    if(epoch!==defectEpoch)return;defectPage=result;const run=result.run,imported=defectService(id)==='frames';
    $('defect-runs').value=id;$('defect-dimensions').textContent=`${run.width.toLocaleString()} × ${run.height.toLocaleString()}`;
    $('defect-count').textContent=result.pagination.total.toLocaleString();const peak=run.retained_input_cache_peak_bytes;
    $('defect-cache').textContent=typeof peak==='number'?`${(peak/1048576).toFixed(2)} MiB`:'见运行记录';$('defect-axis').textContent='x = '+run.width;
    const step=run.images?.corrected?.downsample_step||Math.max(1,Math.ceil(run.width/2048));
    $('defect-provenance').textContent=`${imported?'本地导入文件 '+run.display_name+' · 电脑原生执行':run.provenance==='recorded-replay'?'随包记录回放 · 合成输入':'本次电脑原生执行 · 合成输入'} · ${run.width} × ${run.height} · 预览步长 ${step} · r=${run.radius} / 阈值 ${run.threshold} / ${run.polarity} · 未验证硬件来源 · 未作合格/缺陷判定`;
    $('defect-layer').querySelector('[value="source"]').disabled=!imported;if(!imported&&$('defect-layer').value==='source')$('defect-layer').value='corrected';
    $('defect-layer').disabled=false;populateDefectRows();$('defect-export').disabled=!defectOnline;
    if(result.components.length){defectSelected=result.components[0];}else $('defect-coordinates').textContent='本参数组合没有候选。无候选不等于玻璃合格。';
    await reloadDefectLayer();
  }catch(e){if(epoch===defectEpoch){desiredDefectRun='';resetDefectDisplay('结果读取失败；请手动重新选择记录');setDefectStatus('结果读取失败',true);toast(e.message,true);}}
}
async function pollDefects(){
  if(defectPolling)return;defectPolling=true;const commandEpoch=defectCommandEpoch;
  try{
    const [synthetic,frames]=await Promise.all([api('defects/state'),api('frames/state')]);if(commandEpoch!==defectCommandEpoch)return;defectOnline=true;defectBusy=synthetic.busy||frames.busy;
    const completed=[...synthetic.runs,...frames.runs].filter(run=>run.status==='complete').sort((a,b)=>b.created_at-a.created_at);
    const signature=JSON.stringify(completed.map(run=>[run.id,run.component_count]));
    if(signature!==defectRunsSignature){defectRunsSignature=signature;$('defect-runs').replaceChildren(new Option(completed.length?'手动选择一份历史结果':'尚无完成结果',''));for(const run of completed){const source=defectService(run.id)==='frames'?'导入 · '+run.display_name:run.provenance==='recorded-replay'?'合成随包回放':'合成原生演示';$('defect-runs').add(new Option(`${source} · ${new Date(run.created_at*1000).toLocaleTimeString()} · ${run.component_count} 候选`,run.id));}}
    if(completed.some(r=>r.id===defectRun))$('defect-runs').value=defectRun;
    const inputsSignature=JSON.stringify(frames.inputs.map(input=>[input.input_id,input.display_name]));
    if(inputsSignature!==defectInputsSignature&&!defectCommandBusy){const selected=$('defect-input').value;defectInputsSignature=inputsSignature;$('defect-input').replaceChildren(new Option('选择已导入图像',''));for(const input of frames.inputs)$('defect-input').add(new Option(`${input.display_name} · ${input.width} × ${input.height}`,input.input_id));if(frames.inputs.some(input=>input.input_id===selected))$('defect-input').value=selected;}
    updateDefectControls();if(defectCommandBusy)return;
    const requested=pendingRun&&[...synthetic.runs,...frames.runs].find(run=>run.id===pendingRun);
    if(pendingRun&&!requested&&!defectBusy){pendingRun='';desiredDefectRun='';defectFailure='本次任务记录已不可用，请重新运行；历史结果需手动选择。';resetDefectDisplay(defectFailure);updateDefectControls();}
    const active=requested||(synthetic.busy?synthetic.current:frames.busy?frames.current:($('defect-source').value==='frames'?frames.current:synthetic.current));
    if(defectBusy){setDefectStatus('正在处理');$('defect-message').textContent='正在电脑执行原生计算与跨条带合并。完成后显示本次结果。';}
    else if(defectFailure){setDefectStatus('本次操作未完成',true);$('defect-message').textContent=defectFailure;}
    else if(active?.status==='failed'){setDefectStatus('运行失败',true);$('defect-message').textContent=active.error||'任务失败，请查看运行日志。';if(pendingRun===active.id){pendingRun='';desiredDefectRun='';resetDefectDisplay('本次任务失败。历史结果请手动选择。');}}
    else{setDefectStatus(active?.status==='complete'?'处理完成':'等待运行');$('defect-message').textContent='可使用合成输入，或导入从采集端复制回电脑的 P5 Mono8 文件。导入不验证相机与板卡来源。';}
    if(pendingRun&&completed.some(r=>r.id===pendingRun)){const id=pendingRun;pendingRun='';await showDefectRun(id);}
    else if(!defectRun&&desiredDefectRun&&!defectBusy&&completed.some(r=>r.id===desiredDefectRun))await showDefectRun(desiredDefectRun);
  }catch(e){if(commandEpoch===defectCommandEpoch)invalidateDefects(e.message);}
  finally{defectPolling=false;}
}
$('defect-source').onchange=updateDefectControls;
$('defect-input').onchange=updateDefectControls;
$('defect-upload').onchange=async e=>{
  const file=e.target.files[0];if(!file)return;
  if(defectBusy||defectCommandBusy||!defectOnline){e.target.value='';return;}
  defectFailure='';defectCommandBusy=true;++defectCommandEpoch;updateDefectControls();setDefectStatus('正在导入');
  try{
    if(file.size>8*1048576+4096)throw new Error('导入限制为 8 MiB 像素加 PGM 文件头。');
    const response=await fetch('/api/frames/upload?'+new URLSearchParams({name:file.name}),{method:'POST',headers:{'Content-Type':'application/octet-stream','X-SG-Token':token},body:file});
    const input=await response.json();if(!response.ok)throw new Error(input.error||'导入失败');
    // Insert immediately and preserve this choice across any in-flight state poll.
    if(!Array.from($('defect-input').options).some(o=>o.value===input.input_id))$('defect-input').add(new Option(`${file.name} · ${input.width} × ${input.height}`,input.input_id));
    $('defect-input').value=input.input_id;$('defect-source').value='frames';setDefectStatus('文件已导入');toast('文件已导入，可以开始离线处理；未验证硬件来源。');
  }catch(e){defectFailure=e.message;setDefectStatus('导入失败',true);toast(e.message,true);}
  finally{e.target.value='';defectCommandBusy=false;++defectCommandEpoch;updateDefectControls();await pollDefects();}
};
$('defect-start').onclick=async()=>{
  if(defectBusy||defectCommandBusy||!defectOnline)return;
  const threshold=Number($('defect-threshold').value);if(!Number.isInteger(threshold)||threshold<1||threshold>255){toast('阈值需要是 1 到 255 的整数。',true);return;}
  const service=$('defect-source').value,inputId=$('defect-input').value;if(service==='frames'&&!inputId){toast('请先导入并选择 PGM 图像。',true);return;}
  defectFailure='';defectCommandBusy=true;++defectCommandEpoch;pendingRun='';desiredDefectRun='';resetDefectDisplay('本次任务启动中…');updateDefectControls();setDefectStatus('正在启动');
  try{const data={radius:Number($('defect-radius').value),threshold,polarity:$('defect-polarity').value};if(service==='frames')data.input_id=inputId;const job=await api(service+'/start',data);pendingRun=job.id;}
  catch(e){defectFailure=e.message;resetDefectDisplay('本次任务启动失败。历史结果请手动选择。');setDefectStatus('启动失败',true);toast(e.message,true);}
  finally{defectCommandBusy=false;++defectCommandEpoch;updateDefectControls();await pollDefects();}
};
$('defect-replay').onclick=async()=>{
  if(defectBusy||defectCommandBusy||!defectOnline)return;
  defectFailure='';defectCommandBusy=true;++defectCommandEpoch;pendingRun='';desiredDefectRun='';resetDefectDisplay('正在读取随包合成回放…');updateDefectControls();
  try{const run=await api('defects/replay',{});await showDefectRun(run.id);}catch(e){defectFailure=e.message;resetDefectDisplay('回放失败。历史结果请手动选择。');setDefectStatus('回放失败',true);toast(e.message,true);}finally{defectCommandBusy=false;++defectCommandEpoch;updateDefectControls();await pollDefects();}
};
$('defect-runs').onchange=()=>{defectFailure='';const id=$('defect-runs').value;pendingRun='';desiredDefectRun=id;if(id)showDefectRun(id);else resetDefectDisplay();};
$('defect-prev').onclick=()=>showDefectRun(defectRun,Math.max(0,defectOffset-100));
$('defect-next').onclick=()=>showDefectRun(defectRun,defectOffset+100);
$('defect-layer').onchange=reloadDefectLayer;
$('defect-export').onclick=async()=>{
  if(!defectPage||!defectOnline)return;const id=defectRun,epoch=defectEpoch,button=$('defect-export');button.disabled=true;
  try{
    let blob;
    if(defectService(id)==='frames'){const response=await fetch('/api/frames/export?'+new URLSearchParams({id}));if(!response.ok){const error=await response.json();throw new Error(error.error||'导出失败');}blob=await response.blob();}
    else{let offset=0,components=[],metadata;while(true){const page=await api('defects/results?'+new URLSearchParams({id,offset:String(offset),limit:'100'}));if(epoch!==defectEpoch||id!==defectRun)return;metadata=page.run;components.push(...page.components);if(!page.pagination.has_more)break;if(!page.pagination.returned)throw new Error('结果分页未前进，导出已中止。');offset+=page.pagination.returned;}blob=new Blob([JSON.stringify({schema_version:'defects-export-v1',run:metadata,components},null,2)],{type:'application/json'});}
    if(epoch!==defectEpoch||id!==defectRun||!defectOnline)return;
    const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=id+'-candidates.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1500);toast('候选结果已导出，保留了来源和全局坐标。');
  }catch(e){if(epoch===defectEpoch){desiredDefectRun='';invalidateDefects(e.message);toast(e.message,true);}}
  finally{if(epoch===defectEpoch)button.disabled=!defectPage||!defectOnline;}
};
document.addEventListener('workbench-disconnected',()=>invalidateDefects('本机服务连接中断；连接恢复后重新读取当前记录。'));
document.addEventListener('workbench-ready',()=>{updateDefectControls();pollDefects();setInterval(pollDefects,1000);});
