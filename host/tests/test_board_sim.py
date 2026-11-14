"""中控台 × 模拟板子 端到端测试。

中控台发出的每条命令交给真的 bash 执行，板上程序是本仓库 x86 编译出来的同一份源码；
只有板子特有的命令（uname、mount、umount、insmod、ip）用替身脚本。验证：
  SD 拷贝与校验 → 版本检查 → 驱动加载 → 只读体检报错路径 → 网口配置与测试
  → 经网口回传图像（逐字节相同）→ 连续程序命令行带 --tcp
需要 Linux、bash、gcc、make；Windows 上自动跳过。
"""
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

pytestmark = pytest.mark.skipif(os.name == "nt" or not shutil.which("gcc") or not shutil.which("bash"),
                                reason="needs Linux with gcc and bash")

SHIMS = {
    "uname": '#!/bin/sh\ncase "$1" in -r) echo 5.15.19-xilinx-v2022.1;; -m) echo aarch64;; *) echo Linux;; esac\n',
    "mount": '#!/bin/sh\nfor a; do last=$a; done\ncase "$*" in *mmcblk0p1*) rmdir "$last" 2>/dev/null; ln -sfn "$SIM_SD" "$last";; esac\nexit 0\n',
    "umount": '#!/bin/sh\n[ -L "$1" ] && unlink "$1" && mkdir -p "$1"\nexit 0\n',
    "insmod": '#!/bin/sh\ntouch "$SIM_DEVNODE"\n',
    "ip": '#!/bin/sh\ncase "$*" in *show*) echo "2: eth0: <UP>"; echo "    inet 127.0.0.1/8 scope host eth0";; esac\nexit 0\n',
}


class ShellSerial:
    def __init__(self, env):
        self.q, self.pending, self.env = queue.Queue(), b"", env

    def read(self, n):
        try:
            return self.q.get(timeout=.02)
        except queue.Empty:
            return b""

    def write(self, data):
        self.pending += data
        while b"\r" in self.pending:
            line, self.pending = self.pending.split(b"\r", 1)
            if line.strip():
                threading.Thread(target=self.run, args=(line.decode(),), daemon=True).start()
            else:
                self.q.put(b"\r\n/ # ")
        return len(data)

    def run(self, cmd):
        out = subprocess.run(["bash", "-c", cmd], capture_output=True, env=self.env)
        self.q.put((out.stdout + out.stderr).replace(b"\n", b"\r\n") + b"/ # ")

    def flush(self):
        pass

    def close(self):
        pass


@pytest.fixture()
def board(tmp_path, monkeypatch):
    bins = tmp_path / "bin"
    subprocess.run(["make", "-s", "-C", str(REPO / "board"), f"OUT={bins}", "all"], check=True,
                   capture_output=True)
    sd = tmp_path / "sd"
    subprocess.run([sys.executable, str(REPO / "tools" / "make_sd_bundle.py"), "--bin-dir", str(bins),
                    "--out", str(sd / "glass-capture")], check=True, capture_output=True)
    legacy = sd / "sg3000-linux-20261023"
    legacy.mkdir()
    (legacy / "sg3000_rx.ko").write_text("fake ko\n")
    subprocess.run("sha256sum sg3000_rx.ko > SHA256SUMS", shell=True, cwd=legacy, check=True)
    shim = tmp_path / "shim"
    shim.mkdir()
    shims = dict(SHIMS, hexdump='#!/bin/sh\n# 只模拟 hexdump -v -e FMT FILE：输出十六进制\nfor f; do :; done\nod -An -v -tx1 "$f"\n')
    for name, text in shims.items():
        (shim / name).write_text(text)
        (shim / name).chmod(0o755)
    dev = tmp_path / "pci"
    dev.mkdir()
    (dev / "vendor").write_text("0x4c54\n")
    (dev / "device").write_text("0x3000\n")
    for d in ("board", "boardcap", "mnt", "captures"):
        (tmp_path / d).mkdir()

    monkeypatch.setenv("SG_CAPTURES_DIR", str(tmp_path / "captures"))
    sys.path.insert(0, str(REPO / "host" / "console"))
    import transport as T
    monkeypatch.setattr(T, "CAPTURES", tmp_path / "captures")
    monkeypatch.setattr(T, "NET_PORT", 18000 + os.getpid() % 1000)
    monkeypatch.setattr(T, "DEVICE", str(dev))
    monkeypatch.setattr(T, "DEV_NODE", str(tmp_path / "devnode"))
    monkeypatch.setattr(T, "SD_MOUNT", str(tmp_path / "mnt"))
    monkeypatch.setattr(T, "SD_BUNDLE", str(tmp_path / "mnt" / "glass-capture"))
    monkeypatch.setattr(T, "SD_LEGACY", str(tmp_path / "mnt" / "sg3000-linux-20261023"))
    monkeypatch.setattr(T, "BOARD_DIR", str(tmp_path / "board"))
    monkeypatch.setattr(T, "APP", str(tmp_path / "board" / "sg3000-capture"))
    monkeypatch.setattr(T, "STREAM_APP", str(tmp_path / "board" / "sg3000-stream"))
    monkeypatch.setattr(T, "STATUS_APP", str(tmp_path / "board" / "sg_status"))
    monkeypatch.setattr(T, "BOARD_CAPTURES", str(tmp_path / "boardcap"))
    monkeypatch.setattr(T, "CARD_XML_DIR", str(tmp_path / "board" / "card-xml"))
    monkeypatch.setattr(T, "HEXDUMP", str(shim / "hexdump"))

    env = dict(os.environ, PATH=f"{shim}:{os.environ['PATH']}", SIM_SD=str(sd),
               SIM_DEVNODE=str(tmp_path / "devnode"))
    console = T.Console(tmp_path / "runtime", serial_factory=lambda name: ShellSerial(env))
    console.connect("COM9")
    console.shell_ready = True
    console.identity_ok = True        # probe 读 /proc/device-tree，模拟环境里没有
    yield console, tmp_path
    if console.receiver:
        console.receiver.stop()


def run(console, action, options=None):
    console.start(action, options)
    deadline = time.monotonic() + 60
    while console.busy and time.monotonic() < deadline:
        time.sleep(.05)
    return console.job


def test_full_flow(board):
    console, tmp = board
    job = run(console, "prepare")
    assert job["status"] == "success", job
    assert (tmp / "board" / "sg3000_rx.ko").read_text() == "fake ko\n"

    job = run(console, "status")          # 假设备节点不是真驱动：退出码 2
    assert job["status"] == "error" and "打不开" in job["message"]

    job = run(console, "card_xml")        # 同上：真程序打不开假设备
    assert job["status"] == "error" and "打不开" in job["message"]

    job = run(console, "network", {"pc_ip": "127.0.0.1", "board_ip": ""})
    assert job["status"] == "success", job

    image = b"P5\n300 7\n255\n" + bytes((i * 7) & 255 for i in range(2100))
    (tmp / "boardcap" / "frame-sim.pgm").write_bytes(image)
    job = run(console, "fetch_last")
    assert job["status"] == "success", job
    assert (tmp / "captures" / "frame-sim.pgm").read_bytes() == image
    assert (console.last_frame["width"], console.last_frame["height"]) == (300, 7)

    job = run(console, "stream", {"seconds": 1, "processor": "candidates"})
    assert job["status"] == "error"       # 没有真卡
    sent = [e["text"] for e in console.events if e["kind"] == "tx" and "--source hardware" in e["text"]]
    assert sent and "--processor candidates" in sent[-1] and "--tcp 127.0.0.1:" in sent[-1]


def test_missing_bundle_is_explained(board):
    console, tmp = board
    shutil.rmtree(tmp / "sd" / "glass-capture")
    job = run(console, "prepare")
    assert job["status"] == "error" and "glass-capture" in job["message"]


def test_card_xml_fetched(board):
    """sg_status --card-xml 存好文件后，中控台把它取回电脑、内容一致。"""
    console, tmp = board
    xml = b"<?xml version='1.0'?><RegisterDescription>" + bytes(range(256)) + b"</RegisterDescription>"
    (tmp / "card.bin").write_bytes(xml)
    fake = tmp / "board" / "sg_status"
    fake.write_text("#!/bin/bash\n[ \"$1\" = --card-xml ] || exit 9\n"
                    f"cp '{tmp / 'card.bin'}' \"$2/SGCard.zip\"\n"
                    "echo \"CardXML saved: $2/SGCard.zip (300 bytes, 1 s)\"\n")
    fake.chmod(0o755)
    job = run(console, "card_xml")
    assert job["status"] == "success", job
    assert (tmp / "captures" / "card-xml" / "SGCard.zip").read_bytes() == xml

    fake.write_text("#!/bin/bash\necho 'CardXML FAILED: URL is not \"Local:<name>;<addr>;<len>\"'\nexit 3\n")
    job = run(console, "card_xml")
    assert job["status"] == "error" and "URL is not" in job["message"]


def test_prepare_accepts_crlf_checksums(board):
    """Windows 检出（core.autocrlf=true）会把 SHA256SUMS 变成 CRLF，「从 SD 加载程序」仍要通过。"""
    console, tmp = board
    sums = tmp / "sd" / "glass-capture" / "SHA256SUMS"
    sums.write_bytes(sums.read_bytes().replace(b"\n", b"\r\n"))
    legacy = tmp / "sd" / "sg3000-linux-20261023" / "SHA256SUMS"
    legacy.write_bytes(legacy.read_bytes().replace(b"\n", b"\r\n"))
    job = run(console, "prepare")
    assert job["status"] == "success", job


def test_card_set(board):
    """卡参数写入：值域在电脑端先挡一次；板上程序读回一致才算成功。"""
    console, tmp = board
    with pytest.raises(ValueError):
        console.start("card_set", {"name": "CameraWidth", "value": 100})
    with pytest.raises(ValueError):
        console.start("card_set", {"name": "DeviceReboot", "value": 1})
    fake = tmp / "board" / "sg3000-capture"
    fake.write_text("#!/bin/bash\n[ \"$1\" = card-set ] || exit 9\n"
                    "echo \"CardSet $2 0x10203008: 4096 -> $3, readback $3\"\n")
    fake.chmod(0o755)
    job = run(console, "card_set", {"name": "CameraWidth", "value": 16384})
    assert job["status"] == "success" and "4096 → 16384" in job["message"], job
    fake.write_text("#!/bin/bash\necho 'Refused: acquisition is not stopped (SG_ACQ_STATE 0x00000000)' >&2\nexit 1\n")
    job = run(console, "card_set", {"name": "CameraWidth", "value": 16384})
    assert job["status"] == "error" and "not stopped" in job["message"]


CAMERA_XML = b"""<?xml version="1.0"?>
<RegisterDescription xmlns="http://www.genicam.org/GenApi/Version_1_1" ModelName="Test" VendorName="T">
  <Integer Name="Width"><pValue>WidthReg</pValue></Integer>
  <IntReg Name="WidthReg"><Address>0x1000</Address><Length>4</Length><AccessMode>RW</AccessMode>
    <pPort>Device</pPort><Endianess>BigEndian</Endianess></IntReg>
  <Enumeration Name="ClConfiguration"><EnumEntry Name="Base"><Value>0</Value></EnumEntry>
    <EnumEntry Name="Deca"><Value>4</Value></EnumEntry><pValue>ClConfigReg</pValue></Enumeration>
  <MaskedIntReg Name="ClConfigReg"><Address>0x2000</Address><Length>4</Length><AccessMode>RW</AccessMode>
    <pPort>Device</pPort><LSB>3</LSB><MSB>0</MSB><Endianess>BigEndian</Endianess></MaskedIntReg>
  <Integer Name="ExposureTime"><pValue>ExpCalc</pValue></Integer>
  <IntSwissKnife Name="ExpCalc"><pVariable Name="R">WidthReg</pVariable><Formula>R*2</Formula></IntSwissKnife>
</RegisterDescription>
"""


def test_camera_xml_and_regs(board):
    """相机 XML：驱动是 cam_serial=N 时先重载；SHA1 不符要报错；取回后按 XML 读寄存器并翻译成名字。"""
    import hashlib
    import io
    import zipfile
    console, tmp = board
    import transport as T
    param = tmp / "cam_serial"
    T.CAM_SERIAL_PARAM = str(param)
    T.CAMERA_XML_DIR = str(tmp / "board" / "camera-xml")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("Test.xml", CAMERA_XML)
    blob = buf.getvalue()
    (tmp / "cam.zip").write_bytes(blob)
    sha = hashlib.sha1(blob).hexdigest()
    fake = tmp / "board" / "sg3000-capture"

    # 1. 旧驱动：没有参数文件 → 明确报错，不去 insmod
    fake.write_text("#!/bin/bash\nexit 9\n")
    fake.chmod(0o755)
    job = run(console, "camera_xml")
    assert job["status"] == "error" and "旧驱动" in job["message"], job

    # 2. 参数是 N：rmmod + insmod cam_serial=1 后变 Y；SHA1 一致 → 取回
    param.write_text("N\n")
    shimx = tmp / "shimx"
    shimx.mkdir()
    (shimx / "rmmod").write_text("#!/bin/sh\nexit 0\n")
    (shimx / "insmod").write_text(f"#!/bin/sh\n[ \"$2\" = cam_serial=1 ] || exit 5\ntouch \"$SIM_DEVNODE\"\necho Y > '{param}'\n")
    for f in shimx.iterdir():
        f.chmod(0o755)
    console.port.env["PATH"] = f"{shimx}:{console.port.env['PATH']}"
    fake.write_text("#!/bin/bash\ncase \"$1\" in\n"
                    f"camera-xml) cp '{tmp / 'cam.zip'}' \"$2/Test_1.0.0.zip\"\n"
                    "  echo 'CameraManifest[0] file=1.0.0 type=zip addr=0x100000 size=%d sha1=%s'\n"
                    "  echo \"CameraXML saved: $2/Test_1.0.0.zip (%d bytes, 3 s)\"\n"
                    "  echo 'CameraXML SHA1 expected %s';;\n"
                    "camera-regs) echo 'CameraRegs port=0 baud=115200 count=2'\n"
                    "  for a in \"${@:2}\"; do case $a in WidthReg@0x1000:4) echo 'CameraReg WidthReg 0x1000 = 0x4000 (16384)';;\n"
                    "  ClConfigReg@0x2000:4) echo 'CameraReg ClConfigReg 0x2000 = 0x40000000 (1073741824)';; *) exit 7;; esac; done\n"
                    "  echo 'CameraRegs done: 2 ok, 0 failed';;\n"
                    "*) exit 9;; esac\n" % (len(blob), sha, len(blob), sha))
    job = run(console, "camera_xml")
    assert job["status"] == "success" and "SHA1 与相机一致" in job["message"], job
    assert param.read_text().strip() == "Y"
    assert (tmp / "captures" / "camera-xml" / "Test_1.0.0.zip").read_bytes() == blob

    # 3. 读寄存器：Width = 16384，ClConfiguration：大端编号 LSB=3/MSB=0 是最高 4 位 = 4 = Deca；算出来的 ExposureTime 不读
    job = run(console, "camera_regs")
    assert job["status"] == "success", job
    assert "Width=16384" in job["message"] and "Deca" in job["message"], job

    # 4. SHA1 不符 → 报错
    fake.write_text(fake.read_text().replace(sha, "0" * 39 + "1"))
    job = run(console, "camera_xml")
    assert job["status"] == "error" and "SHA1" in job["message"], job


def test_camera_set_and_two_cable_hint(board):
    """相机写：电脑端先挡值域；板上读回一致才算成功。EightyBit 缺第二根线时，体检给出明确提示。"""
    console, tmp = board
    import transport as T
    param = tmp / "cam_serial"
    param.write_text("Y\n")
    T.CAM_SERIAL_PARAM = str(param)
    with pytest.raises(ValueError):
        console.start("camera_set", {"name": "ClConfiguration", "value": 4})   # 相机的 EightyBit 是 3
    with pytest.raises(ValueError):
        console.start("camera_set", {"name": "UserSetSave", "value": 1})
    with pytest.raises(ValueError):
        console.start("camera_set", {"name": "TestPattern", "value": 4})          # 16K XML 没有 4
    with pytest.raises(ValueError):
        console.start("camera_set", {"name": "DigitalGain", "value": 16.5})
    with pytest.raises(ValueError):
        console.start("camera_set", {"name": "ExposureTime", "value": True})
    fake = tmp / "board" / "sg3000-capture"
    fake.write_text("#!/bin/bash\n[ \"$1\" = camera-set ] || exit 9\n"
                    "echo \"CameraSet $2 0x01501008: 3 -> $3, readback $3\"\n"
                    "echo 'CameraState ClConfiguration=0 (0 Base / 3 EightyBit) DeviceTapGeometry=3 PixelFormat=0x01080001 (not saved)'\n")
    fake.chmod(0o755)
    job = run(console, "camera_set", {"name": "ClConfiguration", "value": 0})
    assert job["status"] == "success" and "3 → 0" in job["message"] and "DeviceTapGeometry=3" in job["message"], job
    fake.write_text("#!/bin/bash\n[ \"$1 $2 $3\" = 'camera-set ExposureTime 80.5' ] || exit 9\n"
                    "echo 'CameraSet ExposureTime 0x01001010: 4800 -> 7728, readback 7728'\n"
                    "echo 'CameraValue ExposureTime = 80.50 us'\n")
    job = run(console, "camera_set", {"name": "ExposureTime", "value": 80.5})
    assert job["status"] == "success" and "80.50 us" in job["message"], job
    with pytest.raises(ValueError):
        console.start("camera_set", {"name": "LineRate", "value": 50})

    status = tmp / "board" / "sg_status"
    status.write_text("#!/bin/bash\necho 'DDR status      2'\necho 'pixel clocks    0x00000019'\n"
                      "echo 'CL config       4'\n"
                      "echo 'missing clocks  1 2   (EightyBit needs BOTH Camera Link cables: is the 2nd cable plugged in at camera and card?)'\n"
                      "echo 'Preflight FAILED: Link has been severed (-67)'\nexit 1\n")
    status.chmod(0o755)
    job = run(console, "status")
    assert job["status"] == "error" and "第二根 Camera Link 线" in job["message"], job
