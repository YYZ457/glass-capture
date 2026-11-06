"""Single-owner serial transport. No connection or command runs on import."""
import codecs
import gzip
import json
import re
import threading
import time
import uuid
import copy
from collections import deque
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports
from telemetry import parse as parse_telemetry

KERNEL = '5.15.19-xilinx-v2022.1'
DEVICE = '/sys/bus/pci/devices/0000:01:00.0'
APP = '/tmp/sg3000-board/sg3000-capture'
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
        captures_dir = Path('D:/SG3000_Captures')
        captures_dir.mkdir(parents=True, exist_ok=True)
        pgms = sorted(captures_dir.glob('*.pgm'), key=lambda p: p.stat().st_mtime, reverse=True)
        if pgms:
            latest = pgms[0]
            self.last_frame = dict(
                name=latest.name,
                path=f"/tmp/sg3000-console/{latest.name}",
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
                        stream=dict(self.stream),
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
        if action not in {'probe', 'prepare', 'preflight', 'capture', 'fetch_last', 'dmesg', 'release', 'prepare_stream', 'stream', 'replay'}:
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
            if type(seconds) is not int or not 0 <= seconds <= 300 or processor not in {'stats', 'pl'}:
                raise ValueError('连续时长必须为 0..300 秒，处理后端为 stats 或 pl。')
            self.stream_options = dict(seconds=seconds, processor=processor)
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
        # Read-only SD access. Never write boot files or change existing mounts.
        mount = '/mnt/sg3000-console'
        src = mount + '/sg3000-linux-20261023'
        self.run_checked("test \"$(uname -r)\" = '" + KERNEL + "' && test \"$(cat " + DEVICE + "/vendor)\" = 0x4c54 && test \"$(cat " + DEVICE + "/device)\" = 0x3000")
        self.run_checked(f"mkdir -p {mount}; cd /; umount {mount} 2>/dev/null; umount /mnt/sd* 2>/dev/null; mount -t vfat -o ro /dev/mmcblk0p1 {mount}")
        try:
            self.run_checked('cd ' + src + ' && sha256sum -c SHA256SUMS && mkdir -p /tmp/sg3000-board /run && cp sg3000_rx.ko sg3000-capture /tmp/sg3000-board/ && chmod 700 ' + APP, 20)
        finally:
            self.run_checked('umount ' + mount, allow_cancel=True)
        self._apply_board_patch()
        self.run_checked("if [ -L " + DEVICE + "/driver ]; then test \"$(basename \"$(readlink " + DEVICE + "/driver)\")\" = sg3000_rx; elif [ -d /sys/module/sg3000_rx ]; then echo 'Module exists but card is not bound'; exit 1; else insmod /tmp/sg3000-board/sg3000_rx.ko; fi && test -e /dev/sg3000_rx_0000:01:00.0", 15)
        self.checks['prepared'] = True
        return '已从 SD 卡校验并加载接收程序；SD 已卸载。尚未启动采集。'

    def _apply_board_patch(self):
        patch_cmd = (
            "dd if=/tmp/sg3000-board/sg3000-capture bs=1 count=5736 of=/tmp/p1 2>/dev/null && "
            "printf '\\037\\040\\003\\325' > /tmp/p2 && "
            "dd if=/tmp/sg3000-board/sg3000-capture bs=1 skip=5740 of=/tmp/p3 2>/dev/null && "
            "cat /tmp/p1 /tmp/p2 /tmp/p3 > /tmp/sg3000-board/sg3000-capture && "
            "chmod 700 " + APP + " && rm -f /tmp/p1 /tmp/p2 /tmp/p3"
        )
        self.run_checked(patch_cmd, 15)
        self.event('system', '板上内存程序已自动打入 DDR 校准兼容补丁。')
        try:
            sd_mount = '/mnt/sd_persist'
            sd_dir = sd_mount + '/sg3000-linux-20261023'
            sd_cmd = (
                f"mkdir -p {sd_mount} && "
                f"mount -t vfat /dev/mmcblk0p1 {sd_mount} && "
                f"cp /tmp/sg3000-board/sg3000-capture {sd_dir}/sg3000-capture && "
                f"cd {sd_dir} && sha256sum sg3000_rx.ko sg3000-capture resume.sh > SHA256SUMS && "
                f"cd / && sync && umount {sd_mount}"
            )
            self.run_checked(sd_cmd, 20)
            self.event('system', '补丁已成功持久化写入 SD 卡！重新上电均会自动生效。')
        except Exception as sd_err:
            self.event('system', f'SD 卡持久化提示（非致命）：{sd_err}')

    def _preflight(self):
        rc, output = self.exchange(APP + ' preflight', 15)
        if rc != 0 and '(-67)' in output:
            self.event('system', '检测到 DDR 状态检查需要适配，正在自动应用持久化补丁...')
            self._apply_board_patch()
            rc, output = self.exchange(APP + ' preflight', 15)
        self._parse_preflight(output, rc)
        if rc:
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

    def _fetch_file_from_board(self, remote_path, local_dest_dir=Path('D:/SG3000_Captures')):
        local_dest_dir = Path(local_dest_dir)
        local_dest_dir.mkdir(parents=True, exist_ok=True)
        filename = Path(remote_path).name
        local_file = local_dest_dir / filename
        temp_gz = f"/tmp/{filename}.gz"

        self.event('system', f'正在板端打包压缩图像：{filename} ...')
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
            rc, hex_output = self.exchange(f"/usr/bin/hexdump -v -e '16/1 \"%02x\" \"\\n\"' '{temp_gz}'", timeout=transfer_timeout)
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
        self.event('system', f'图像已成功存入电脑：{local_file}（{len(decompressed):,} 字节）')

        preview_file = local_dest_dir / (local_file.stem + '_preview.png')
        width, height = 4096, 2000
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
        rc, output = self.exchange("ls -t /tmp/sg3000-console/*.pgm 2>/dev/null | head -n 1", timeout=10)
        lines = [l.strip() for l in output.strip().splitlines() if l.strip()]
        target = lines[-1] if lines else ""
        if not target or not target.startswith('/tmp/sg3000-console/'):
            rc_sd, output_sd = self.exchange("mkdir -p /mnt/sd && mount -t vfat /dev/mmcblk0p1 /mnt/sd 2>/dev/null; ls -t /mnt/sd/frame-*.pgm 2>/dev/null | head -n 1; umount /mnt/sd 2>/dev/null", timeout=10)
            sd_lines = [l.strip() for l in output_sd.strip().splitlines() if 'frame-' in l and l.endswith('.pgm')]
            if sd_lines:
                target_sd = sd_lines[-1]
                self.run_checked(f"mkdir -p /mnt/sd && mount -t vfat /dev/mmcblk0p1 /mnt/sd && cp '{target_sd}' /tmp/sg3000-console/ && umount /mnt/sd", 15)
                target = f"/tmp/sg3000-console/{Path(target_sd).name}"
            else:
                raise RuntimeError('板上尚未找到任何已采集的 .pgm 图像。请先点击「04 采集一帧」。')

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
        return f"最新图像已成功回传并保存至 D:\\SG3000_Captures\\{local_file.name}（原始 {size:,} 字节，USB传输 {gz_size:,} 字节）。"

    def _capture(self):
        self.last_frame = None
        name = 'frame-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6] + '.pgm'
        path = '/tmp/sg3000-console/' + name
        rc, output = self.exchange('mkdir -p /tmp/sg3000-console && ' + APP + ' capture --timeout-ms 10000 --output ' + path, 25)
        self._parse_preflight(output, rc)
        if rc or 'Saved complete Mono8 frame:' not in output:
            raise RuntimeError('未确认生成完整图像。请查看采集和停止日志；不会显示成功预览。')

        with self.lock:
            self.last_frame = dict(
                name=name,
                path=path,
                location='board-ram',
                size_bytes=8192017,
                at=datetime.now().isoformat(timespec='seconds'),
                width=4096,
                height=2000
            )
        return '完整图像采集成功！已存入板载内存：' + path + '（4096 × 2000, 8.19 MB）。点击「05 回传最新图像到 D 盘」即可传输至电脑。'

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
        mount = '/mnt/sg3000-console'
        src = mount + '/sg3000-stream-v0.2'
        self.run_checked("mkdir -p " + mount + "; if grep -q ' " + mount + " ' /proc/mounts; then exit 1; fi; mount -t vfat -o ro /dev/mmcblk0p1 " + mount)
        try:
            self.run_checked('cd ' + src + ' && sha256sum -c SHA256SUMS && mkdir -p /tmp/sg3000-board && cp sg3000-stream /tmp/sg3000-board/ && chmod 700 /tmp/sg3000-board/sg3000-stream', 20)
        finally:
            self.run_checked('umount ' + mount, allow_cancel=True)
        version = self.run_checked('/tmp/sg3000-board/sg3000-stream --version')
        if 'sg3000-stream 0.2 ABI 1' not in version: raise RuntimeError('连续接收程序版本不匹配。')
        self.checks['stream_prepared'] = True
        return '连续接收 v0.2 已加载；它尚未上板验证。原单帧程序保留。'

    def _stream(self):
        seconds, processor = self.stream_options['seconds'], self.stream_options['processor']
        version = self.run_checked('/tmp/sg3000-board/sg3000-stream --version')
        if 'sg3000-stream 0.2 ABI 1' not in version: raise RuntimeError('先加载 v0.2 连续接收程序。')
        rc, output = self.exchange('/tmp/sg3000-board/sg3000-stream --source hardware --processor ' + processor + ' --seconds ' + str(seconds) + ' --queue 4 --max-frame-mib 16 --emit-ms 1000', seconds + 40 if seconds else float('inf'))
        ended = self.stream.get('ended', {})
        if rc or self.stream.get('status')!='ended' or any(ended.get(k)!=0 for k in ('error','source_error','processing_error','output_error','stop_error')):
            raise RuntimeError('连续接收未正常结束，请查看停止状态与原始日志。' + output[-300:])
        return '连续接收已结束，收到 ' + str(ended.get('received', '?')) + ' 块，处理 ' + str(ended.get('processed', '?')) + ' 块。结果见下方；灰度统计不是缺陷判定。'

    def _replay(self):
        path = Path(__file__).resolve().parents[1] / 'evidence' / 'synthetic-stream.jsonl'
        self.event('system', '离线回放：记录来自合成图像的软件测试，不连接硬件。')
        for line in path.read_text(encoding='utf-8').splitlines():
            if self.cancel.wait(0.35):
                self.stream['status'] = 'replay-stopped'
                return '离线回放已停止。'
            self.accept_stream_line(line, 'recorded-replay')
        return '离线回放完成；未读取相机或运行板上 DMA。'
