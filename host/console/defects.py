"""Offline, local-only bridge to the bundled, unchanged native line-scan demo.

No camera, board, arbitrary input path, executable, or calibration is accepted.
The JSONL is the full result store; pagination never truncates that store.
"""
from __future__ import annotations

import copy
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

import numpy as np


RUN_ID = re.compile(r"(?:run-[0-9a-f]{32}|replay-16k)\Z")
MAX_PIXELS = 8 * 1024 * 1024
MAX_HEADER = 4096
MAX_LINE = 65536


def integer(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} 必须是 {low}..{high} 的整数。")
    return value


def _json(path, maximum=65536):
    with path.open("rb") as file:
        raw = file.read(maximum + 1)
    if len(raw) > maximum:
        raise ValueError("JSON 元数据过大。")
    return json.loads(raw)


def _pgm(path, expected=None):
    """Strict P5 parser: bounded header, exact raster length, no trailing bytes."""
    with path.open("rb") as file:
        head = file.read(MAX_HEADER)
        pos, tokens = 0, []
        for _ in range(4):
            while pos < len(head):
                if head[pos] in b" \t\r\n":
                    pos += 1
                elif head[pos] == 35:
                    end = head.find(b"\n", pos)
                    if end < 0:
                        raise ValueError("PGM 注释不完整。")
                    pos = end + 1
                else:
                    break
            start = pos
            while pos < len(head) and head[pos] not in b" \t\r\n#":
                pos += 1
            if pos == start or pos == len(head):
                raise ValueError("PGM 文件头无效或过大。")
            tokens.append(head[start:pos])
        if tokens[0] != b"P5" or tokens[3] != b"255" or any(not t.isdigit() for t in tokens[1:3]):
            raise ValueError("仅接受 P5 Mono8 图像。")
        width, height = map(int, tokens[1:3])
        integer(width, "width", 1, 16384)
        integer(height, "height", 1, 65536)
        if width * height > MAX_PIXELS:
            raise ValueError("离线演示图像过大。")
        if expected is not None and (width, height) != expected:
            raise ValueError("图像尺寸与运行元数据不一致。")
        if head[pos] not in b" \t\r\n":
            raise ValueError("PGM 缺少像素分隔符。")
        pos += 2 if head[pos:pos + 2] == b"\r\n" else 1
        file.seek(0, 2)
        if file.tell() != pos + width * height:
            raise ValueError("PGM 像素不完整或带有尾随数据。")
        file.seek(pos)
        data = file.read(width * height + 1)
        if len(data) != width * height:
            raise ValueError("PGM 在读取时发生变化。")
    return np.frombuffer(data, dtype=np.uint8).reshape(height, width)


def _component(item, index, width, height):
    if not isinstance(item, dict):
        raise ValueError("候选必须是对象。")
    bbox = item.get("bbox")
    if not isinstance(bbox, list) or len(bbox) != 4:
        raise ValueError("候选 bbox 必须有四个坐标。")
    x0, y0, x1, y1 = [integer(n, "bbox", 0, max(width, height)) for n in bbox]
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        raise ValueError("候选 bbox 超出图像或不是半开区间。")
    polarity = item.get("polarity")
    if polarity not in ("bright", "dark"):
        raise ValueError("候选极性无效。")
    area = integer(item.get("area"), "area", 1, (x1 - x0) * (y1 - y0))
    peak = integer(item.get("peak"), "peak", 1, 255)
    frame = integer(item.get("frame_id"), "frame_id", 0, (1 << 64) - 1)
    return dict(id=f"c{index + 1:06d}", bbox=bbox, polarity=polarity, area=area,
                peak=peak, signed_peak=peak if polarity == "bright" else -peak,
                frame_id=str(frame) if frame > (1 << 53) - 1 else frame,
                decision="unknown")


def _components(path, width, height):
    with path.open("rb") as file:
        index = 0
        while True:
            raw = file.readline(MAX_LINE + 1)
            if not raw:
                break
            if len(raw) > MAX_LINE or not raw.endswith(b"\n"):
                raise ValueError("候选记录过大或不完整。")
            yield _component(json.loads(raw), index, width, height)
            index += 1


def _preview_shape(width, height):
    step = max(1, math.ceil(width / 2048))
    return math.ceil(width / step), math.ceil(height / step)


def _png(pixels):
    height, width = pixels.shape
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    scanlines = b"".join(b"\0" + row.tobytes() for row in pixels)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


class DefectService:
    """One serialized native job; completed runs survive application restarts."""

    timeout_seconds = 60

    def __init__(self, bundle_root, output_root):
        self.bundle_root = Path(bundle_root).resolve()
        self.output_root = Path(output_root).resolve()
        expected = self.bundle_root / "SG3000Workbench" / "runtime" / "defects"
        if self.output_root != expected.resolve():
            raise ValueError("离线输出目录必须为随包 SG3000Workbench/runtime/defects。")
        self.output_root.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._runs = {}
        self._current = None
        self._busy = False
        self._restore()

    def _folder(self, run):
        folder = (self.bundle_root / "sample_output_16k" if run["provenance"] == "recorded-replay"
                  else self.output_root / run["id"])
        if folder.resolve() != folder:
            raise ValueError("运行目录不能使用符号链接跳转。")
        return folder

    def _save(self, run):
        path = self.output_root / (run["id"] + ".json")
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(run, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(path)

    def _restore(self):
        for path in self.output_root.glob("*.json"):
            if not RUN_ID.fullmatch(path.stem):
                continue
            try:
                if path.resolve().parent != self.output_root:
                    raise ValueError("运行索引不能指向输出目录外。")
                run = _json(path)
                if not isinstance(run, dict) or run.get("id") != path.stem:
                    raise ValueError("运行索引不匹配。")
                provenance = "recorded-replay" if path.stem == "replay-16k" else "synthetic-native"
                if run.get("provenance") != provenance:
                    raise ValueError("运行来源不匹配。")
                run["created_at"] = float(run["created_at"])
                if not math.isfinite(run["created_at"]):
                    raise ValueError("运行时间无效。")
                if run.get("status") == "complete":
                    run = self._validate(run)
                else:
                    run.update(status="failed", error=run.get("error") or "服务重启，前次任务未完成。")
            except (OSError, ValueError, TypeError, KeyError) as error:
                run = dict(id=path.stem, status="failed", provenance="recorded-replay" if path.stem == "replay-16k" else "synthetic-native",
                           created_at=path.stat().st_mtime, decision="unknown", board_tested=False,
                           accuracy_evaluated=False, error=f"历史结果验证失败：{error}")
            self._runs[run["id"]] = run
        if self._runs:
            self._current = max(self._runs, key=lambda key: self._runs[key]["created_at"])

    def _validate(self, run):
        for key in ("created_at", "finished_at"):
            if key in run and (type(run[key]) not in (int, float) or not math.isfinite(run[key]) or run[key] < 0):
                raise ValueError("运行时间无效。")
        folder = self._folder(run)
        # Fixed files only; resolved children may never escape their recorded run.
        for name in ("run.json", "corrected.pgm", "mask.pgm", "components.jsonl"):
            if (folder / name).resolve().parent != folder.resolve():
                raise ValueError("结果文件不能指向运行目录外。")
        meta = _json(folder / "run.json")
        if not isinstance(meta, dict) or meta.get("status") != "complete":
            raise ValueError("原生检测尚未完整完成。")
        if meta.get("source") != "synthetic" or meta.get("board_tested") is not False or meta.get("accuracy_evaluated") is not False:
            raise ValueError("离线来源或验证边界不一致。")
        if meta.get("implementation") != "native C++ shared with FPGA IP":
            raise ValueError("原始算法实现声明不一致。")
        width = integer(meta.get("width"), "width", 1, 16384)
        height = integer(meta.get("height"), "height", 1, 65536)
        if (width, height) != (16384, 135):
            raise ValueError("本入口只接受固定 16K 合成演示。")
        opts = self._options({key: meta.get(key) for key in ("radius", "threshold", "polarity")})
        if run.get("options") is not None and opts != run["options"]:
            raise ValueError("实际检测参数与请求不匹配。")
        _pgm(folder / "corrected.pgm", (width, height))
        mask = _pgm(folder / "mask.pgm", (width, height))
        if np.any((mask != 0) & (mask != 255)):
            raise ValueError("掩膜包含非二值像素。")
        count = 0
        for item in _components(folder / "components.jsonl", width, height):
            if item["peak"] < opts["threshold"] or item["area"] < 2:
                raise ValueError("候选响应或面积低于本次筛选条件。")
            if opts["polarity"] != "both" and item["polarity"] != opts["polarity"]:
                raise ValueError("候选极性与本次筛选条件不一致。")
            count += 1
        if integer(meta.get("components"), "components", 0, width * height) != count:
            raise ValueError("候选数量与完整记录不一致。")
        preview_width, preview_height = _preview_shape(width, height)
        image_meta = dict(source_width=width, source_height=height, preview_width=preview_width, preview_height=preview_height)
        result = copy.deepcopy(run)
        result.update(status="complete", source="synthetic", width=width, height=height,
                      **opts, options=opts, component_count=count, decision="unknown", board_tested=False,
                      accuracy_evaluated=False, bbox_convention="half-open", connectivity=8,
                      implementation="native C++ shared with FPGA IP", min_area=2,
                      images={"corrected": image_meta, "mask": image_meta.copy()})
        wall = meta.get("native_wall_seconds")
        if type(wall) not in (int, float) or not math.isfinite(wall) or wall < 0:
            raise ValueError("原生执行耗时无效。")
        result["native_wall_seconds"] = wall
        result["stripes"] = integer(meta.get("stripes"), "stripes", 1, height)
        result["retained_input_cache_peak_bytes"] = integer(meta.get("retained_input_cache_peak_bytes"), "cache", 0, MAX_PIXELS)
        digest = meta.get("output_words_sha256")
        if not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest):
            raise ValueError("原生输出摘要无效。")
        result["output_words_sha256"] = digest
        result.pop("error", None)
        return result

    @staticmethod
    def _options(options):
        if options is None:
            options = {}
        if not isinstance(options, dict) or set(options) - {"radius", "threshold", "polarity"}:
            raise ValueError("仅接受 radius、threshold、polarity 参数。")
        radius = integer(options.get("radius", 7), "radius", 0, 7)
        threshold = integer(options.get("threshold", 25), "threshold", 1, 255)
        polarity = options.get("polarity", "both")
        if not isinstance(polarity, str) or polarity not in ("both", "bright", "dark"):
            raise ValueError("polarity 必须是 both、bright 或 dark。")
        return dict(radius=radius, threshold=threshold, polarity=polarity)

    def start(self, options=None):
        options = self._options(options)
        with self._lock:
            if self._busy:
                raise ValueError("离线检测任务进行中，请等待完成。")
            run = dict(id="run-" + uuid.uuid4().hex, status="running", created_at=time.time(), options=options,
                       provenance="synthetic-native", decision="unknown", board_tested=False, accuracy_evaluated=False)
            self._save(run)
            self._runs[run["id"]] = run
            self._current, self._busy = run["id"], True
            thread = threading.Thread(target=self._execute, args=(copy.deepcopy(run),), daemon=True)
            try:
                thread.start()
            except BaseException as error:
                self._busy = False
                run.update(status="failed", error=str(error))
                self._save(run)
                raise
            return dict(id=run["id"], status="running")

    def _execute(self, run):
        try:
            module = self.bundle_root / "linescan_preprocess_v2"
            script = module / "examples" / "run_linescan.py"
            library = module / "prebuilt" / "windows-x64" / "linescan_preprocess.dll"
            if not script.is_file() or not library.is_file():
                raise ValueError("随包原生演示程序或 DLL 缺失。")
            options = run["options"]
            command = [sys.executable, "-u", str(script), "--native-library", str(library),
                       "--output-dir", str(self._folder(run)), "--radius", str(options["radius"]),
                       "--threshold", str(options["threshold"]), "--polarity", options["polarity"]]
            log_path = self.output_root / (run["id"] + ".log")
            with log_path.open("wb") as log:
                completed = subprocess.run(command, cwd=str(module), stdin=subprocess.DEVNULL,
                                           stdout=log, stderr=subprocess.STDOUT, timeout=self.timeout_seconds,
                                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), check=False)
            if completed.returncode:
                with log_path.open("rb") as log:
                    log.seek(max(0, log_path.stat().st_size - 4096))
                    detail = log.read(4096).decode("utf-8", errors="replace")
                raise RuntimeError(f"原生程序退出码 {completed.returncode}：{detail}")
            run = self._validate(run)
        except Exception as error:
            run.update(status="failed", error=f"{type(error).__name__}: {error}", finished_at=time.time())
        else:
            run["finished_at"] = time.time()
        finally:
            with self._lock:
                try:
                    self._save(run)
                except OSError as error:
                    run.update(status="failed", error=f"结果索引保存失败：{error}")
                self._runs[run["id"]] = run
                self._busy = False

    def replay(self):
        with self._lock:
            if self._busy:
                raise ValueError("离线检测任务进行中，请等待完成。")
            run = dict(id="replay-16k", status="running", created_at=time.time(), provenance="recorded-replay",
                       decision="unknown", board_tested=False, accuracy_evaluated=False)
            try:
                run = self._validate(run)
            except Exception as error:
                run.update(status="failed", error=f"随包回放验证失败：{error}")
                self._runs[run["id"]] = run
                self._current = run["id"]
                self._save(run)
                raise ValueError(run["error"]) from error
            run["finished_at"] = time.time()
            self._save(run)
            self._runs[run["id"]], self._current = run, run["id"]
            return dict(id=run["id"], status="complete")

    def state(self):
        with self._lock:
            return copy.deepcopy(dict(busy=self._busy,
                                      current=self._runs.get(self._current),
                                      runs=sorted(self._runs.values(), key=lambda run: run["created_at"], reverse=True)))

    def _complete(self, run_id):
        if not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id):
            raise ValueError("运行编号无效。")
        with self._lock:
            if run_id not in self._runs:
                raise KeyError("运行不存在。")
            run = copy.deepcopy(self._runs[run_id])
        if run["status"] != "complete":
            raise RuntimeError("运行尚未成功完成：" + run.get("error", run["status"]))
        try:
            return self._validate(run)
        except Exception as error:
            with self._lock:
                run.update(status="failed", error=f"结果验证失败：{error}")
                self._runs[run_id] = run
                self._save(run)
            raise ValueError(run["error"]) from error

    def results(self, run_id, offset=0, limit=100):
        integer(offset, "offset", 0, (1 << 53) - 1)
        integer(limit, "limit", 1, 1000)
        run = self._complete(run_id)
        items = []
        for index, item in enumerate(_components(self._folder(run) / "components.jsonl", run["width"], run["height"])):
            if index >= offset + limit:
                break
            if index >= offset:
                items.append(item)
        return dict(schema_version="defects-v1", run=run,
                    pagination=dict(offset=offset, limit=limit, total=run["component_count"], returned=len(items),
                                    has_more=offset + len(items) < run["component_count"]), components=items)

    def image(self, run_id, kind="corrected", x=0, y=0, width=None, height=None):
        if not isinstance(kind, str) or kind not in ("corrected", "mask"):
            raise ValueError("图像类型必须为 corrected 或 mask。")
        run = self._complete(run_id)
        integer(x, "x", 0, run["width"] - 1)
        integer(y, "y", 0, run["height"] - 1)
        pixels = _pgm(self._folder(run) / (kind + ".pgm"), (run["width"], run["height"]))
        if width is None and height is None:
            if x or y:
                raise ValueError("裁剪须同时提供 width 和 height。")
            step = max(1, math.ceil(run["width"] / 2048))
            if kind == "mask":
                # Padded max pooling preserves a one-pixel hit in every overview cell.
                pad_y, pad_x = (-pixels.shape[0]) % step, (-pixels.shape[1]) % step
                padded = np.pad(pixels, ((0, pad_y), (0, pad_x)), constant_values=0)
                pixels = padded.reshape(padded.shape[0] // step, step, padded.shape[1] // step, step).max(axis=(1, 3))
            else:
                pixels = pixels[::step, ::step]
        else:
            integer(width, "width", 1, 1024)
            integer(height, "height", 1, 512)
            if x + width > run["width"] or y + height > run["height"]:
                raise ValueError("裁剪范围超出源图像。")
            pixels = pixels[y:y + height, x:x + width]
        return _png(pixels)
