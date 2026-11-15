"""模拟板子上的 sg3000-capture / sg_status（只用于电脑上的测试和界面演示，不上板）。

真的板上程序要有采集卡和相机才能跑。这里用一个状态文件假装相机和采集卡：
  camera-get / camera-set / card-set / capture / camera-probe 由本脚本回答，输出格式和真程序一样；
  其余子命令（pgm-stats、bench、version……）原样交给真程序（本仓库 x86 编译的同一份源码）。
capture 生成的图会随相机状态变：曝光、数字位移、测试图、平场校正开关；每采一帧"焦点"变一点
（先变清楚再变糊），用来演示对焦曲线。

用法：python sim_board_tool.py STATE_DIR REAL_APP 子命令 [参数...]
     python sim_board_tool.py STATE_DIR --status        （假 sg_status）
"""
import json
import os
import sys
from pathlib import Path

import numpy as np

WIDTH, HEIGHT, CLOCK = 16384, 200, 96
DEFAULT = {
    "regs": {"Width": 16384, "PixelFormat": 0x01080001, "ClConfiguration": 3, "DeviceTapGeometry": 10, "TestPattern": 0,
             "ExposureTime": 4800, "LineRate": 9600, "MeasuredLineRate": 9600, "LineTriggerMode": 0, "AnalogGain": 0,
             "DigitalGain": 256, "DigitalShift": 0, "BlackLevel": 0, "Binning": 1, "TDIStage": 0, "TDIMode": 0,
             "FFCEnable": 0, "FFCUserSetSelector": 0, "FFCSampleFrames": 1024, "FFCCalTargetMode": 0,
             "FFCCalTargetValue": 2880, "FFCFactory": 0, "ReverseX": 0, "Temperature": 2415},
    "card": {"CameraWidth": 4096, "CameraHeight": 2000, "ClConfiguration": 4, "DeviceTapGeometry": 10},
    "captures": 0, "ffc_calibrated": False,
}
ADDR = {"ClConfiguration": 0x01501008, "DeviceTapGeometry": 0x01501010, "TestPattern": 0x01100000, "ExposureTime": 0x01001010,
        "DigitalGain": 0x0120604c, "AnalogGain": 0x01001c00, "LineRate": 0x0100101c, "DigitalShift": 0x01207040,
        "TDIStage": 0x01106008, "TDIMode": 0x0110600c, "BlackLevel": 0x0120c050, "FFCEnable": 0x01102000,
        "FFCUserSetSelector": 0x01102d00, "FFCSampleFrames": 0x01102080, "FFCCalTargetMode": 0x01102c04,
        "FFCCalTargetValue": 0x01102c08, "FFCCalFPN": 0x01102084, "FFCCalPRNU": 0x01102088, "FFCUserSetLoad": 0x01102d04}
COMMANDS = ("FFCCalFPN", "FFCCalPRNU", "FFCUserSetLoad")
ALLOWED = {"ClConfiguration": (0, 3), "DeviceTapGeometry": (2, 3), "TestPattern": (0, 1, 2, 3, 5), "AnalogGain": range(5),
           "DigitalShift": range(5), "TDIStage": (0, 1), "TDIMode": (0, 1), "FFCEnable": (0, 1),
           "FFCUserSetSelector": range(4), "FFCSampleFrames": (1024, 2048), "FFCCalTargetMode": range(3)}


def load(state_dir):
    path = Path(state_dir) / "sim_state.json"
    return json.loads(path.read_text()) if path.exists() else json.loads(json.dumps(DEFAULT))


def save(state_dir, state):
    (Path(state_dir) / "sim_state.json").write_text(json.dumps(state))


def exposure_range(regs):
    return 192, regs["LineRate"] - 480          # 2 µs .. 一行少 5 µs


def print_params(regs):
    lo, hi = exposure_range(regs)
    for name, raw in regs.items():
        if name == "PixelFormat":
            print(f"CameraParam {name} raw=0x{raw:08x}")
        elif name == "ExposureTime":
            print(f"CameraParam {name} raw={raw} value={raw / CLOCK:.2f} min={lo / CLOCK:.2f} max={hi / CLOCK:.2f} unit=us")
        elif name == "LineRate":
            print(f"CameraParam {name} raw={raw} value={CLOCK * 1e6 / raw:.1f} min=100.0 max=15600.0 unit=Hz")
        elif name == "MeasuredLineRate":
            print(f"CameraParam {name} raw={raw} value={CLOCK * 1e6 / raw:.1f} unit=Hz")
        elif name == "DigitalGain":
            print(f"CameraParam {name} raw={raw} value={raw / 256:.3f} unit=x")
        elif name == "BlackLevel":
            print(f"CameraParam {name} raw={raw & 0xffffffff} value={raw} min=-64 max=63")
        elif name == "FFCCalTargetValue":
            print(f"CameraParam {name} raw={raw} value={raw // 16}")
        elif name == "Temperature":
            print(f"CameraParam {name} raw={raw} value={(raw - 2000) / 10:.1f} unit=C")
        elif name == "AnalogGain":
            print(f"CameraParam {name} raw={raw}{' locked=1' if regs['Binning'] in (1, 2) else ''}")
        elif name == "DeviceTapGeometry":
            print(f"CameraParam {name} raw={raw}{' locked=1' if regs['ClConfiguration'] != 0 else ''}")
        else:
            print(f"CameraParam {name} raw={raw}")


def refuse(name, text, why):
    print(f"Refused: {name} = {text} ({why}). Camera now: ...", file=sys.stderr)
    print("Failed: Operation not permitted (-1)", file=sys.stderr)
    return 1


def camera_set(state, args):
    name, text = args[0], args[1]
    regs = state["regs"]
    if name not in ADDR:
        return 2
    if name in COMMANDS:
        if text != "1":
            return refuse(name, text, "this is a command: the only value is 1")
        if name != "FFCUserSetLoad" and regs["FFCFactory"] == 0 and regs["FFCUserSetSelector"] == 0:
            return refuse(name, text, "FFC calibration is locked: set FFCUserSetSelector to 1..3 first (and ReverseX must be off)")
        state["ffc_calibrated"] = name != "FFCUserSetLoad" and (state["ffc_calibrated"] or name == "FFCCalPRNU")
        print(f"CameraCommand {name} 0x{ADDR[name]:08x}: sent, camera answered again after about 600 ms (write-only register, no readback)")
    else:
        old = regs[name]
        if name == "ExposureTime":
            raw = round(float(text) * CLOCK)
            lo, hi = exposure_range(regs)
            if not lo <= raw <= hi:
                return refuse(name, text, "ExposureTime outside the range the camera reports for the current line rate")
        elif name == "LineRate":
            raw = round(CLOCK * 1e6 / float(text))
            if not 6154 <= raw <= 960000:
                return refuse(name, text, "LineRate outside what the camera allows for the current width / CL mode")
            regs["MeasuredLineRate"] = raw
        elif name == "DigitalGain":
            raw = round(float(text) * 256)
        elif name == "FFCCalTargetValue":
            if regs["FFCCalTargetMode"] != 2:
                return refuse(name, text, "FFCCalTargetValue is locked unless FFCCalTargetMode is 2 (Target)")
            raw = int(text) * 16
        elif name == "BlackLevel":
            raw = int(text)
            if not -64 <= raw <= 63:
                return refuse(name, text, "BlackLevel outside the range the camera reports")
        else:
            raw = int(text)
            if raw not in ALLOWED[name]:
                return refuse(name, text, "value not allowed")
            if name == "AnalogGain" and regs["Binning"] in (1, 2):
                return refuse(name, text, "AnalogGain is locked by the camera while BinningV (0x01302040) is 1 or 2")
            if name == "DeviceTapGeometry" and regs["ClConfiguration"] != 0:
                return refuse(name, text, "DeviceTapGeometry is locked unless the camera is Base + Mono8 (set ClConfiguration 0 first)")
        regs[name] = raw
        if name == "ClConfiguration":
            regs["DeviceTapGeometry"] = 3 if raw == 0 else 10
        print(f"CameraSet {name} 0x{ADDR[name]:08x}: {old & 0xffffffff} -> {raw & 0xffffffff}, readback {raw & 0xffffffff}")
        if name == "ExposureTime":
            print(f"CameraValue ExposureTime = {raw / CLOCK:.2f} us")
        if name == "LineRate":
            print(f"CameraValue LineRate = {CLOCK * 1e6 / raw:.1f} Hz")
    print(f"CameraState ClConfiguration={regs['ClConfiguration']} (0 Base / 3 EightyBit) DeviceTapGeometry={regs['DeviceTapGeometry']} "
          f"PixelFormat=0x01080001 (not saved: camera power cycle restores UserSet1)")
    if "--params" in args:
        print_params(regs)
    return 0


def render(state):
    """按相机状态画一张图：中间一条黑胶带、全宽刻度线，边缘糊多少随采图次数变（模拟拧对焦环）。"""
    regs = state["regs"]
    x = np.arange(WIDTH)
    if regs["TestPattern"] == 3:
        return np.tile((x % 256).astype(np.uint8), (HEIGHT, 1))
    if regs["TestPattern"] in (1, 2):
        return np.full((HEIGHT, WIDTH), 0 if regs["TestPattern"] == 1 else 255, np.uint8)
    u = (x - WIDTH / 2) / (WIDTH / 2)
    light = 1.0 if regs["FFCEnable"] and state["ffc_calibrated"] else (1 - 0.3 * u ** 2) * (1 + 0.03 * np.sin(u * 11))
    target = np.ones(WIDTH)
    target[(x % 512) < 20] = 0.45                         # 刻度线
    target[(x >= 6000) & (x < 10000)] = 0.12              # 黑胶带
    n = state["captures"]
    sigma = 1.0 + 5.0 * abs(1 - min(n, 14) / 7)           # 第 7 帧最清楚
    k = np.arange(-30, 31)
    kernel = np.exp(-k ** 2 / (2 * sigma ** 2))
    line = np.convolve(np.pad(target, 30, mode="edge"), kernel / kernel.sum(), mode="valid")
    level = 70.0 * regs["ExposureTime"] / 4800 * (2 ** regs["DigitalShift"]) * regs["DigitalGain"] / 256 * (2 if regs["TDIStage"] else 1)
    ideal = level * light * line + 2
    rng = np.random.default_rng(n)
    image = ideal[None, :] + np.sqrt(np.maximum(ideal, 1) / 40 + 0.1)[None, :] * rng.standard_normal((HEIGHT, WIDTH))
    return np.clip(np.floor(image + 0.5), 0, 255).astype(np.uint8)


def capture(state, args):
    out = args[args.index("--output") + 1]
    if os.path.exists(out):
        print("Failed: File exists (-17)", file=sys.stderr)
        return 1
    if state["regs"]["ClConfiguration"] != 0 or state["card"]["CameraWidth"] != WIDTH:
        print("DDRInitialStatus=2, CameraLinkPixelClocks=0x00000001, CL=4, Color=0, Depth=8")
        print("Failed: Link has been severed (-67)", file=sys.stderr)
        return 1
    image = render(state)
    state["captures"] += 1
    with open(out, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (WIDTH, HEIGHT) + image.tobytes())
    print("DDRInitialStatus=2, CameraLinkPixelClocks=0x00000059, CL=0, Color=0, Depth=8")
    print(f"Saved complete Mono8 frame: {WIDTH} x {HEIGHT}, {image.size} bytes")
    return 0


def status(state):
    regs, card = state["regs"], state["card"]
    ok = regs["ClConfiguration"] == 0                # 只接了一根线：相机还在 EightyBit 时第 2、3 路时钟没有
    card["ClConfiguration"] = 0 if ok else 4
    print("max_transfer    1048576 bytes\npoisoned        0   (non-zero = driver has isolated the card)")
    print(f"DDR status      2   (0 = uninitialized, 2 = calibrated; both normal)\npixel clocks    0x{0x59 if ok else 0x1:08x}   bits set: 0")
    print(f"CL config       {card['ClConfiguration']}   (0 = Base, 1 cable / 4 = EightyBit, 2 cables / 1, 2 unsupported)")
    print("Preflight passed." if ok else "missing clocks  1 2   (EightyBit needs BOTH Camera Link cables)\nPreflight FAILED: Link has been severed (-67)")
    print("SG_HDR_READY    0   (non-zero = a data block is waiting)")
    for name, value in card.items():
        print(f"CardParam {name:<18} 0x10203008 = {value} (0x{value:x})")
    print(f"CardParam LineRateHz          {CLOCK * 1e6 / regs['LineRate']:.1f}   (100 MHz / (LinePeriod10ns + 1); 0 = no lines)")
    print("CardParam CameraClockMHz      85.00   (formula from card XML)")
    return 0 if ok else 1


def main(argv):
    state_dir = argv[1]
    state = load(state_dir)
    if argv[2] == "--status":
        rc = status(state)
        save(state_dir, state)
        return rc
    real, command, args = argv[2], argv[3] if len(argv) > 3 else "", argv[4:]
    if command == "camera-get":
        print_params(state["regs"])
        print("CameraGet done (read only)")
        rc = 0
    elif command == "camera-set":
        rc = camera_set(state, args)
    elif command == "camera-probe":
        print("CameraVendor=JAI\nCameraModel=SW-WS2-CL16K-50M-SDE\nCameraManifest[0] version 0.0.3 zip 18279 bytes")
        rc = 0
    elif command == "card-set":
        old = state["card"].get(args[0], 0)
        state["card"][args[0]] = int(args[1])
        print(f"CardSet {args[0]} 0x10203008: {old} -> {args[1]}, readback {args[1]}")
        rc = 0
    elif command == "capture":
        rc = capture(state, args)
    else:
        os.execv(real, [real, *argv[3:]])
    save(state_dir, state)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
