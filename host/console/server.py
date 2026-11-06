"""Local-only SG3000 control center, stdlib HTTP + pyserial."""
import argparse
from datetime import datetime
import hashlib
import json
import mimetypes
import secrets
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from transport import Console
from defects import DefectService
from offline_frames import FrameService, MAX_UPLOAD

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'console' / 'web'
TOKEN = secrets.token_urlsafe(32)
FILES = {
    'firstframe': ('docs/第一帧到结果.md', 1, '第一帧到结果', '相机到位后的验证顺序、图像复制与离线检测入口。'),
    'frames': ('docs/真实图像离线接入.md', 1, '第一帧图像与检测接入', '上传 Mono8 PGM，执行原生检测，保留输入摘要、坐标和结果来源。'),
    'frameschema': ('interfaces/frame-candidates-v1.schema.json', 1, '实际图像候选合同', '真实文件离线计算，不代表板上 DMA 或 PL 已通过。'),
    'integration': ('docs/离线检测接口.md', 1, '离线检测与后续接入', 'v0.3 离线任务、候选结果合同与实机接入边界。'),
    'defects': ('console/defects.py', 1, '检测任务与结果适配', '真实调用同包原生模块，验证完成产物，分页显示候选。'),
    'defectschema': ('interfaces/defects-v1.schema.json', 1, '缺陷候选结果格式', '候选不是质量判定；坐标为全局半开区间，保留来源。'),
    'tilecontract': ('../linescan_preprocess_v2/contract.json', 1, 'FPGA 预处理合同', 'packed Mono8、signed9、halo、控制寄存器和错误状态。'),
    'tilehls': ('../linescan_preprocess_v2/hls/preprocess_tile.cpp', 1, 'FPGA 行缓存与窗口', '已综合并通过 RTL 协同仿真的预处理源码。'),
    'tileruntime': ('../linescan_preprocess_v2/python/stream_pipeline.py', 1, '条带与横向拼接', '复用已验证模块，按全局行序恢复核心区。'),
    'tilemmio': ('../linescan_preprocess_v2/python/mmio_runtime.py', 1, '真实硬件后端合同', '后续实现 DMA 映射与缓存同步；本演示不执行此硬件路径。'),
    'entry': ('core/sg3000_capture.c', 120, '应用入口', 'main 决定检查或采集；152 行打开驱动，166 行检查，172 行采集，180 行保存。'),
    'flow': ('core/sg3000_capture_core.c', 149, '连续接收核心', 'begin 只启动一次，next 连续取块，end 统一停止；原单帧入口复用这三个阶段。'),
    'stream': ('core/sg_stream_main.c', 1, '连续程序入口', '硬件或合成输入、处理后端选择、SGJSON 结果输出和停止状态。'),
    'queue': ('core/sg_pipeline.c', 1, '接收与处理队列', '有界队列、缓冲区所有权、队列满时停止、处理与输出错误传播。'),
    'processing': ('core/sg_processing.h', 1, '算法交接接口', '图像尺寸、行偏移、编号、时间戳及结果结构；指针不等于 DMA 地址。'),
    'plabi': ('core/sg_pl_uapi.h', 1, 'FPGA 驱动接口约定', '待实现的 PL 内核驱动 ABI。尚未分配物理地址，不代表 FPGA 硬件已连接。'),
    'hls': ('fpga/sg_image_stats.cpp', 1, 'HLS 参考模块', '图像与结果通过 AXI memory 接口交接；灰度统计是参考算法，不是缺陷判定。'),
    'interfaces': ('docs/连续采集与处理接口.md', 1, '连续采集与接口说明', '数据路径、部署、输出格式与硬件验证边界。'),
    'driver': ('core/sg3000_rx.c', 102, 'DMA 驱动', '102 行提交 DMA；158 行处理 ioctl；224 行识别并初始化 PCIe 卡。'),
    'abi': ('core/sg3000_rx_uapi.h', 8, '应用与驱动接口', 'sg_dma 描述源地址、目标缓存、长度、超时和完成状态。'),
    'types': ('core/sg3000_capture_core.h', 13, '数据结构', '硬件接口、会话、帧结构与采集结果。'),
    'resume': ('board/resume.sh', 1, '板上启动脚本', '校验文件、加载驱动、执行 preflight；不会自动采图。'),
    'guide': ('docs/使用说明.md', 1, '使用说明', '从连接到采集的操作步骤，以及当前限制。'),
    'build': ('build/receiver.ps1', 1, '应用编译脚本', '使用 Vitis 2022.1 交叉编译器；产物进入 build/output，不覆盖已验证程序。'),
    'module': ('build/module.sh', 1, '驱动编译脚本', '需要外部准备好的固定版本 Linux 内核树，以及 Image / symbols.json。'),
    'transport': ('console/transport.py', 1, '串口与操作实现', '串口独占、命令完成标记、超时、板卡检查和采集操作。'),
    'server': ('console/server.py', 1, '中控台服务', '仅监听本机，提供状态、操作、日志和源码接口。'),
    'frontend': ('console/web/app.js', 1, '中控台界面逻辑', '界面状态更新、源码阅读与本地 PGM 查看。'),
    'history': ('evidence/load-and-preflight.log', 1, '历史上板日志', '2026-10-23 的历史实测；不能代表当前连接或硬件状态。'),
    'saved': ('evidence/sd-saved-verified.log', 1, 'SD 保存与读回', '历史日志中的哈希核对、SD 卸载和驱动卸载记录。'),
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, code, data, kind='application/json; charset=utf-8'):
        if isinstance(data, (dict, list)):
            data = json.dumps(data, ensure_ascii=False).encode('utf-8')
        elif isinstance(data, str):
            data = data.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' blob: data:; connect-src 'self'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(data)

    def local_request(self):
        allowed = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
        if self.headers.get('Host') not in allowed:
            self.send(403, {'error': '仅允许本机访问。'})
            return False
        origin = self.headers.get('Origin')
        if origin and origin not in {'http://' + h for h in allowed}:
            self.send(403, {'error': '跨站请求已拒绝。'})
            return False
        return True

    def do_GET(self):
        if not self.local_request():
            return
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        try:
            if parsed.path == '/api/bootstrap':
                self.send(200, dict(token=TOKEN, root=str(ROOT), version='0.4', bundle=str(ROOT.parent), files=[dict(id=k, path=v[0], line=v[1], title=v[2], guide=v[3]) for k, v in FILES.items()]))
            elif parsed.path == '/api/frames/state':
                self.send(200, self.server.frames.state())
            elif parsed.path == '/api/frames/results':
                self.send(200, self.server.frames.results(query.get('id',[''])[0], int(query.get('offset',['0'])[0]), int(query.get('limit',['100'])[0])))
            elif parsed.path == '/api/frames/export':
                self.send(200, self.server.frames.export(query.get('id',[''])[0]))
            elif parsed.path == '/api/frames/image':
                options={name:int(query[name][0]) for name in ('x','y','width','height') if name in query}
                self.send(200, self.server.frames.image(query.get('id',[''])[0],query.get('kind',['source'])[0],**options), 'image/png')
            elif parsed.path == '/api/defects/state':
                self.send(200, self.server.defects.state())
            elif parsed.path == '/api/defects/results':
                self.send(200, self.server.defects.results(query.get('id',[''])[0], int(query.get('offset',['0'])[0]), int(query.get('limit',['100'])[0])))
            elif parsed.path == '/api/defects/image':
                options={name:int(query[name][0]) for name in ('x','y','width','height') if name in query}
                self.send(200, self.server.defects.image(query.get('id',[''])[0],query.get('kind',['corrected'])[0],**options), 'image/png')
            elif parsed.path == '/api/ports':
                self.send(200, self.server.console.ports())
            elif parsed.path == '/api/state':
                self.send(200, self.server.console.state(int(query.get('after', ['0'])[0])))
            elif parsed.path == '/api/source':
                file = FILES[query.get('id', ['entry'])[0]]
                path = ROOT / file[0]
                self.send(200, dict(path=str(path), text=path.read_text(encoding='utf-8', errors='replace'), line=file[1], guide=file[3]))
            elif parsed.path == '/api/log':
                self.send(200, self.server.console.log_path.read_bytes(), 'text/plain; charset=utf-8')
            elif parsed.path == '/api/addon':
                self.send(200, (ROOT/'board'/'sg3000-stream-v0.2.zip').read_bytes(), 'application/zip')
            elif parsed.path == '/api/captures/preview':
                name = Path(query.get('name', [''])[0]).name
                captures_dir = Path('D:/SG3000_Captures')
                preview_file = captures_dir / (Path(name).stem + '_preview.png')
                if preview_file.is_file():
                    self.send(200, preview_file.read_bytes(), 'image/png')
                else:
                    pgm_file = captures_dir / name
                    if not pgm_file.is_file() and not name.endswith('.pgm'):
                        pgm_file = captures_dir / (name + '.pgm')
                    if pgm_file.is_file():
                        from PIL import Image
                        import io
                        img = Image.open(str(pgm_file))
                        if img.mode != 'L':
                            img = img.convert('L')
                        img.save(preview_file, format='PNG')
                        buf = io.BytesIO()
                        img.save(buf, format='PNG')
                        self.send(200, buf.getvalue(), 'image/png')
                    else:
                        self.send(404, {'error': '图像未找到。'})
            elif parsed.path == '/api/captures/list':
                captures_dir = Path('D:/SG3000_Captures')
                captures_dir.mkdir(parents=True, exist_ok=True)
                items = []
                for p in sorted(captures_dir.glob('*.pgm'), key=lambda x: x.stat().st_mtime, reverse=True):
                    items.append(dict(name=p.name, size=p.stat().st_size, time=datetime.fromtimestamp(p.stat().st_mtime).isoformat(timespec='seconds')))
                self.send(200, dict(files=items))
            elif parsed.path in {'/', '/index.html', '/app.js', '/defects.js', '/style.css'}:
                path = WEB / ('index.html' if parsed.path == '/' else parsed.path[1:])
                kind = {'.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css'}[path.suffix]
                self.send(200, path.read_bytes(), kind + '; charset=utf-8')
            else:
                self.send(404, {'error': '入口不存在。'})
        except (ValueError, KeyError, OSError, RuntimeError) as exc:
            self.send(400, {'error': str(exc)})

    def do_POST(self):
        if not self.local_request():
            return
        if not secrets.compare_digest(self.headers.get('X-SG-Token', ''), TOKEN):
            self.send(403, {'error': '页面会话已失效，请刷新。'})
            return
        try:
            self.connection.settimeout(15)
            if self.headers.get('Transfer-Encoding'):
                raise ValueError('请使用有明确长度的请求正文。')
            size = int(self.headers.get('Content-Length', '0'))
            parsed = urlparse(self.path)
            if parsed.path == '/api/frames/upload':
                if size < 1 or size > MAX_UPLOAD:
                    raise ValueError('图像超过限制：最多 8 MiB 像素加 PGM 文件头。')
                if self.headers.get_content_type() not in ('application/octet-stream', 'image/x-portable-graymap'):
                    raise ValueError('上传必须为 P5 Mono8 PGM 原始正文。')
                query = parse_qs(parsed.query)
                self.send(201, self.server.frames.register_pgm(self.rfile, size, query.get('name',['image.pgm'])[0]))
                return
            if size < 0 or size > 4096:
                raise ValueError('请求过大。')
            body = self.rfile.read(size)
            if len(body) != size:
                raise ValueError('请求正文不完整。')
            data = json.loads(body or '{}')
            if not isinstance(data,dict): raise ValueError('请求必须为 JSON 对象。')
            c = self.server.console
            if self.path in ('/api/defects/start', '/api/frames/start'):
                with self.server.offline_lock:
                    if self.server.defects.state()['busy'] or self.server.frames.state()['busy']:
                        raise ValueError('已有离线检测任务，请等待完成。')
                    service = self.server.frames if self.path == '/api/frames/start' else self.server.defects
                    result = service.start(data)
                self.send(202, result)
                return
            elif self.path == '/api/defects/replay':
                self.send(200, self.server.defects.replay())
                return
            elif self.path == '/api/connect':
                c.connect(str(data.get('port', '')))
            elif self.path == '/api/disconnect':
                c.disconnect()
            elif self.path == '/api/wake':
                c.wake()
            elif self.path == '/api/interrupt':
                c.interrupt()
            elif self.path == '/api/action':
                c.start(data.get('action'), data.get('options'))
            elif self.path == '/api/shutdown':
                if c.busy or self.server.defects.state()['busy'] or self.server.frames.state()['busy']:
                    raise ValueError('操作进行中，不能关闭服务。请先等待完成或中断。')
                if c.connected:
                    c.disconnect()
                self.send(200, {'ok': True})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            else:
                self.send(404, {'error': '未知操作。'})
                return
            self.send(200, {'ok': True})
        except Exception as exc:
            self.send(400, {'error': str(exc)})


def make_server(port=8767, console=None, defects=None, frames=None):
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.console = console or Console(ROOT / 'runtime')
    server.defects = defects or DefectService(ROOT.parent, ROOT / 'runtime' / 'defects')
    server.frames = frames or FrameService(ROOT.parent)
    server.offline_lock = threading.Lock()
    return server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8767)
    parser.add_argument('--open', action='store_true')
    args = parser.parse_args()
    try:
        server = make_server(args.port)
    except OSError as exc:
        raise SystemExit('Cannot start local console; port may be occupied: ' + str(exc))
    url = f'http://127.0.0.1:{server.server_port}/'
    print(url, flush=True)
    if args.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    finally:
        if server.console.connected and not server.console.busy:
            server.console.disconnect()
        server.server_close()


if __name__ == '__main__':
    main()
