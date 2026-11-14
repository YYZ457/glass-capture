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
