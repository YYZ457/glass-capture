"""Single-owner serial transport. No connection or command runs on import."""
import codecs
import gzip
import json
import re
import threading
import time
import uuid
import copy
import ipaddress
import os
import sys
from collections import deque
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports
from telemetry import parse as parse_telemetry

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'receiver'))
import rx_tcp  # noqa: E402

KERNEL = '5.15.19-xilinx-v2022.1'
DEVICE = '/sys/bus/pci/devices/0000:01:00.0'
DEV_NODE = '/dev/sg3000_rx_0000:01:00.0'
BOARD_DIR = '/tmp/sg3000-board'
BOARD_CAPTURES = '/tmp/sg3000-console'   # 板上存采集图的目录（内存盘，断电即失）
APP = BOARD_DIR + '/sg3000-capture'
STREAM_APP = BOARD_DIR + '/sg3000-stream'
STATUS_APP = BOARD_DIR + '/sg_status'
CARD_XML_DIR = BOARD_DIR + '/card-xml'   # 板上存卡 XML 的目录
CAMERA_XML_DIR = BOARD_DIR + '/camera-xml'   # 板上存相机 XML 的目录
CAM_SERIAL_PARAM = '/sys/module/sg3000_rx/parameters/cam_serial'
DMA_FAST_PARAM = '/sys/module/sg3000_rx/parameters/dma_fast'
APP_VERSION = 'sg3000-capture 1.0 ABI 1'
STREAM_VERSION = 'sg3000-stream 0.2 ABI 1'

# SD 卡第一个分区上的目录：
#   glass-capture/          本仓库编译的程序（tools/make_sd_bundle.py 生成）
#   sg3000-linux-20261023/  旧目录，只从这里取驱动 sg3000_rx.ko（新目录里没有 .ko 时）
SD_MOUNT = '/mnt/sg3000-console'
SD_BUNDLE = SD_MOUNT + '/glass-capture'
SD_LEGACY = SD_MOUNT + '/sg3000-linux-20261023'

NET_PORT = 8770
HEXDUMP = '/usr/bin/hexdump'               # 串口回传文件时用板上的 hexdump 转成十六进制文本
CAPTURES = Path(os.environ.get('SG_CAPTURES_DIR') or ('D:/SG3000_Captures' if os.name == 'nt' else Path.home() / 'SG3000_Captures'))
PROMPT = re.compile(r'(?:^|[\r\n])[^\r\n]{0,140}# ?$')


class Console:
    def __init__(self, runtime, serial_factory=None):
        self.runtime = Path(runtime)
        self.runtime.mkdir(parents=True, exist_ok=True)
        self.serial_factory = serial_factory
        self.lock = threading.RLock()
        self.cv = threading.Condition(self.lock)
        self.wire_lock = threading.Lock()
        self.port = None
        self.reader = None
        self.rx = ''
        self.events = deque(maxlen=2000)
        self.seq = 0
        self.cancel = threading.Event()
        self.busy = False
        self.connected = False
        self.shell_ready = False
        self.identity_ok = False
        self.port_name = ''
        self.job = None
        captures_dir = CAPTURES
        captures_dir.mkdir(parents=True, exist_ok=True)
        pgms = sorted(captures_dir.glob('*.pgm'), key=lambda p: p.stat().st_mtime, reverse=True)
        if pgms:
            latest = pgms[0]
            self.last_frame = dict(
                name=latest.name,
                path=f"{BOARD_CAPTURES}/{latest.name}",
                location='local-pc',
                local_path=str(latest),
                size_bytes=latest.stat().st_size,
                at=datetime.fromtimestamp(latest.stat().st_mtime).isoformat(timespec='seconds'),
                width=4096,
                height=2000
            )
        else:
            self.last_frame = None
        self.checks = {}
        self.stream = {}
        self.stream_lines = ''
        self.silent_rx = False
        self.net = {}                 # 配置好网口后：pc_ip、board_ip、port
        self.net_frames = deque(maxlen=50)
        self.net_candidates = 0
        self.receiver = None
        self.camera = {}              # 相机参数（camera-get / camera-set --params 读回的），名字 → dict
        self.live = {}                # 最近一次"采一帧看轮廓"的结果 + 历史
        self.live_stop = False
        self.live_sets = []           # 实时调试时排队的相机参数修改（两帧之间执行）
        self.bench = []               # sg3000-capture bench 的结果
        self.driver = {}              # cam_serial / dma_fast 当前值
        self.log_path = self.runtime / ('session-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8] + '.jsonl')
        self.event('system', '中控台已启动。尚未连接设备；历史记录不代表当前状态。')

    def event(self, kind, text):
        with self.lock:
            self.seq += 1
            item = dict(seq=self.seq, time=datetime.now().isoformat(timespec='seconds'), kind=kind, text=text)
            self.events.append(item)
            with self.log_path.open('a', encoding='utf-8') as f:
                f.write(json.dumps(item, ensure_ascii=False) + '\n')

    def ports(self):
        return [dict(device=p.device, description=p.description, serial=p.serial_number,
                     suggested=p.vid == 0x10C4) for p in list_ports.comports()]

    def state(self, after=0):
        with self.lock:
            return copy.deepcopy(dict(connected=self.connected, port=self.port_name, shell_ready=self.shell_ready,
                        identity_ok=self.identity_ok, busy=self.busy, job=self.job,
                        checks=dict(self.checks), last_frame=self.last_frame, seq=self.seq,
                        stream=dict(self.stream), net=dict(self.net), captures=str(CAPTURES),
                        camera=self.camera, live=dict(self.live, pending=[o['name'] for o in self.live_sets]), bench=self.bench, driver=self.driver,
                        events=[e for e in self.events if e['seq'] > after]))

    def _stream_unknown(self, message):
        if self.stream.get('channel')=='board' and self.stream.get('status') in {'starting','running'}:
            self.stream.update(status='unknown',error=message)

    def connect(self, name):
        with self.lock:
            if self.connected or self.busy:
                raise ValueError('请先断开当前连接。')
            if self.reader and self.reader.is_alive():
                raise ValueError('旧串口读取线程尚未退出，请稍后再连接。')
            if not re.fullmatch(r'COM\d+', name, re.I):
                raise ValueError('请选择有效 COM 端口。')
            if self.serial_factory:
                p = self.serial_factory(name)
            else:
                p = serial.Serial(port=None, baudrate=115200, timeout=0.08,
                                  write_timeout=2, rtscts=False, dsrdtr=False, xonxoff=False)
                p.port = name
                p.dtr = False
                p.rts = False
                p.open()
            self.port = p
            self.port_name = name
            self.connected = True
            self.shell_ready = False
            self.identity_ok = False
            self.checks = {}
            self.last_frame = None
            self.stream = {}
            self.stream_lines = ''
            self.rx = ''
            self.reader = threading.Thread(target=self._read, args=(p,), daemon=True)
            self.reader.start()
            self.event('system', f'已打开 {name}，115200 / 8N1。点击「唤醒终端」等待 Linux root 提示符。')

    def _read(self, port):
        decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        try:
            while self.connected and self.port is port:
                chunk = port.read(4096)
                if not chunk:
                    continue
                text = decoder.decode(chunk)
                with self.cv:
                    self.rx = (self.rx + text)[-25_000_000:]
                    if self.job and self.job.get('action') in {'stream', 'short_record'} and self.busy:
                        self.stream_lines = (self.stream_lines + text)[-12000:]
                        while '\n' in self.stream_lines:
                            line, self.stream_lines = self.stream_lines.split('\n', 1)
                            self.accept_stream_line(line.strip(), 'board')
                    if re.search(r'U-Boot \d|Linux version \d', self.rx[-500:]):
                        self.identity_ok = False
                        self.checks = {}
                        self.last_frame = None
                        self._stream_unknown('检测到板端重新启动；本次接收完整性未知。')
                    if not self.busy:
                        self.shell_ready = bool(PROMPT.search(self.rx[-300:]))
                    if not self.silent_rx:
                        self.event('rx', text)
                    self.cv.notify_all()
        except Exception as exc:
            with self.cv:
                self.connected = False
                self.shell_ready = False
                self.identity_ok = False
                self._stream_unknown('串口连接丢失；未确认完整接收和停止。')
                self.event('error', '串口断开：' + str(exc))
                self.cv.notify_all()
        finally:
            port.close()

    def disconnect(self):
        with self.lock:
            if self.busy:
                raise ValueError('操作仍在运行。先点中断并等待结果，避免丢失停止状态。')
            self.connected = False
            self.shell_ready = False
            self.identity_ok = False
            self._stream_unknown('连接已断开；未确认完整接收和停止。')
            if self.port:
                self.port.close()
            self.event('system', '串口已断开。下面的检查结果属于断开前的记录。')

    def _write(self, data):
        with self.wire_lock:
            if not self.connected:
                raise ValueError('串口未连接。')
            for offset in range(0, len(data), 32):
                piece=data[offset:offset+32]
                written=0
                deadline=time.monotonic()+2
                while written<len(piece):
                    if time.monotonic()>=deadline:raise TimeoutError('串口写入超时，命令可能不完整。')
                    count=self.port.write(piece[written:])
                    if type(count) is not int or not 0<count<=len(piece)-written:
                        raise ConnectionError('串口未完整写入，命令状态未知。')
                    written+=count
                time.sleep(0.004)
            try:
                self.port.flush()
            except Exception:
                pass

    def wake(self):
        if self.busy:
            raise ValueError('操作进行中。')
        self._write(b'\r\n')

    def interrupt(self):
        if not self.busy:
            raise ValueError('没有正在运行的操作。')
        self.cancel.set()
        if self.job and self.job.get('action') == 'replay':
            self.event('system', '已停止离线回放；未向串口发送命令。')
            return
        self._write(b'\x03')
        self.event('system', '已发送 Ctrl+C，正在等待程序清理。此操作不等于已确认 DMA 停止。')

    def exchange(self, command, timeout=12, allow_cancel=False):
        if self.cancel.is_set() and not allow_cancel:
            raise InterruptedError('操作已中断。')
        tag = uuid.uuid4().hex[:12]
        begin, end = '__SG_BEGIN_' + tag + '__', '__SG_END_' + tag + '__'
        wire = f"printf '\\n{begin}\\n'; ( {command} ); _sg_rc=$?; printf '\\n{end}:%s\\n' \"$_sg_rc\"\r"
        if len(wire.encode('ascii')) > 1000:
            raise ValueError('命令超过串口单行限制。')
        with self.cv:
            self.rx = ''
        self.event('tx', command)
        try:
            self._write(wire.encode('ascii'))
        except Exception:
            with self.lock:
                self.shell_ready=False;self.identity_ok=False
                self._stream_unknown('命令写入失败；板端状态未知。')
            raise
        deadline = time.monotonic() + timeout
        cancel_deadline = None
        with self.cv:
            while True:
                if not self.connected:
                    raise ConnectionError('连接丢失，板上操作结果未知。')
                match = re.search(r'(?:\r?\n)' + end + r':(\d+)\r?\n', self.rx)
                if match:
                    start = re.search(r'(?:\r?\n)' + begin + r'\r?\n', self.rx)
                    output = self.rx[start.end() if start else 0:match.start()]
                    # A completed marker proves this shell executed the command.
                    self.shell_ready = True
                    return int(match.group(1)), output
                remaining = deadline - time.monotonic()
                if self.cancel.is_set() and not allow_cancel:
                    if cancel_deadline is None:
                        cancel_deadline = time.monotonic() + 12
                    remaining = min(remaining, cancel_deadline - time.monotonic())
                if remaining <= 0:
                    self.shell_ready = False
                    self.identity_ok = False
                    raise TimeoutError('未收到命令完成标记。板上状态未知，请查看原始日志并唤醒终端；不要立即断电。')
                self.cv.wait(min(remaining, 0.1))

    def run_checked(self, command, timeout=12, allow_cancel=False):
        rc, output = self.exchange(command, timeout, allow_cancel)
        if rc:
            raise RuntimeError(f'板上命令退出码 {rc}。' + output[-1200:])
        return output

    def start(self, action, options=None):
        if action not in {'probe', 'prepare', 'preflight', 'capture', 'fetch_last', 'save_sd', 'dmesg', 'release', 'prepare_stream', 'stream', 'short_record', 'replay', 'status', 'network', 'card_xml', 'card_set', 'camera_probe', 'camera_xml', 'camera_regs', 'camera_set', 'board_session',
                          'camera_get', 'camera_preset', 'snap', 'live', 'driver', 'bench'}:
            raise ValueError('未知操作。')
        with self.lock:
            if action != 'replay' and (not self.connected or not self.shell_ready):
                raise ValueError('先连接并唤醒 Linux root 终端；如出现登录界面，请先用串口工具登录。')
            if self.busy and action == 'camera_set' and self.job and self.job.get('action') == 'live' and self.live.get('running'):
                o = self._check_camera_set(options if isinstance(options, dict) else {})
                if o['name'] not in self.LIVE_ADJUSTABLE:
                    raise ValueError(f"实时调试时只能改曝光、行频、数字位移、增益、黑电平、TDI 和测试图；{o['name']} 要先停止实时调试。")
                self.live_sets = [x for x in self.live_sets if x['name'] != o['name']] + [o]   # 同一参数只留最新值
                self.event('system', f"实时调试中：{o['name']} = {o['text']} 已排队，下一帧开始前写入相机。")
                return
            if self.busy:
                raise ValueError('上一个操作尚未完成。')
            if action not in {'probe', 'dmesg', 'replay', 'fetch_last', 'save_sd'} and not self.identity_ok:
                raise ValueError('先执行「检查板卡」，确认板型、内核和采集卡。')
            options = {} if options is None else options
            if not isinstance(options,dict):raise ValueError('操作参数必须是对象。')
            seconds = options.get('seconds', 30)
            processor = options.get('processor', 'stats')
            if type(seconds) is not int or not 0 <= seconds <= 300 or processor not in {'stats', 'candidates'}:
                raise ValueError('连续时长必须为 0..300 秒，处理后端为 stats 或 candidates。')
            self.stream_options = dict(seconds=seconds, processor=processor)
            if action == 'network':
                self.net_options = self._check_net_options(options)
            if action == 'card_set':
                self.card_set_options = self._check_card_set(options)
            if action == 'camera_set':
                self.camera_set_options = self._check_camera_set(options)
            if action in {'snap', 'live'}:
                fetch, count = options.get('fetch', False), options.get('count', 0)
                if type(fetch) is not bool or type(count) is not int or not 0 <= count <= 10000:
                    raise ValueError('fetch 必须是 true / false，count 必须是 0..10000（0 = 直到点停止）。')
                self.snap_options = dict(fetch=fetch, count=count)
                self.live_stop = False
            if action == 'driver':
                if type(options.get('dma_fast')) is not bool:
                    raise ValueError('dma_fast 必须是 true 或 false。')
                self.driver_options = dict(dma_fast=options['dma_fast'])
            if action == 'short_record':
                short_seconds = options.get('seconds', 20)
                storage = options.get('storage', 'ram_sd')
                if type(short_seconds) is not int or not 1 <= short_seconds <= 60 or storage not in {'ram_sd', 'direct_sd'}:
                    raise ValueError('短录时长必须为 1..60 秒，存储方式为 ram_sd 或 direct_sd。')
                self.short_options = dict(seconds=short_seconds, storage=storage)
            if action in {'stream', 'replay', 'short_record'}:
                self.stream = {'channel': 'recorded-replay' if action == 'replay' else 'board', 'status': 'starting'}
                self.stream_lines = ''
            self.busy = True
            self.cancel.clear()
            self.job = dict(action=action, status='running', message='正在执行', started=datetime.now().isoformat(timespec='seconds'))
            threading.Thread(target=self._job, args=(action,), daemon=True).start()

    def _job(self, action):
        try:
            result = getattr(self, '_' + action)()
            if self.cancel.is_set() and action not in {'stream', 'replay', 'short_record'}:
                raise InterruptedError('已请求中断，请检查日志中的停止结果。')
            with self.lock:
                self.job.update(status='success', message=result)
            self.event('result', result)
        except Exception as exc:
            with self.lock:
                self.job.update(status='error', message=str(exc))
                if action=='stream':self._stream_unknown(str(exc))
            self.event('error', str(exc))
        finally:
            with self.lock:
                self.busy = False

    def _probe(self):
        self.identity_ok = False
        output = self.run_checked("printf 'KERNEL='; uname -r; printf 'ARCH='; uname -m; printf 'MODEL='; tr '\\000' ' ' < /proc/device-tree/model; echo; printf 'VENDOR='; cat " + DEVICE + "/vendor; printf 'DEVICE='; cat " + DEVICE + "/device; printf 'DRIVER='; readlink " + DEVICE + "/driver; echo; printf 'SD='; test -b /dev/mmcblk0p1 && echo present")
        fields = dict(re.findall(r'^(KERNEL|ARCH|MODEL|VENDOR|DEVICE|DRIVER|SD)=(.*)$', output.replace('\r', ''), re.M))
        self.identity_ok = (fields.get('KERNEL') == KERNEL and fields.get('ARCH') == 'aarch64'
                            and 'zcu102' in fields.get('MODEL', '').lower()
                            and fields.get('VENDOR') == '0x4c54' and fields.get('DEVICE') == '0x3000')
        with self.lock:
            self.checks = dict(fields=fields, at=datetime.now().isoformat(timespec='seconds'))
        if not self.identity_ok:
            raise RuntimeError('板型、内核或 PCIe 身份不匹配；已禁止加载与采集。请查看日志。')
        return '板卡身份检查通过。尚未检查相机信号，也未运行 DMA。'

    def _prepare(self):
        """从 SD 卡只读拷贝本仓库编译的程序到 /tmp，校验版本，加载驱动。不改任何二进制（D003）。"""
        self.run_checked("test \"$(uname -r)\" = '" + KERNEL + "' && test \"$(cat " + DEVICE + "/vendor)\" = 0x4c54 && test \"$(cat " + DEVICE + "/device)\" = 0x3000")
        self.run_checked(f"mkdir -p {SD_MOUNT}; cd /; umount {SD_MOUNT} 2>/dev/null; umount /mnt/sd* 2>/dev/null; mount -t vfat -o ro /dev/mmcblk0p1 {SD_MOUNT}")
        try:
            rc, _ = self.exchange(f"test -d {SD_BUNDLE}")
            if rc:
                raise RuntimeError('SD 卡第一个分区根目录下没有 glass-capture 文件夹。按 docs/BRINGUP.md 第 1 步拷贝。')
            self.run_checked(f"cd {SD_BUNDLE} && test -s SHA256SUMS && tr -d '\\r' < SHA256SUMS | sha256sum -c && mkdir -p {BOARD_DIR} /run && "
                             f"cp sg3000-capture sg3000-stream sg_status {BOARD_DIR}/ && chmod 700 {BOARD_DIR}/sg* && "
                             f"if [ -f board_session.sh ]; then cp board_session.sh {BOARD_DIR}/ && chmod 700 {BOARD_DIR}/board_session.sh; fi", 20)
            # 驱动：新目录里有就用新目录的（已由 SHA256SUMS 校验），否则用旧目录里校验过的那一份
            self.run_checked(f"if [ -f {SD_BUNDLE}/sg3000_rx.ko ]; then cp {SD_BUNDLE}/sg3000_rx.ko {BOARD_DIR}/; "
                             f"else cd {SD_LEGACY} && test -s SHA256SUMS && tr -d '\\r' < SHA256SUMS | grep ' sg3000_rx.ko$' | sha256sum -c && cp sg3000_rx.ko {BOARD_DIR}/; fi", 20)
        finally:
            self.run_checked('umount ' + SD_MOUNT, allow_cancel=True)
        for app, expected in ((APP, APP_VERSION), (STREAM_APP, STREAM_VERSION)):
            out = self.run_checked(app + (' version' if app == APP else ' --version'))
            if expected not in out:
                raise RuntimeError(f'{app} 版本不对：期望 {expected}。SD 卡上的程序可能是旧的。')
        self.run_checked("if [ -L " + DEVICE + "/driver ]; then test \"$(basename \"$(readlink " + DEVICE + "/driver)\")\" = sg3000_rx; elif [ -d /sys/module/sg3000_rx ]; then echo 'Module exists but card is not bound'; exit 1; else insmod " + BOARD_DIR + "/sg3000_rx.ko; fi && test -e " + DEV_NODE, 15)
        self.checks['prepared'] = True
        self.checks['stream_prepared'] = True
        return '已从 SD 卡校验并加载新程序（单帧、连续、体检）和驱动；SD 已卸载。尚未启动采集。'

    def _status(self):
        """只读体检：驱动信息、预检五项、两个状态寄存器。不启动采集。"""
        rc, output = self.exchange(f"{STATUS_APP}; _rc=$?; printf 'CAM_SERIAL='; cat {CAM_SERIAL_PARAM} 2>/dev/null || echo MISSING; "
                                   f"printf 'DMA_FAST='; cat {DMA_FAST_PARAM} 2>/dev/null || echo MISSING; exit $_rc", 15)
        self._parse_preflight_status(output)
        self._read_driver_params(output)
        card = {}
        for name, value in re.findall(r'^CardParam (\w+)\s+(?:0x[0-9a-f]+ = )?([\d.]+)', output.replace('\r', ''), re.M):
            card[name] = float(value) if '.' in value else int(value)
        for key, pattern in (('max_transfer', r'^max_transfer\s+(\d+)'), ('poisoned', r'^poisoned\s+(\d+)'),
                             ('hdr_ready', r'^SG_HDR_READY\s+(\d+)')):
            found = re.search(pattern, output.replace('\r', ''), re.M)
            if found:
                card[key] = int(found.group(1))
        if card:
            with self.lock:
                self.checks['card'] = card
        if rc == 2:
            raise RuntimeError('打不开采集卡设备（驱动没加载，或被别的程序占用）。')
        if rc == 1:
            if 'BOTH Camera Link cables' in output:
                raise RuntimeError('预检没通过：采集卡设成了 EightyBit（双线），但第 2、3 路像素时钟没有——第二根 Camera Link 线没接上或没信号。'
                                   '接好第二根线；或者走 B 方案（相机和卡都切 Base 3 Tap，见 BRINGUP 第 5¾ 节）。')
            raise RuntimeError('体检完成，但预检没通过。看日志里的 Preflight FAILED 那一行和时钟位。')
        return '体检通过：驱动正常、预检五项正常。没有启动采集。'

    def _card_xml(self):
        """只读：从采集卡存储里读出卡自己的 GenICam XML，取回电脑（宽度寄存器的地址在里面）。"""
        self.event('system', '正在从采集卡存储读 XML：每次读 4 字节，可能要一两分钟，请不要中断。')
        rc, output = self.exchange(f"mkdir -p {CARD_XML_DIR} && {STATUS_APP} --card-xml {CARD_XML_DIR}", 900)
        if rc == 2:
            raise RuntimeError('打不开采集卡设备（驱动没加载，或被别的程序占用）。先做「从 SD 加载程序」。')
        saved = re.search(r'CardXML saved: (\S+) \((\d+) bytes', output)
        if not saved:
            fail = re.search(r'CardXML FAILED: ([^\r\n]*)', output)
            raise RuntimeError('没读出采集卡 XML：' + (fail.group(1) if fail else output[-400:])
                               + '。把日志里 == Card XML == 那一段发给 Claude。')
        local, n, *_ = self._fetch_file_from_board(saved.group(1), CAPTURES / 'card-xml', preview=False)
        return f'采集卡 XML 已存到电脑：{local}（{n:,} 字节）。把它发给 Claude：CameraWidth 等寄存器的地址就在里面。'

    # ---- 相机（GenCP over Camera Link 串口，全部只读）--------------------------------

    def _ensure_cam_serial(self):
        """相机命令需要新驱动带 cam_serial=1。已是 Y 就不动；是 N 就重新加载（不影响采图）。"""
        rc, out = self.exchange(f"cat {CAM_SERIAL_PARAM} 2>/dev/null || echo MISSING", 10)
        state = out.replace('\r', '').strip().splitlines()[-1:] or ['']
        if state[0] == 'Y':
            return
        if state[0] == 'MISSING':
            raise RuntimeError('板上加载的是旧驱动（没有 cam_serial 参数），不能和相机通信。'
                               'SD 卡 glass-capture/ 里放新编译的 sg3000_rx.ko，再做「02 从 SD 加载程序」。')
        self.event('system', '正在以 cam_serial=1 重新加载驱动（只多放开相机串口端口 0，采图行为不变）。')
        # 重载时保留 dma_fast（快速 DMA）设置，不然点一下相机按钮就悄悄退回慢速模式
        self.run_checked(f"fast=$(cat {DMA_FAST_PARAM} 2>/dev/null); "
                         f"rmmod sg3000_rx && insmod {BOARD_DIR}/sg3000_rx.ko cam_serial=1 $([ \"$fast\" = Y ] && echo dma_fast=1) && "
                         f"test -e {DEV_NODE} && test \"$(cat {CAM_SERIAL_PARAM})\" = Y", 20)

    def _camera_probe(self):
        """只读：GenCP 读相机厂商、型号、序列号、XML 清单表。"""
        self._ensure_cam_serial()
        rc, output = self.exchange(f"{APP} camera-probe 2>&1", 60)
        model = re.search(r'CameraModel=([^\r\n]*)', output)
        if rc or not model:
            raise RuntimeError('相机没有应答：' + output[-400:] + '。确认相机已上电、串口线（第一根 Camera Link 线）已接。')
        manifest = re.findall(r'CameraManifest\[\d+\][^\r\n]*', output)
        return f'相机：{model.group(1)}。XML 清单：' + ('；'.join(manifest) if manifest else '未列出')

    def _latest_camera_xml(self):
        path = getattr(self, 'camera_xml_path', None)
        if path and Path(path).exists():
            return Path(path)
        folder = CAPTURES / 'camera-xml'
        files = sorted((p for p in folder.glob('*') if p.suffix.lower() in ('.zip', '.xml')),
                       key=lambda p: p.stat().st_mtime) if folder.exists() else []
        return files[-1] if files else None

    def _camera_xml(self):
        """只读：经 GenCP 清单表从相机本身读出它的 GenICam XML，校验 SHA1，取回电脑。"""
        self._ensure_cam_serial()
        self.event('system', '正在经 Camera Link 串口从相机读 XML（115200 波特，几十 KB 约需 10–60 秒，大的几分钟），请不要中断。')
        rc, output = self.exchange(f"mkdir -p {CAMERA_XML_DIR} && {APP} camera-xml {CAMERA_XML_DIR} 2>&1", 1800)
        saved = re.search(r'CameraXML saved: (\S+) \((\d+) bytes', output)
        if rc or not saved:
            fail = re.search(r'CameraXML FAILED: ([^\r\n]*)', output)
            raise RuntimeError('没读出相机 XML：' + (fail.group(1) if fail else output[-400:])
                               + '。把日志里 CameraManifest 那几行和报错发给 Claude。')
        remote = saved.group(1)
        sha = re.search(r'CameraXML SHA1 expected ([0-9a-f]{40})', output)
        if sha:
            out = self.run_checked(f"sha1sum '{remote}'", 20)
            if sha.group(1) not in out:
                raise RuntimeError('相机 XML 的 SHA1 和相机清单里写的不一致（串口传输出错）。再读一次。')
        local, n, *_ = self._fetch_file_from_board(remote, CAPTURES / 'camera-xml', preview=False)
        self.camera_xml_path = str(local)
        return (f'相机 XML 已存到电脑：{local}（{n:,} 字节' + ('，SHA1 与相机一致' if sha else '，相机没给 SHA1') +
                '）。下一步点「读相机寄存器」核对相机实际输出格式。')

    def _camera_regs(self):
        """只读：按相机 XML 读出宽度、像素格式、Tap、CL 配置等，翻译成名字。"""
        import genicam
        xml = self._latest_camera_xml()
        if xml is None:
            raise RuntimeError('电脑上还没有相机 XML。先点「读取相机 XML」。')
        model = genicam.Model(genicam.load_xml(xml.read_bytes()))
        infos = genicam.features(model)
        args = genicam.board_args(infos)
        if not args:
            raise RuntimeError(f'{xml.name} 里找不到可直接读的标准特征（Width、PixelFormat……）。把 XML 发给 Claude。')
        self._ensure_cam_serial()
        rc, output = self.exchange(f"{APP} camera-regs {' '.join(args)} 2>&1", 300)
        if 'CameraRegs done' not in output:
            raise RuntimeError('读相机寄存器失败：' + output[-400:])
        rows = genicam.decode_board_output(infos, output)
        report = '\n'.join(f'{n:28s} {("0x%08x" % a) if a is not None else "-":>12s}  {v}' for n, a, v in rows)
        folder = CAPTURES / 'camera-xml'
        folder.mkdir(parents=True, exist_ok=True)
        out_file = folder / f"camera-regs-{datetime.now().strftime('%Y%m%d-%H%M%S')}.txt"
        out_file.write_text(f'# {xml.name}\n' + report + '\n\n# raw board output\n' + output, encoding='utf-8')
        self.event('system', '相机寄存器：\n' + report)
        key = {n: v for n, _, v in rows if n in ('Width', 'PixelFormat', 'ClConfiguration', 'DeviceTapGeometry',
                                                 'AcquisitionLineRate', 'ExposureTime')}
        return (f'已读相机寄存器（{len(rows)} 项，已存 {out_file}）。' +
                '；'.join(f'{k}={v}' for k, v in key.items()))

    def _board_session(self):
        """一键上板测试：板上跑 board_session.sh（BRINGUP 5⅞ + 5⁹ + CPU 基准，约 4–6 分钟），取回结果包并解开。"""
        import tarfile
        rc, _ = self.exchange(f"test -f {BOARD_DIR}/board_session.sh", 10)
        if rc:
            raise RuntimeError('板上没有 board_session.sh：SD 卡里的 glass-capture/ 是旧版。用新 dist 更新 SD 卡，再做「02 从 SD 加载程序」。')
        self.event('system', '一键上板测试开始：会重载驱动、切相机和卡到 Base 3 Tap / 16384、拍测试图、调曝光增益、测 DMA 吞吐和 15 kHz，最后测 CPU 处理速度。'
                             '全部断电可恢复，约 4–6 分钟，请不要中断。')
        rc, output = self.exchange(f"sh {BOARD_DIR}/board_session.sh 2>&1", 1800)
        saved = re.search(r'BoardSession saved: (\S+\.tar\.gz) \((\d+) bytes', output)
        if not saved:
            raise RuntimeError('一键测试没有生成结果包：' + output[-600:])
        local, n, *_ = self._fetch_file_from_board(saved.group(1), CAPTURES / 'board-session', preview=False)
        folder = Path(local).parent
        with tarfile.open(local, 'r:gz') as tar:
            # 只解普通文件、只解到 folder 里面（不用 extractall：老版本 Python 没有安全过滤）
            for member in tar.getmembers():
                target = (folder / member.name).resolve()
                if not member.isfile() or folder.resolve() not in target.parents:
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
        failed = re.search(r'== Summary: (\d+) step', output)
        lines = [l.strip() for l in output.replace('\r', '').splitlines()
                 if l.strip().startswith(('StreamSummary', 'StreamHint', 'Ramp ', 'FAIL', 'SKIP', 'Bench name'))]
        head = f"一键测试完成：{failed.group(1) if failed else '?'} 步失败。结果已存到 {folder}（session.log、summary.txt、缩略图）。"
        if rc and not failed:
            raise RuntimeError(head + ' 脚本中途退出：' + output[-400:])
        return head + ' ' + '；'.join(lines[:16]) + '。把整个文件夹发给 Claude。'

    def _parse_preflight_status(self, output):
        ddr = re.search(r'DDR status\s+(\d+)', output)
        clk = re.search(r'pixel clocks\s+0x([0-9a-fA-F]+)', output)
        cl = re.search(r'CL config\s+(\d+)', output)
        if ddr and clk:
            self.checks['preflight'] = dict(at=datetime.now().isoformat(timespec='seconds'),
                                            passed='Preflight passed.' in output, known=True,
                                            ddr=int(ddr.group(1)), clocks=int(clk.group(1), 16),
                                            config=dict(cl=int(cl.group(1)) if cl else 0, color=0, depth=8)
                                            if 'Preflight passed.' in output else None,
                                            reads_confirmed=True)

    def _preflight(self):
        rc, output = self.exchange(APP + ' preflight', 15)
        self._parse_preflight(output, rc)
        if rc:
            if 'BOTH Camera Link cables' in output:
                raise RuntimeError('采集条件未满足：EightyBit 需要两根 Camera Link 线，第 2、3 路像素时钟没有。没有启动 DMA。')
            raise RuntimeError('采集条件未满足。请查看信号检查和原始输出；没有启动 DMA。')
        return '采集前检查通过。尚未启动 DMA，不能据此认定能采到图像。'

    def _parse_preflight(self, output, rc):
        match = re.search(r'DDRInitialStatus=(\d+), CameraLinkPixelClocks=0x([0-9a-fA-F]+), CL=(\d+), Color=(\d+), Depth=(\d+)', output)
        data = dict(at=datetime.now().isoformat(timespec='seconds'), passed=rc == 0, known=False)
        if match:
            ddr, clocks, cl, color, depth = match.groups()
            # Existing binary prints zero-initialized unread fields on early return.
            # Values are trusted as a complete configuration only after full success.
            data.update(known=True, ddr=int(ddr), clocks=int(clocks, 16),
                        config=dict(cl=int(cl), color=int(color), depth=int(depth)) if rc == 0 else None,
                        reads_confirmed=rc == 0 or '(-67)' in output or '(-95)' in output)
        self.checks['preflight'] = data

    def _fetch_file_from_board(self, remote_path, local_dest_dir=CAPTURES, preview=True):
        local_dest_dir = Path(local_dest_dir)
        local_dest_dir.mkdir(parents=True, exist_ok=True)
        filename = Path(remote_path).name
        local_file = local_dest_dir / filename
        temp_gz = f"/tmp/{filename}.gz"

        self.event('system', f'正在板端打包压缩：{filename} ...')
        self.run_checked(f"gzip -1 -c '{remote_path}' > '{temp_gz}'", timeout=20)

        size_str = self.run_checked(f"wc -c < '{temp_gz}'", timeout=10)
        try:
            gz_size = int(size_str.strip())
        except ValueError:
            gz_size = 0

        self.event('system', f'板端打包完成（压缩后 {gz_size:,} 字节），正在通过 USB 串口传输到电脑，请稍候...')
        transfer_timeout = max(35, int(gz_size * 2 / 5000) + 15)

        self.silent_rx = True
        try:
            rc, hex_output = self.exchange(f"{HEXDUMP} -v -e '16/1 \"%02x\" \"\\n\"' '{temp_gz}'", timeout=transfer_timeout)
        finally:
            self.silent_rx = False

        if rc:
            raise RuntimeError(f"读取板端压缩文件失败 (退出码 {rc})")

        self.exchange(f"rm -f '{temp_gz}'", timeout=5, allow_cancel=True)

        clean_hex = re.sub(r'[^0-9a-fA-F]', '', hex_output)
        if len(clean_hex) % 2 != 0:
            clean_hex = clean_hex[:-1]
        gz_bytes = bytes.fromhex(clean_hex)
        if gz_size > 0 and len(gz_bytes) != gz_size:
            self.event('system', f"提示：回传大小存在轻微偏差（期望 {gz_size:,} 字节，实际接收 {len(gz_bytes):,} 字节），尝试解包...")

        try:
            decompressed = gzip.decompress(gz_bytes)
        except Exception as dec_err:
            raise ValueError(f"图像解包校验失败（期望 {gz_size:,} 字节，接收 {len(gz_bytes):,} 字节）：{dec_err}")
        local_file.write_bytes(decompressed)
        self.event('system', f'文件已存入电脑：{local_file}（{len(decompressed):,} 字节）')

        preview_file = local_dest_dir / (local_file.stem + '_preview.png')
        width, height = 4096, 2000
        if not preview:
            return local_file, len(decompressed), gz_size, 0, 0
        try:
            from PIL import Image
            import io
            img = Image.open(io.BytesIO(decompressed))
            width, height = img.size
            if img.mode != 'L':
                img = img.convert('L')
            img.save(preview_file)
        except Exception as p_err:
            self.event('system', f'生成预览图提示: {p_err}')

        return local_file, len(decompressed), gz_size, width, height

    def _fetch_last(self):
        rc, output = self.exchange(f"ls -t {BOARD_CAPTURES}/*.pgm 2>/dev/null | head -n 1", timeout=10)
        lines = [l.strip() for l in output.strip().splitlines() if l.strip()]
        target = lines[-1] if lines else ""
        if not target or not target.startswith(BOARD_CAPTURES + '/'):
            rc_sd, output_sd = self.exchange("mkdir -p /mnt/sd && mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null; ls -t /mnt/sd/frame-*.pgm 2>/dev/null | head -n 1; umount /mnt/sd 2>/dev/null", timeout=10)
            sd_lines = [l.strip() for l in output_sd.strip().splitlines() if 'frame-' in l and l.endswith('.pgm')]
            if sd_lines:
                target_sd = sd_lines[-1]
                self.run_checked(f"mkdir -p /mnt/sd && mount -t vfat /dev/mmcblk0p1 /mnt/sd && cp '{target_sd}' {BOARD_CAPTURES}/ && umount /mnt/sd", 15)
                target = f"{BOARD_CAPTURES}/{Path(target_sd).name}"
            else:
                raise RuntimeError('板上尚未找到任何已采集的 .pgm 图像。请先点击「04 采集一帧」。')

        if self.net:
            local_file, size, w, h = self._fetch_over_network(target)
            gz_size = size
        else:
            local_file, size, gz_size, w, h = self._fetch_file_from_board(target)
        with self.lock:
            self.last_frame = dict(
                name=local_file.name,
                path=target,
                location='board-ram + local-pc',
                local_path=str(local_file),
                size_bytes=size,
                gz_bytes=gz_size,
                at=datetime.now().isoformat(timespec='seconds'),
                width=w,
                height=h
            )
        via = '网口' if self.net else f'USB 串口（压缩后 {gz_size:,} 字节）'
        return f"最新图像已回传并保存至 {local_file}（{size:,} 字节，经{via}）。"

    def _save_sd(self):
        """把板上内存最新的 .pgm 写入 SD 卡根目录，sync 并卸载，便于断电拔卡插回电脑读取。"""
        rc, output = self.exchange(f"ls -t {BOARD_CAPTURES}/*.pgm 2>/dev/null | head -n 1", timeout=10)
        lines = [l.strip() for l in output.strip().splitlines() if l.strip()]
        target = lines[-1] if lines else ''
        if rc or not target.startswith(BOARD_CAPTURES + '/'):
            raise RuntimeError('板上内存里还没有已采集的 .pgm 图像。请先点击「单帧」采集。')
        fname = Path(target).name
        if not re.fullmatch(r'[A-Za-z0-9._-]+\.pgm', fname):
            raise RuntimeError('板上图像文件名异常，已拒绝写入 SD 卡。')
        self.run_checked(
            f"mkdir -p {SD_MOUNT}; cd /; umount {SD_MOUNT} 2>/dev/null; umount /mnt/sd* 2>/dev/null; "
            f"mount -t vfat /dev/mmcblk0p1 {SD_MOUNT}",
            15,
        )
        try:
            out = self.run_checked(
                f"src_sz=$(wc -c < '{target}' | tr -d ' \\t') && test \"$src_sz\" -gt 0 && "
                f"cp '{target}' '{SD_MOUNT}/{fname}' && sync && "
                f"dst_sz=$(wc -c < '{SD_MOUNT}/{fname}' | tr -d ' \\t') && test \"$src_sz\" = \"$dst_sz\" && "
                f"printf 'SAVED_SD_BYTES=%s\\n' \"$dst_sz\"",
                45,
            )
        finally:
            self.run_checked(f"cd /; sync; umount {SD_MOUNT}", allow_cancel=True)
        m = re.search(r'SAVED_SD_BYTES=(\d+)', out)
        if not m:
            raise RuntimeError('写入 SD 卡后大小核对失败：' + out[-200:])
        size = int(m.group(1))
        with self.lock:
            prev = dict(self.last_frame or {})
            prev.update(
                name=fname,
                path=target,
                size_bytes=size,
                sd_saved=True,
                sd_name=fname,
                location='board-ram + sd-card + local-pc' if prev.get('local_path') else 'board-ram + sd-card',
            )
            self.last_frame = prev
        return f"已写入 SD 卡根目录（{fname}，{size:,} 字节），并完成 sync 与卸载。可断电拔卡插回电脑，到「图像 → 打开 PGM…」查看。"

    def _capture(self):
        self.last_frame = None
        name = 'frame-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6] + '.pgm'
        path = BOARD_CAPTURES + '/' + name
        rc, output = self.exchange('mkdir -p ' + BOARD_CAPTURES + ' && ' + APP + ' capture --timeout-ms 10000 --output ' + path, 25)
        self._parse_preflight(output, rc)
        saved = re.search(r'Saved complete Mono8 frame: (\d+) x (\d+), (\d+) bytes', output)
        if rc or not saved:
            raise RuntimeError('未确认生成完整图像。请查看采集和停止日志；不会显示成功预览。')
        w, h, n = (int(v) for v in saved.groups())

        with self.lock:
            self.last_frame = dict(
                name=name,
                path=path,
                location='board-ram',
                size_bytes=n,
                at=datetime.now().isoformat(timespec='seconds'),
                width=w,
                height=h
            )
        return f'完整图像采集成功！已存入板载内存：{path}（{w} × {h}，{n:,} 字节）。点「回传最新图像」传到电脑。'

    def _dmesg(self):
        self.run_checked('dmesg | tail -n 60')
        return '已读取最近 60 行内核日志。'

    def _release(self):
        self.run_checked('if [ -d /sys/module/sg3000_rx ]; then rmmod sg3000_rx || exit $?; fi; sync', 15)
        self.checks['prepared'] = False
        self.checks.pop('preflight', None)
        return '接收驱动已卸载，已请求同步文件。此操作不会关闭 Linux。'

    def accept_stream_line(self, line, channel):
        data=parse_telemetry(line,channel)
        if data is None:return
        with self.lock:
            if self.stream.get('status') in {'ended','error','unknown'}:return
            kind=data['event']
            started=self.stream.get('started')
            if kind=='started' and started:return
            if kind=='result' and not started:return
            if started and kind in {'result','ended'} and any(data[k]!=started[k] for k in ('source','processor')):return
            previous=self.stream.get('result')
            if kind=='result' and previous and (int(data['sequence'])<=int(previous['sequence']) or int(data['processed_total'])<=int(previous['processed_total'])):return
            if kind=='ended':
                errors=('error','source_error','processing_error','output_error','stop_error')
                if not started and all(data[k]==0 for k in errors):return
                if started and data['queue_peak']>started['queue_depth']:return
                if previous and (int(data['received'])<int(previous['sequence']) or int(data['processed'])<int(previous['processed_total'])):return
            self.stream['channel'] = channel
            self.stream['updated_at'] = datetime.now().isoformat(timespec='seconds')
            self.stream[kind] = data
            if kind == 'started': self.stream['status'] = 'running'
            elif kind == 'ended': self.stream['status'] = 'ended' if all(data[k]==0 for k in ('error','source_error','processing_error','output_error','stop_error')) else 'error'

    def _prepare_stream(self):
        out = self.run_checked(STREAM_APP + ' --version')
        if STREAM_VERSION not in out:
            raise RuntimeError('板上没有连续程序，先执行「从 SD 加载程序」。')
        self.checks['stream_prepared'] = True
        return '连续程序已在板上（「从 SD 加载程序」时一起拷贝的）。'

    def _stream(self):
        seconds, processor = self.stream_options['seconds'], self.stream_options['processor']
        version = self.run_checked(STREAM_APP + ' --version')
        if STREAM_VERSION not in version: raise RuntimeError('先执行「从 SD 加载程序」。')
        command = (STREAM_APP + ' --source hardware --processor ' + processor + ' --seconds ' + str(seconds)
                   + ' --queue 4 --max-frame-mib 48 --emit-ms 1000')   # 16384 × 2000 一块约 31 MiB
        if processor == 'candidates':
            if self.net:
                self._ensure_receiver()
                command += f" --tcp {self.net['pc_ip']}:{self.net['port']}"
                self.event('system', f'候选会经网口存到 {CAPTURES / "candidates"}。')
            else:
                self.event('system', '网口未配置：只能看到每块的候选个数，候选坐标不会传回电脑。')
        rc, output = self.exchange(command, seconds + 40 if seconds else float('inf'))
        ended = self.stream.get('ended', {})
        if rc or self.stream.get('status')!='ended' or any(ended.get(k)!=0 for k in ('error','source_error','processing_error','output_error','stop_error')):
            raise RuntimeError('连续接收未正常结束，请查看停止状态与原始日志。' + output[-300:])
        what = '候选检测（第一版算法，未对真值）' if processor == 'candidates' else '灰度统计（不是缺陷判定）'
        return '连续接收已结束，收到 ' + str(ended.get('received', '?')) + ' 块，处理 ' + str(ended.get('processed', '?')) + ' 块。处理方式：' + what + '。'

    def _check_short_result(self, rc, output):
        ended = self.stream.get('ended', {})
        if (
            rc
            or self.stream.get('status') != 'ended'
            or any(ended.get(k) != 0 for k in ('error', 'source_error', 'processing_error', 'output_error', 'stop_error'))
            or int(ended.get('id_discontinuities', 0)) != 0
            or int(ended.get('queue_full_events', 0)) != 0
        ):
            raise RuntimeError('短录未完整成功（可能发生丢块、队列满或写入错误），未保留残缺文件。' + output[-300:])
        m = re.search(r'^SAVED_PGM width=(\d+) height=(\d+) bytes=(\d+) path=(\S+)', output.replace('\r', ''), re.M)
        if not m:
            raise RuntimeError('短录结束但未收到 SAVED_PGM 确认，请查看日志。' + output[-300:])
        return dict(width=int(m.group(1)), height=int(m.group(2)), bytes=int(m.group(3)), path=m.group(4))

    def _short_record(self):
        """短录连续长图并写入 SD 卡，确保无块号跳变且完成 sync + 卸载。"""
        seconds = self.short_options['seconds']
        storage = self.short_options['storage']
        version = self.run_checked(STREAM_APP + ' --version')
        if STREAM_VERSION not in version:
            raise RuntimeError('先执行「从 SD 加载程序」。')
        help_out = self.run_checked(STREAM_APP + ' --help')
        if '--save-pgm' not in help_out:
            raise RuntimeError('板上 sg3000-stream 还是旧版（不支持 --save-pgm）。请用新版 dist/glass-capture 更新 SD 卡后再试。')

        fname = 'short-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '.pgm'
        if storage == 'ram_sd':
            space_out = self.run_checked(
                f"mkdir -p {BOARD_CAPTURES}; "
                f"printf 'TMP_DF='; (df -k /tmp 2>/dev/null || true) | awk 'NR>1 && NF>=4 {{v=$(NF-2)}} END{{print v+0}}'; "
                f"printf 'MEM_AVAIL='; awk '/^MemAvailable:/{{print $2; ok=1; exit}} /^MemFree:/{{f=$2}} END{{if(!ok) print f+0}}' /proc/meminfo",
                10,
            )
            clean_space = space_out.replace('\r', '')
            tmp_m = re.search(r'^TMP_DF=(\d+)', clean_space, re.M)
            mem_m = re.search(r'^MEM_AVAIL=(\d+)', clean_space, re.M)
            tmp_df_kib = int(tmp_m.group(1)) if tmp_m else 0
            mem_kib = int(mem_m.group(1)) if mem_m else 0
            if mem_kib <= 0 and tmp_df_kib <= 0:
                raise RuntimeError('无法读取板上可用内存与 /tmp 空间。')
            candidates = []
            if tmp_df_kib > 0:
                candidates.append((tmp_df_kib - 256 * 1024) * 1024)
            if mem_kib > 0:
                candidates.append((mem_kib - 640 * 1024) * 1024)
            usable_bytes = min(min(candidates), 4_200_000_000)
            if usable_bytes < 64 * 1024 * 1024:
                raise RuntimeError(f'板上可用内存不足安全短录（MEM_AVAIL={mem_kib // 1024} MiB, TMP_DF={tmp_df_kib // 1024} MiB）。')
            avail_label = f"/tmp {tmp_df_kib // 1024} MiB / " if tmp_df_kib > 0 else "initramfs / "
            self.event('system', f'短录先写板载 /tmp（{avail_label}可用内存 {mem_kib // 1024} MiB，本次上限 {usable_bytes // 1048576} MiB），录完再拷到 SD 卡。')
            rec_path = f'{BOARD_CAPTURES}/{fname}'
            command = (
                f"{STREAM_APP} --source hardware --processor stats --seconds {seconds} "
                f"--queue 3 --max-frame-mib 48 --block-policy strict --max-bytes {usable_bytes} "
                f"--save-pgm {rec_path} --emit-ms 1000"
            )
            rc, output = self.exchange(command, seconds + 45)
            saved = self._check_short_result(rc, output)
            w, h, n = saved['width'], saved['height'], saved['bytes']
            ended = self.stream.get('ended', {})
            was_cancelled = bool(ended.get('cancelled'))
            mounted = False
            copy_timeout = max(120, n // 4_000_000 + 60)
            try:
                copy_msg = f'采集已成功（{w} × {h}），正在写入 SD 卡并 sync（{n / 1048576:.1f} MB，约需 1–2 分钟，请勿断电或点停止）…'
                with self.lock:
                    if self.job:
                        self.job['message'] = copy_msg
                self.event('system', copy_msg)
                self.run_checked(
                    f"mkdir -p {SD_MOUNT}; cd /; umount {SD_MOUNT} 2>/dev/null; umount /mnt/sd* 2>/dev/null; mount -t vfat /dev/mmcblk0p1 {SD_MOUNT}",
                    15,
                    allow_cancel=was_cancelled,
                )
                mounted = True
                sd_df = self.run_checked(
                    f"df -k {SD_MOUNT} 2>/dev/null | awk 'NR>1 && NF>=4 {{v=$(NF-2)}} END{{print v+0}}'",
                    10,
                    allow_cancel=was_cancelled,
                )
                sd_m = re.search(r'(\d+)', sd_df)
                if sd_m and int(sd_m.group(1)) > 0 and int(sd_m.group(1)) * 1024 < n + 16 * 1024 * 1024:
                    raise RuntimeError('SD 卡剩余空间不足，无法写入短录长图。')
                cp_out = self.run_checked(
                    f"cp '{rec_path}' '{SD_MOUNT}/{fname}' && sync && printf 'SAVED_SD_BYTES=' && wc -c < '{SD_MOUNT}/{fname}'",
                    copy_timeout,
                    allow_cancel=was_cancelled,
                )
                sd_bytes = re.search(r'SAVED_SD_BYTES=(\d+)', cp_out)
                if not sd_bytes or int(sd_bytes.group(1)) != n:
                    raise RuntimeError('SD 卡写入后的文件字节数与板载内存不一致，请检查日志并确认勿拔卡。')
                self.exchange(f"rm -f '{rec_path}'", 15, allow_cancel=was_cancelled)
            finally:
                if mounted:
                    self.exchange(f"cd /; sync; umount {SD_MOUNT}", 120, allow_cancel=True)
        else:
            mounted = False
            rec_path = f'{SD_MOUNT}/{fname}'
            try:
                self.run_checked(
                    f"mkdir -p {SD_MOUNT}; cd /; umount {SD_MOUNT} 2>/dev/null; umount /mnt/sd* 2>/dev/null; mount -t vfat /dev/mmcblk0p1 {SD_MOUNT}",
                    15,
                )
                mounted = True
                sd_df = self.run_checked(
                    f"df -k {SD_MOUNT} 2>/dev/null | awk 'NR>1 && NF>=4 {{v=$(NF-2)}} END{{print v+0}}'",
                    10,
                )
                sd_m = re.search(r'(\d+)', sd_df)
                if not sd_m or int(sd_m.group(1)) <= 0:
                    raise RuntimeError('无法读取 SD 卡剩余空间。')
                avail_kib = int(sd_m.group(1))
                usable_bytes = min((avail_kib - 64 * 1024) * 1024, 4_200_000_000)
                if usable_bytes < 64 * 1024 * 1024:
                    raise RuntimeError(f'SD 卡可用空间仅 {avail_kib // 1024} MiB，不足写入。')
                self.event('system', f'低速直写 SD 卡（可用 {avail_kib // 1024} MiB，字节上限 {usable_bytes // 1048576} MiB）；若行频过高会立刻报错停机不丢块。')
                command = (
                    f"{STREAM_APP} --source hardware --processor stats --seconds {seconds} "
                    f"--queue 4 --max-frame-mib 48 --block-policy strict --max-bytes {usable_bytes} "
                    f"--save-pgm {rec_path} --emit-ms 1000"
                )
                rc, output = self.exchange(command, seconds + 60)
                saved = self._check_short_result(rc, output)
                w, h, n = saved['width'], saved['height'], saved['bytes']
            finally:
                if mounted:
                    self.exchange(f"cd /; sync; umount {SD_MOUNT}", 60, allow_cancel=True)

        ended = self.stream.get('ended', {})
        with self.lock:
            self.last_frame = dict(
                name=fname,
                path=fname,
                location='sd-card',
                size_bytes=n,
                at=datetime.now().isoformat(timespec='seconds'),
                width=w,
                height=h,
                sd_saved=True,
                sd_name=fname,
            )
        note = '（提前停止）' if ended.get('cancelled') else ''
        return f"短录{note}并写入 SD 卡完成：{fname}（{w} × {h}，{n / 1048576:.1f} MB），已 sync 并卸载；断电拔卡后到「采集 → 图像 → 打开 PGM…」查看。"

    # ==== 网口 ==========================================================

    # 和 board/capture/sg_mailbox.c 的 sg_mbx_write_allowed 一致（值域来自卡的 XML）
    CARD_SET_RANGES = {'CameraWidth': range(128, 65536), 'CameraHeight': range(200, 65536),
                       'DeviceTapGeometry': range(1, 11), 'ClConfiguration': (0, 1, 2, 4),
                       'CameraScanType': (0, 1)}

    @classmethod
    def _check_card_set(cls, options):
        name, value = options.get('name'), options.get('value')
        if name not in cls.CARD_SET_RANGES or type(value) is not int or value not in cls.CARD_SET_RANGES[name]:
            raise ValueError('卡参数名或数值不在卡 XML 允许的范围内。')
        return dict(name=name, value=value)

    def _card_set(self):
        """写一个卡参数（只在采集停止时；写后读回核对）。不启动采集。"""
        o = self.card_set_options
        rc, output = self.exchange(f"{APP} card-set {o['name']} {o['value']} 2>&1", 15)
        done = re.search(r'CardSet (\w+) 0x[0-9a-f]+: (\d+) -> (\d+), readback (\d+)', output)
        if rc or not done:
            raise RuntimeError('卡参数没写成：' + output[-400:])
        old, _, back = (int(x) for x in done.groups()[1:])
        return f"采集卡 {o['name']}：{old} → {back}（已读回确认）。重新「04 采集一帧」看效果；断电后恢复默认。"

    # 和板上 cam_set_check()（board/camera_ctrl/cam_set.c）一致；锁定条件和曝光上下限由板上按相机当前状态再查一次。
    CAMERA_SET_INTS = {'ClConfiguration': (0, 3), 'DeviceTapGeometry': (2, 3),
                       'TestPattern': (0, 1, 2, 3, 5), 'AnalogGain': (0, 1, 2, 3, 4),
                       'DigitalShift': (0, 1, 2, 3, 4), 'TDIStage': (0, 1), 'TDIMode': (0, 1),
                       'FFCEnable': (0, 1), 'FFCUserSetSelector': (0, 1, 2, 3), 'FFCSampleFrames': (1024, 2048),
                       'FFCCalTargetMode': (0, 1, 2), 'FFCCalTargetValue': range(0, 251),
                       'BlackLevel': range(-65536, 65537),          # 真正的范围由板上按相机报的上下限查
                       # 命令：值只能是 1。永远没有 UserSetSave / FFCUserSetSave。
                       'FFCCalFPN': (1,), 'FFCCalPRNU': (1,), 'FFCUserSetLoad': (1,)}
    CAMERA_COMMANDS = ('FFCCalFPN', 'FFCCalPRNU', 'FFCUserSetLoad')
    # 实时调试时可以边看边改的参数（都不改变图像尺寸和输出模式）。
    LIVE_ADJUSTABLE = ('ExposureTime', 'LineRate', 'DigitalShift', 'DigitalGain', 'AnalogGain', 'BlackLevel', 'TDIStage', 'TDIMode', 'TestPattern')
    CAMERA_SET_FLOATS = {'ExposureTime': (0.0, 100000.0), 'DigitalGain': (0.10, 15.99), 'LineRate': (100.0, 200000.0)}

    @classmethod
    def _check_camera_set(cls, options):
        name, value = options.get('name'), options.get('value')
        if name in cls.CAMERA_SET_INTS and type(value) is int and value in cls.CAMERA_SET_INTS[name]:
            return dict(name=name, value=value, text=str(value))
        if name in cls.CAMERA_SET_FLOATS and type(value) in (int, float):
            lo, hi = cls.CAMERA_SET_FLOATS[name]
            if (lo < value if name == 'ExposureTime' else lo <= value) and value <= hi:
                return dict(name=name, value=value, text=f'{value:g}')
        raise ValueError('相机参数不在允许范围：ClConfiguration 0/3，DeviceTapGeometry 2/3（仅 Base），'
                         'TestPattern 0/1/2/3/5，ExposureTime 微秒，DigitalGain 0.10–15.99，AnalogGain 0–4，LineRate 100–200000 Hz，'
                         'DigitalShift 0–4，TDIStage / TDIMode / FFCEnable 0/1，FFCUserSetSelector 0–3，FFCSampleFrames 1024/2048，'
                         'FFCCalTargetMode 0–2，FFCCalTargetValue 0–250，BlackLevel 整数，命令 FFCCalFPN / FFCCalPRNU / FFCUserSetLoad 只能是 1。')

    @staticmethod
    def _parse_camera_params(output):
        """把板上 CameraParam 行变成 {名字: {raw, value, min, max, unit, locked, error}}。"""
        params = {}
        for name, rest in re.findall(r'^CameraParam (\w+) ([^\r\n]*)', output.replace('\r', ''), re.M):
            item = {}
            for key, text in re.findall(r'(\w+)=(\S+)', rest):
                if key in ('value', 'min', 'max'):
                    try: item[key] = float(text)
                    except ValueError: pass
                elif key == 'raw':
                    try: item[key] = int(text, 0)
                    except ValueError: pass
                elif key == 'locked':
                    item[key] = text == '1'
                else:
                    item[key] = text
            params[name] = item
        return params

    def _store_camera_params(self, output):
        params = self._parse_camera_params(output)
        if params:
            with self.lock:
                self.camera = dict(params=params, at=datetime.now().isoformat(timespec='seconds'),
                                   partial='CameraParamAbort' in output)
        return params

    def _camera_get(self):
        """只读：一次读出相机当前全部常用参数（曝光、行频、增益、输出模式、平场校正、TDI……）。"""
        self._ensure_cam_serial()
        rc, output = self.exchange(f"{APP} camera-get 2>&1", 90)
        params = self._store_camera_params(output)
        if rc or not params:
            raise RuntimeError('没读到相机参数：' + output[-400:] + '。确认相机已上电、第一根 Camera Link 线已接。')
        bad = [n for n, v in params.items() if 'error' in v]
        return f'已读相机参数 {len(params)} 项' + (f'（{len(bad)} 项读不出：{"、".join(bad)}）' if bad else '') + '。只读，没有改动相机。'

    def _camera_set(self):
        """写相机的一个运行时参数，或发一个校正命令（都不保存，相机断电恢复）。写后读回核对，并刷新参数表。"""
        o = self.camera_set_options
        self._ensure_cam_serial()
        command = o['name'] in self.CAMERA_COMMANDS
        rc, output = self.exchange(f"{APP} camera-set {o['name']} {o['text']} --params 2>&1", 120 if command else 90)
        self._store_camera_params(output)
        if command:
            sent = re.search(r'CameraCommand (\w+) 0x[0-9a-f]+: sent, camera answered again after about (\d+) ms', output)
            if rc or not sent:
                raise RuntimeError('相机命令没执行：' + output[-500:])
            what = {'FFCCalFPN': '暗场校正（FPN）', 'FFCCalPRNU': '亮场校正（PRNU）', 'FFCUserSetLoad': '重新载入已存的平场校正数据'}[o['name']]
            return f"相机已执行{what}，约 {int(sent.group(2)) / 1000:.1f} 秒后恢复应答。校正结果只在相机内存里，断电恢复原样。采一帧看轮廓是否变平。"
        done = re.search(r'CameraSet (\w+) 0x[0-9a-f]+: (\d+) -> (\d+), readback (\d+)', output)
        if rc or not done:
            raise RuntimeError('相机参数没写成：' + output[-500:])
        state = re.search(r'CameraState ClConfiguration=(\d+).*?DeviceTapGeometry=(\d+)', output)
        old, _, back = (int(x) for x in done.groups()[1:])
        mode = {0: 'Base', 3: 'EightyBit'}
        now = (f"相机现在 ClConfiguration={state.group(1)}（{mode.get(int(state.group(1)), '?')}）、"
               f"DeviceTapGeometry={state.group(2)}。") if state else ''
        value = re.search(r'CameraValue \w+ = ([^\r\n]*)', output)
        tail = '采集卡要改成同样的模式（见 BRINGUP 第 5¾ 节）；' if o['name'] in ('ClConfiguration', 'DeviceTapGeometry') else ''
        return (f"相机 {o['name']}：{old} → {back}（已读回确认" + (f"，= {value.group(1)}" if value else '') + "）。" + now +
                tail + '没有保存，相机断电后恢复。')

    def _camera_preset(self):
        """B 方案一键配置：相机和采集卡都切到 Base 3 Tap、宽 16384（单线），然后体检。都不保存。"""
        self._ensure_cam_serial()
        for name, value in (('ClConfiguration', 0), ('DeviceTapGeometry', 3)):
            rc, output = self.exchange(f"{APP} camera-set {name} {value}" + (' --params' if name == 'DeviceTapGeometry' else '') + " 2>&1", 90)
            self._store_camera_params(output)
            if rc or not re.search(r'CameraSet ' + name + r' 0x[0-9a-f]+: \d+ -> \d+, readback ' + str(value), output):
                raise RuntimeError(f'相机 {name} 没写成：' + output[-400:])
        rc, output = self.exchange(f"{APP} card-set CameraWidth 16384 2>&1", 15)
        if rc or not re.search(r'CardSet CameraWidth 0x[0-9a-f]+: \d+ -> 16384, readback 16384', output):
            raise RuntimeError('采集卡 CameraWidth 没写成：' + output[-400:])
        self._status()
        return '相机和采集卡都已切到 Base 3 Tap、宽 16384，体检通过。没有保存：相机或板子断电后要重做一次。'

    # ---- 采一帧 / 实时调试：采图后在板上算亮度、线轮廓和对焦指标，只把这些数字传回来 ----
    LIVE_BINS = 256

    @staticmethod
    def _parse_snapshot(output):
        text = output.replace('\r', '')
        stats = re.search(r'PGMStats \S+ (\d+)x(\d+) min=(\d+) max=(\d+) mean=([\d.]+) p1=(\d+) p50=(\d+) p99=(\d+) saturated=(\d+)', text)
        if not stats:
            return None
        w, h, lo, hi, mean, p1, p50, p99, sat = stats.groups()
        data = dict(width=int(w), height=int(h),
                    stats=dict(min=int(lo), max=int(hi), mean=float(mean), p1=int(p1), p50=int(p50), p99=int(p99), saturated=int(sat)))
        bits = re.search(r'PGMBits OR=0x([0-9a-f]+) missing=(\S+)', text)
        if bits:
            data['bits'] = dict(seen=int(bits.group(1), 16), missing=bits.group(2))
        head = re.search(r'PGMProfile bins=(\d+) width=(\d+) scale=(\d+)', text)
        if head:
            bins, scale = int(head.group(1)), int(head.group(3))
            profile = dict(bins=bins, width=int(head.group(2)))
            for key, tag in (('mean', 'PGMProfileMean'), ('min', 'PGMProfileMin'), ('max', 'PGMProfileMax')):
                values = []
                for row in re.findall(r'^' + tag + r' \d+:([ \d]*)$', text, re.M):
                    values.extend(int(v) / scale for v in row.split())
                if len(values) != bins:
                    profile = None
                    break
                profile[key] = values
            if profile:
                data['profile'] = profile
        focus = re.search(r'PGMFocus sharpness=([\d.]+) contrast=([\d.]+) edge=(\w+)(?: x=(\d+) width_px=([\d.]+) edge_contrast=([\d.]+))?', text)
        if focus:
            data['focus'] = dict(sharpness=float(focus.group(1)), contrast=float(focus.group(2)),
                                 edge=dict(x=int(focus.group(4)), width_px=float(focus.group(5)), contrast=float(focus.group(6)))
                                 if focus.group(3) == 'found' else None)
            seg = re.search(r'^PGMFocusSeg((?: [\d.]+)+)$', text, re.M)
            if seg:
                data['focus']['segments'] = [float(v) for v in seg.group(1).split()]
        return data

    def _snap_once(self, name):
        """板上采一帧到 name，算统计 / 轮廓 / 对焦指标，更新 self.live。返回 (板上路径, 解析结果)。"""
        path = BOARD_CAPTURES + '/' + name
        rc, output = self.exchange(f"mkdir -p {BOARD_CAPTURES} && {APP} capture --timeout-ms 10000 --output {path}", 30)
        self._parse_preflight(output, rc)
        saved = re.search(r'Saved complete Mono8 frame: (\d+) x (\d+), (\d+) bytes', output)
        if rc or not saved:
            raise RuntimeError('未确认生成完整图像。请查看采集和停止日志。')
        w, h, n = (int(v) for v in saved.groups())
        with self.lock:
            self.last_frame = dict(name=name, path=path, location='board-ram', size_bytes=n,
                                   at=datetime.now().isoformat(timespec='seconds'), width=w, height=h)
        rc, output = self.exchange(f"{APP} pgm-stats {path} --quick --profile {self.LIVE_BINS} 2>&1", 30)
        data = None if rc else self._parse_snapshot(output)
        if not data:
            raise RuntimeError('图已采到，但板上统计失败（板上程序可能是旧版，没有 --profile）：' + output[-300:])
        now = datetime.now().isoformat(timespec='seconds')
        with self.lock:
            history = list(self.live.get('history', []))[-119:]
            focus = data.get('focus') or {}
            history.append(dict(at=now, sharpness=focus.get('sharpness'), mean=data['stats']['mean'], max=data['stats']['max'],
                                edge_width=(focus.get('edge') or {}).get('width_px')))
            self.live = dict(data, name=name, at=now, count=self.live.get('count', 0) + 1, history=history,
                             running=self.live.get('running', False), last_set=self.live.get('last_set'))
        return path, data

    @staticmethod
    def _snapshot_text(data):
        st, focus = data['stats'], data.get('focus') or {}
        edge = focus.get('edge')
        return (f"{data['width']} × {data['height']}，灰度 {st['min']}–{st['max']}，均值 {st['mean']:.1f}"
                + (f"，饱和 {st['saturated']} 个像素" if st['saturated'] else '')
                + (f"；最强边缘 {edge['width_px']:.2f} 像素（x = {edge['x']}）" if edge else '；画面里没有明显的边缘'))

    def _snap(self):
        """采一帧：统计、轮廓、对焦指标马上显示；fetch=True 时再把整张图传回电脑。"""
        name = 'frame-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6] + '.pgm'
        _, data = self._snap_once(name)
        text = '采到一帧：' + self._snapshot_text(data) + '。'
        if self.snap_options['fetch']:
            return text + self._fetch_last()
        return text + '图在板上内存里，需要看整张图再点「保存到电脑」。'

    def stop_live(self):
        """请求实时调试在当前这一帧采完后停下（不发 Ctrl+C，不会打断正在进行的采集）。"""
        with self.lock:
            if not self.busy or not self.job or self.job.get('action') != 'live':
                raise ValueError('实时调试没有在运行。')
            self.live_stop = True
        self.event('system', '已请求停止实时调试：当前这一帧采完就停。')

    def clear_live_history(self):
        """清空对焦曲线（换了靶标、重新开始对焦时用）。"""
        with self.lock:
            if self.live:
                self.live = dict(self.live, history=[])

    def _live(self):
        """实时调试：反复"采一帧 → 板上算轮廓和对焦指标"，直到点停止。每帧约 2–4 秒（走串口，只传数字不传图）。"""
        limit, done, previous = self.snap_options['count'], 0, None
        with self.lock:
            self.live = dict(self.live, running=True)
        try:
            while not self.live_stop and not self.cancel.is_set() and (not limit or done < limit):
                self._apply_live_sets()
                path, _ = self._snap_once(f"live-{datetime.now().strftime('%H%M%S')}-{done:04d}.pgm")
                if previous:
                    self.exchange(f"rm -f {previous}", 10)        # 只留最新一帧，免得占满板上内存
                previous, done = path, done + 1
        finally:
            with self.lock:
                self.live = dict(self.live, running=False)
                dropped, self.live_sets = self.live_sets, []
            if dropped:
                self.event('system', '实时调试已停止，未写入的修改作废：' + '、'.join(o['name'] for o in dropped))
        return f'实时调试结束，共 {done} 帧。最后一帧留在板上，可以「保存到电脑」。'

    def _apply_live_sets(self):
        """实时调试两帧之间：把排队的相机参数依次写入。写失败只记下原因，不中断实时调试。"""
        while True:
            with self.lock:
                if not self.live_sets:
                    return
                o = self.live_sets.pop(0)
            self.camera_set_options = o
            try:
                message, ok = self._camera_set(), True
            except Exception as exc:
                message, ok = str(exc), False
            with self.lock:
                self.live = dict(self.live, last_set=dict(name=o['name'], value=o['text'], ok=ok, message=message,
                                                          at=datetime.now().isoformat(timespec='seconds')))
            self.event('result' if ok else 'error', message)

    def _driver(self):
        """重新加载驱动，选择 DMA 模式（dma_fast=1：1 MiB 缓存缓冲区；否则 64 KiB 旧模式）。相机串口一并放开。"""
        fast = self.driver_options['dma_fast']
        rc, output = self.exchange(f"if [ -d /sys/module/sg3000_rx ]; then rmmod sg3000_rx || exit 9; fi; "
                                   f"insmod {BOARD_DIR}/sg3000_rx.ko cam_serial=1{' dma_fast=1' if fast else ''} && test -e {DEV_NODE} && "
                                   f"printf 'CAM_SERIAL='; cat {CAM_SERIAL_PARAM}; printf 'DMA_FAST='; cat {DMA_FAST_PARAM} 2>/dev/null || echo MISSING; "
                                   f"dmesg | tail -n 3", 30)
        self._read_driver_params(output)
        if rc:
            raise RuntimeError('驱动没有加载成功：' + output[-500:])
        if fast and self.driver.get('dma_fast') != 'Y':
            raise RuntimeError('驱动已加载，但快速 DMA 没有生效（这份 sg3000_rx.ko 没有 dma_fast 参数，或 2 MiB 缓冲区申请不到）。现在是旧模式。')
        self.checks['prepared'] = True
        return f"驱动已重新加载：相机串口已放开，DMA {'快速模式（1 MiB）' if fast else '旧模式（64 KiB）'}。"

    def _read_driver_params(self, output):
        text = output.replace('\r', '')
        cam = re.search(r'^CAM_SERIAL=(\S*)', text, re.M)
        fast = re.search(r'^DMA_FAST=(\S*)', text, re.M)
        if cam or fast:
            with self.lock:
                self.driver = dict(cam_serial=cam.group(1) if cam else '', dma_fast=fast.group(1) if fast else '',
                                   at=datetime.now().isoformat(timespec='seconds'))

    def _bench(self):
        """板上 CPU 基准（不碰采集卡）：各处理环节一个核的速度，是 15 kHz 数据率的多少倍。"""
        rc, output = self.exchange(f"{APP} bench 2>&1", 180)
        rows = [dict(name=n, ms=float(ms), mbps=float(mbps), x15k=float(x), extra=extra.strip())
                for n, ms, mbps, x, extra in re.findall(r'^Bench name=(\w+) ms=([\d.]+) MBps=([\d.]+) x15k=([\d.]+)([^\r\n]*)', output.replace('\r', ''), re.M)]
        if rc or not rows:
            raise RuntimeError('基准没有跑出来（板上程序可能是旧版）：' + output[-300:])
        with self.lock:
            self.bench = rows
        slow = [r['name'] for r in rows if r['x15k'] < 1]
        return 'CPU 基准完成：' + '；'.join(f"{r['name']} {r['mbps']:.0f} MB/s（{r['x15k']:.2f}×）" for r in rows) + \
               ('。一个核跟不上 15 kHz 的环节：' + '、'.join(slow) if slow else '。各环节一个核都跟得上 15 kHz') + '。'

    @staticmethod
    def _check_net_options(options):
        pc, board = str(options.get('pc_ip', '')).strip(), str(options.get('board_ip', '')).strip()
        try:
            ipaddress.IPv4Address(pc)
            if board:
                ipaddress.IPv4Address(board)
        except ValueError:
            raise ValueError('IP 地址格式不对，应类似 192.168.1.10。板子 IP 可以留空（板子已有地址时）。')
        if pc == board:
            raise ValueError('电脑和板子不能用同一个 IP。')
        return dict(pc_ip=pc, board_ip=board)

    def _ensure_receiver(self):
        if self.receiver is not None and self.receiver.running:
            return
        layout = rx_tcp.Layout(CAPTURES, CAPTURES / 'candidates', CAPTURES / 'status.jsonl')
        self.receiver = rx_tcp.Receiver(layout, self._on_net_message, lambda text: self.event('system', text))
        try:
            self.receiver.start('0.0.0.0', NET_PORT)
        except OSError as exc:
            self.receiver = None
            raise RuntimeError(f'电脑上 {NET_PORT} 端口打不开（可能被别的程序占用）：{exc}')
        self.event('system', f'电脑端网口接收已启动，端口 {NET_PORT}，存到 {CAPTURES}。')

    def _on_net_message(self, kind, seq, block_id, text, path):
        with self.cv:
            if kind == rx_tcp.TYPE_FRAME:
                self.net_frames.append(path)
                self.cv.notify_all()
            elif kind == rx_tcp.TYPE_CANDIDATES:
                self.net_candidates += 1
        if kind == rx_tcp.TYPE_FRAME or (kind == rx_tcp.TYPE_CANDIDATES and not text.startswith('0 candidates')):
            self.event('net', f'#{seq} 块 {block_id}：{text}')

    def _wait_net_frame(self, before, timeout):
        deadline = time.monotonic() + timeout
        with self.cv:
            while len(self.net_frames) == before or (self.net_frames and self.net_frames[-1] is None):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('板子说发出去了，但电脑在规定时间内没收到完整的图。')
                self.cv.wait(min(remaining, 0.2))
            return self.net_frames[-1]

    def _network(self):
        """配置板子网口 → 电脑开始监听 → 板子发一张 4×1 的测试图 → 电脑确认收到且内容一致。"""
        o = self.net_options
        self.net = {}
        if APP_VERSION not in self.run_checked(APP + ' version'):
            raise RuntimeError('先执行「从 SD 加载程序」：网口测试要用新版 sg3000-capture。')
        self._ensure_receiver()
        # 板上可能只有 ifconfig（BusyBox），也可能有 ip（iproute2），两种都支持
        if o['board_ip']:
            b = o['board_ip']
            self.run_checked(f"if command -v ip >/dev/null; then ip link set eth0 up && "
                             f"(ip -4 addr show dev eth0 | grep -q 'inet {b}/' || ip addr add {b}/24 dev eth0); "
                             f"else ifconfig eth0 {b} netmask 255.255.255.0 up; fi")
        self.run_checked('if command -v ip >/dev/null; then ip -4 addr show dev eth0; else ifconfig eth0; fi')
        before = len(self.net_frames)
        rc, output = self.exchange(f"printf 'P5\\n4 1\\n255\\nABCD' > {BOARD_DIR}/nettest.pgm && "
                                   f"{APP} send {BOARD_DIR}/nettest.pgm --to {o['pc_ip']}:{NET_PORT}", 20)
        if rc:
            raise RuntimeError(f"板子连不上电脑 {o['pc_ip']}:{NET_PORT}。依次检查：网线是否插在板子网口和电脑之间；"
                               f"电脑网卡 IP 是否是 {o['pc_ip']}、和板子同一网段；Windows 防火墙是否允许 Python 接收连接。"
                               + output[-300:])
        path = self._wait_net_frame(before, 5)
        data = Path(path).read_bytes()
        Path(path).unlink()
        if not data.endswith(b'ABCD'):
            raise RuntimeError('收到的测试图内容不对，网口数据有错。')
        self.net = dict(o, port=NET_PORT)
        return f"网口已通：板子 → 电脑 {o['pc_ip']}:{NET_PORT}。之后回传图像、连续候选都走网口。"

    def _fetch_over_network(self, target):
        before = len(self.net_frames)
        rc, output = self.exchange(f"{APP} send '{target}' --to {self.net['pc_ip']}:{self.net['port']}", 60)
        sent = re.search(r'Sent frame: (\d+) x (\d+), (\d+) bytes', output)
        if rc or not sent:
            raise RuntimeError('网口发送失败。可以重新做「配置网口」，或清空网口设置后走串口。' + output[-400:])
        path = Path(self._wait_net_frame(before, 30))
        final = CAPTURES / Path(target).name
        if not final.exists():
            os.replace(path, final)
            path = final
        w, h, n = (int(v) for v in sent.groups())
        self._make_preview(path)
        self.event('system', f'图像已经网口存入电脑：{path}（{n:,} 字节）')
        return path, n, w, h

    @staticmethod
    def _make_preview(pgm_path):
        try:
            from PIL import Image
            img = Image.open(pgm_path)
            img.convert('L').save(Path(pgm_path).with_name(Path(pgm_path).stem + '_preview.png'))
        except Exception:
            pass

    def _replay(self):
        path = Path(__file__).resolve().parents[1] / 'evidence' / 'synthetic-stream.jsonl'
        self.event('system', '离线回放：记录来自合成图像的软件测试，不连接硬件。')
        for line in path.read_text(encoding='utf-8').splitlines():
            if self.cancel.wait(0.35):
                self.stream['status'] = 'replay-stopped'
                return '离线回放已停止。'
            self.accept_stream_line(line, 'recorded-replay')
        return '离线回放完成；未读取相机或运行板上 DMA。'
