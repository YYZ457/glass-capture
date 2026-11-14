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
                    if self.job and self.job.get('action') == 'stream' and self.busy:
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
        if action not in {'probe', 'prepare', 'preflight', 'capture', 'fetch_last', 'dmesg', 'release', 'prepare_stream', 'stream', 'replay', 'status', 'network', 'card_xml', 'card_set', 'camera_probe', 'camera_xml', 'camera_regs', 'camera_set'}:
            raise ValueError('未知操作。')
        with self.lock:
            if action != 'replay' and (not self.connected or not self.shell_ready):
                raise ValueError('先连接并唤醒 Linux root 终端；如出现登录界面，请先用串口工具登录。')
            if self.busy:
                raise ValueError('上一个操作尚未完成。')
            if action not in {'probe', 'dmesg', 'replay', 'fetch_last'} and not self.identity_ok:
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
            if action in {'stream', 'replay'}:
                self.stream = {'channel': 'recorded-replay' if action == 'replay' else 'board', 'status': 'starting'}
                self.stream_lines = ''
            self.busy = True
            self.cancel.clear()
            self.job = dict(action=action, status='running', message='正在执行', started=datetime.now().isoformat(timespec='seconds'))
            threading.Thread(target=self._job, args=(action,), daemon=True).start()

    def _job(self, action):
        try:
            result = getattr(self, '_' + action)()
            if self.cancel.is_set() and action not in {'stream', 'replay'}:
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
                             f"cp sg3000-capture sg3000-stream sg_status {BOARD_DIR}/ && chmod 700 {BOARD_DIR}/sg*", 20)
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
        rc, output = self.exchange(STATUS_APP, 15)
        self._parse_preflight_status(output)
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
        self.run_checked(f"rmmod sg3000_rx && insmod {BOARD_DIR}/sg3000_rx.ko cam_serial=1 && "
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
                       'TestPattern': (0, 1, 2, 3, 5), 'AnalogGain': (0, 1, 2, 3, 4)}
    CAMERA_SET_FLOATS = {'ExposureTime': (0.0, 100000.0), 'DigitalGain': (0.10, 15.99)}

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
                         'TestPattern 0/1/2/3/5，ExposureTime 微秒，DigitalGain 0.10–15.99，AnalogGain 0–4。')

    def _camera_set(self):
        """写相机的 Camera Link 输出模式（不保存，相机断电恢复）。写后读回核对。"""
        o = self.camera_set_options
        self._ensure_cam_serial()
        rc, output = self.exchange(f"{APP} camera-set {o['name']} {o['text']} 2>&1", 30)
        done = re.search(r'CameraSet (\w+) 0x[0-9a-f]+: (\d+) -> (\d+), readback (\d+)', output)
        if rc or not done:
            raise RuntimeError('相机参数没写成：' + output[-400:])
        state = re.search(r'CameraState ClConfiguration=(\d+).*?DeviceTapGeometry=(\d+)', output)
        old, _, back = (int(x) for x in done.groups()[1:])
        mode = {0: 'Base', 3: 'EightyBit'}
        now = (f"相机现在 ClConfiguration={state.group(1)}（{mode.get(int(state.group(1)), '?')}）、"
               f"DeviceTapGeometry={state.group(2)}。") if state else ''
        value = re.search(r'CameraValue \w+ = ([^\r\n]*)', output)
        return (f"相机 {o['name']}：{old} → {back}（已读回确认" + (f"，= {value.group(1)}" if value else '') + "）。" + now +
                '采集卡要改成同样的模式（见 BRINGUP 第 5¾ 节）；相机断电后恢复 EightyBit。')

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
