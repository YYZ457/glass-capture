"""生成拷到 SD 卡上的 glass-capture/ 目录。

内容：sg3000-capture、sg3000-stream、sg_status（ARM64 静态链接）、SHA256SUMS、BUILD-INFO.txt，
可选 sg3000_rx.ko（--ko 指定；不给的话中控台会用 SD 卡旧目录 sg3000-linux-20261023 里的驱动）。

用法：
  python tools/make_sd_bundle.py                        # 交叉编译（需要 aarch64-linux-gnu-gcc）→ dist/glass-capture/
  python tools/make_sd_bundle.py --bin-dir DIR          # 不编译，直接用 DIR 里现成的三个程序
  python tools/make_sd_bundle.py --ko path/sg3000_rx.ko # 顺便带上驱动
  python tools/make_sd_bundle.py --bin-dir dist/glass-capture --ko path/sg3000_rx.ko
                                                        # 只补/换驱动，程序原样保留（不重编）
不给 --ko 时，dist/glass-capture/ 里已有的 sg3000_rx.ko 会原样保留（它进 git，见 .gitignore）。
然后把 dist/glass-capture 整个文件夹拷到 SD 卡第一个分区的根目录。
"""
import argparse
import datetime
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PROGRAMS = ("sg3000-capture", "sg3000-stream", "sg_status")
SCRIPTS = {"board_session.sh": REPO / "board" / "scripts" / "board_session.sh"}   # 总是取仓库里的版本
CROSS = "aarch64-linux-gnu-"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def build(out_dir):
    run(["make", "-C", str(REPO / "board"), f"CROSS_COMPILE={CROSS}", f"OUT={out_dir}", "all"])
    for name in PROGRAMS:
        run([CROSS + "strip", str(Path(out_dir) / name)])
    return Path(out_dir)


def git_describe():
    try:
        rev = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain", "board", "include"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return rev + ("+uncommitted" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin-dir", type=Path, help="已编译好的程序目录（不再编译）")
    parser.add_argument("--ko", type=Path, help="sg3000_rx.ko，可选")
    parser.add_argument("--out", type=Path, default=REPO / "dist" / "glass-capture")
    args = parser.parse_args(argv)

    bin_dir = args.bin_dir or build(REPO / "board" / "out-arm64")
    # 先把要用的文件全部读进内存：--bin-dir 可以就是输出目录本身，删目录前不能丢
    blobs = {name: (bin_dir / name).read_bytes() for name in PROGRAMS}
    for name, src in SCRIPTS.items():
        # 写成 Unix 换行：Windows 检出可能带 \r\n，板上 sh 不认
        blobs[name] = src.read_bytes().replace(b"\r\n", b"\n")
    ko = args.ko if args.ko else args.out / "sg3000_rx.ko"
    if ko.exists():
        blobs["sg3000_rx.ko"] = ko.read_bytes()
    elif args.ko:
        raise SystemExit(f"--ko {args.ko} does not exist")
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    files = list(blobs)
    for name, data in blobs.items():
        (args.out / name).write_bytes(data)
        if name in PROGRAMS or name in SCRIPTS:
            (args.out / name).chmod(0o755)

    sums = []
    for name in files:
        digest = hashlib.sha256((args.out / name).read_bytes()).hexdigest()
        sums.append(f"{digest}  {name}\n")
    # 写成 Unix 换行：板上 sha256sum -c 不认 \r\n
    (args.out / "SHA256SUMS").write_bytes("".join(sums).encode("ascii"))
    (args.out / "BUILD-INFO.txt").write_bytes(
        (f"glass-capture SD bundle\nsource: {git_describe()}\n"
         f"built: {datetime.datetime.now().isoformat(timespec='seconds')}\n"
         f"files: {', '.join(files)}\n"
         "copy this whole folder to the root of the SD card's first (FAT) partition.\n").encode("ascii"))
    print(f"\nbundle ready: {args.out}")
    for line in sums:
        print("  " + line.rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
