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
from transport import CAPTURES

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'console' / 'web'
TOKEN = secrets.token_urlsafe(32)
REPO = ROOT.parent
# 「核心代码」页面能打开的文件（路径相对仓库根目录）
FILES = {
    'entry': ('board/apps/sg_capture_main.c', 1, '单帧程序入口', '解析参数 → 打开设备 → 预检 / 采一帧 → 存 PGM → 可选经网口发送。'),
    'acq': ('board/capture/sg_acq.c', 1, '采集生命周期', 'begin 只启动一次，next 连续取块，end 统一停止；单帧 = begin + next + end。'),
    'block': ('board/capture/sg_block.c', 1, '块头与分块 DMA', '等块就绪 → 读块头 → 校验 → 64 KiB 一次 DMA。'),
    'mailbox': ('board/capture/sg_mailbox.c', 1, '邮箱协议', '和卡上固件对话：发送、等待、取回、解析。'),
    'preflight': ('board/capture/sg_preflight.c', 1, '只读预检', 'DDR、时钟、CL 配置、彩色、位深五项。'),
    'iolinux': ('board/capture/sg_io_linux.c', 1, '回调表的 Linux 实现', '加锁、打开设备、INFO 检查，6 个回调翻译成 ioctl。'),
    'stream': ('board/apps/sg_stream_main.c', 1, '连续程序入口', '硬件或合成输入、处理器选择、SGJSON 输出、可选网口。'),
    'queue': ('board/stream/sg_pipeline.c', 1, '接收与处理队列', '有界队列、缓冲区所有权、队列满时停止、错误传播。'),
    'processing': ('board/processing/sg_processing.h', 1, '处理器接口', '图像视图、处理结果、处理器回调表。'),
    'candidates': ('board/processing/sg_candidates.c', 1, '颗粒候选检测', '列背景校正 → 阈值 → 8 邻接连通域 → 候选列表。'),
    'tcp': ('board/transport/tx_tcp.c', 1, '网口发送', 'I6 协议：32 字节头 + 负载，CRC32。'),
    'receiver': ('host/receiver/rx_tcp.py', 1, '网口接收', '电脑端校验并落盘；中控台内置同一份代码。'),
    'driver': ('board/driver/sg3000_rx.c', 1, 'DMA 驱动', 'PCIe 绑定、寄存器白名单、DMA、出错隔离。'),
    'abi': ('include/sg3000_rx_uapi.h', 1, '应用与驱动接口', '4 个 ioctl 的结构体。'),
    'regs': ('include/sg3000_regs.h', 1, '寄存器表', '所有地址和魔数，每条标可信度。'),
    'architecture': ('ARCHITECTURE.md', 1, '架构总览', '全链路、依赖方向、每个文件的状态。'),
    'guide': ('docs/BRINGUP.md', 1, '上板步骤', '从拷贝 SD 卡到采到第一帧、连续检测。'),
    'transport': ('host/console/transport.py', 1, '串口与操作实现', '串口独占、命令完成标记、超时、板卡检查和采集操作。'),
    'server': ('host/console/server.py', 1, '中控台服务', '仅监听本机，提供状态、操作、日志和源码接口。'),
    # 页面里按钮引用的旧编号，指向新仓库里对应的文件
    'integration': ('ARCHITECTURE.md', 1, '架构总览', '全链路、依赖方向、每个文件的状态。'),
    'firstframe': ('docs/BRINGUP.md', 1, '上板步骤', '从拷贝 SD 卡到采到第一帧。'),
    'flow': ('board/capture/sg_acq.c', 1, '采集生命周期', 'begin / next / end。'),
    'frames': ('host/detect/candidates.py', 1, '候选检测（Python 参考）', '和板上 C 同一算法，可离线跑本地 PGM。'),
    'frameschema': ('docs/interfaces/schemas/frame-candidates-v1.schema.json', 1, '离线候选结果格式', '中控台离线检测用的格式。'),
    'tilecontract': ('fpga/linescan_preprocess_v2/contract.json', 1, 'FPGA 预处理合同', 'packed Mono8、signed9、halo、控制寄存器和错误状态。'),
    'history': ('docs/decisions/D003_no_binary_patch.md', 1, '历史：二进制补丁', '旧程序靠打补丁跳过 DDR 检查；新程序不再需要。历史上板日志不在仓库里。'),
    'saved': ('docs/BRINGUP.md', 1, '上板步骤', '历史 SD 读回日志不在仓库里；按新步骤重新验证。'),
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
                path = REPO / file[0]
                self.send(200, dict(path=str(path), text=path.read_text(encoding='utf-8', errors='replace'), line=file[1], guide=file[3]))
            elif parsed.path == '/api/log':
                self.send(200, self.server.console.log_path.read_bytes(), 'text/plain; charset=utf-8')
            elif parsed.path == '/api/captures/preview':
                name = Path(query.get('name', [''])[0]).name
                captures_dir = CAPTURES
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
                captures_dir = CAPTURES
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
        except (ValueError, KeyError, OSError, RuntimeError, ImportError) as exc:
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
