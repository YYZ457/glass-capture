"""Uploaded P5 -> unchanged native pipeline. No device or arbitrary-path API.

Input names are display text only. Only validated, committed artifacts become
readable. Candidate JSONL is complete; the HTTP adapter owns request framing.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import uuid

import numpy as np

from defects import _components, _json, _pgm, _png, integer, MAX_PIXELS, MAX_HEADER


INPUT_ID = re.compile(r"input-[0-9a-f]{32}\Z")
RUN_ID = re.compile(r"frame-[0-9a-f]{32}\Z")
MAX_UPLOAD = MAX_PIXELS + MAX_HEADER
MAX_JSONL_BYTES = 4 * 1024 * 1024
MAX_EXPORT_BYTES = 8 * 1024 * 1024
FIXED = dict(source="uploaded-pgm", provenance="file-native", decision="unknown",
             board_tested=False, accuracy_evaluated=False)
OUTPUT_NAMES = ("run.json", "corrected.pgm", "mask.pgm", "components.jsonl")


def _sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for data in iter(lambda: file.read(65536), b""):
            digest.update(data)
    return digest.hexdigest()


def _preview(width, height):
    step = max(1, math.ceil(width / 2048), math.ceil(height / 1024))
    return dict(source_width=width, source_height=height,
                preview_width=math.ceil(width / step), preview_height=math.ceil(height / step),
                downsample_step=step)


class FrameService:
    """One bounded subprocess at a time; validated history survives restart."""

    timeout_seconds = 120

    def __init__(self, bundle_root, output_root=None):
        self.bundle_root = Path(bundle_root).resolve()
        expected = self.bundle_root / "host" / "runtime" / "frames"   # bundle_root = 仓库根目录
        self.output_root = Path(output_root).resolve() if output_root is not None else expected.resolve()
        if self.output_root != expected.resolve() or expected.resolve() != expected:
            raise ValueError("真实图像离线输出必须位于随包 runtime/frames，不能链接到外部。")
        self.output_root.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._runs, self._inputs = {}, {}
        self._current, self._busy = None, False
        self._restore()

    def _path(self, identifier, name=None, *, input_file=False):
        pattern = INPUT_ID if input_file else RUN_ID
        if not isinstance(identifier, str) or not pattern.fullmatch(identifier):
            raise ValueError("输入或运行编号无效。")
        folder = self.output_root / identifier
        path = folder / name if name else folder
        if folder.resolve() != folder or path.resolve() != path:
            raise ValueError("运行产物不能使用链接跳转。")
        return path

    def _save(self, record):
        identifier = record.get("id", record.get("input_id"))
        self._path(identifier, input_file="input_id" in record and "id" not in record)
        path = self.output_root / (identifier + ".json")
        if path.resolve() != path:
            raise ValueError("索引不能使用链接跳转。")
        temporary = self.output_root / (identifier + ".tmp")
        if temporary.resolve() != temporary:
            raise ValueError("索引临时文件不能使用链接跳转。")
        temporary.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(path)

    def register_pgm(self, stream, bounded_length, display_name="image.pgm"):
        """Consume exactly declared bytes, tolerating fragmented stream reads.

        Never read beyond bounded_length: a live HTTP stream may contain the next
        request. Trailing raster bytes *inside* that body are rejected by _pgm.
        """
        integer(bounded_length, "上传字节数", 1, MAX_UPLOAD)
        if not isinstance(display_name, str) or not 1 <= len(display_name) <= 256:
            raise ValueError("显示文件名必须为 1..256 字符。")
        display_name = display_name.replace("\\", "/").rsplit("/", 1)[-1]
        if not display_name or any(ord(c) < 32 for c in display_name):
            raise ValueError("显示文件名无效。")
        identifier = "input-" + uuid.uuid4().hex
        folder = self._path(identifier, input_file=True)
        folder.mkdir(exist_ok=False)
        temporary, target = folder / "source.partial", folder / "source.pgm"
        try:
            remaining = bounded_length
            with temporary.open("xb") as output:
                while remaining:
                    block = stream.read(min(65536, remaining))
                    if not isinstance(block, bytes) or not block or len(block) > remaining:
                        raise ValueError("上传正文不完整或读取长度违反合同。")
                    output.write(block)
                    remaining -= len(block)
            pixels = _pgm(temporary)
            height, width = pixels.shape
            record = dict(input_id=identifier, width=width, height=height,
                          display_name=display_name, bytes=bounded_length,
                          sha256=_sha(temporary), created_at=time.time(), source="uploaded-pgm")
            temporary.replace(target)
            with self._lock:
                self._save(record)
                self._inputs[identifier] = record
            return dict(input_id=identifier, width=width, height=height)
        except BaseException:
            for path in (temporary, target):
                path.unlink(missing_ok=True)
            folder.rmdir()
            raise

    def _input(self, identifier):
        path = self._path(identifier, "source.pgm", input_file=True)
        if identifier not in self._inputs:
            raise KeyError("上传图像不存在。")
        record = self._inputs[identifier]
        if not isinstance(record, dict) or record.get("input_id") != identifier or record.get("source") != "uploaded-pgm":
            raise ValueError("输入登记元数据不一致。")
        integer(record.get("width"), "width", 1, 16384)
        integer(record.get("height"), "height", 1, 65536)
        integer(record.get("bytes"), "bytes", 1, MAX_UPLOAD)
        name, created, digest = record.get("display_name"), record.get("created_at"), record.get("sha256")
        if (not isinstance(name, str) or not 1 <= len(name) <= 256 or "/" in name or "\\" in name
                or any(ord(c) < 32 for c in name)
                or type(created) not in (int, float) or not math.isfinite(created) or created < 0
                or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)):
            raise ValueError("输入文件名、时间或摘要元数据无效。")
        pixels = _pgm(path, (record["width"], record["height"]))
        if path.stat().st_size != record["bytes"] or _sha(path) != record["sha256"]:
            raise ValueError("已登记源图像发生变化。")
        return copy.deepcopy(record), path, pixels

    @staticmethod
    def _options(options):
        if not isinstance(options, dict) or set(options) - {"input_id", "radius", "threshold", "polarity"}:
            raise ValueError("仅接受 input_id、radius、threshold、polarity。")
        identifier = options.get("input_id")
        if not isinstance(identifier, str) or not INPUT_ID.fullmatch(identifier):
            raise ValueError("input_id 无效。")
        radius = integer(options.get("radius", 7), "radius", 0, 7)
        threshold = integer(options.get("threshold", 25), "threshold", 1, 255)
        polarity = options.get("polarity", "both")
        if not isinstance(polarity, str) or polarity not in ("both", "bright", "dark"):
            raise ValueError("polarity 必须为 both、bright 或 dark。")
        return dict(input_id=identifier, radius=radius, threshold=threshold, polarity=polarity)

    def start(self, options):
        options = self._options(options)
        with self._lock:
            if self._busy:
                raise ValueError("真实图像检测任务进行中。")
            record, _, _ = self._input(options["input_id"])
            run = dict(id="frame-" + uuid.uuid4().hex, status="running", created_at=time.time(),
                       options=options, input_id=record["input_id"], input_sha256=record["sha256"],
                       display_name=record["display_name"], **FIXED)
            self._save(run)
            self._runs[run["id"]] = run
            self._current, self._busy = run["id"], True
            thread = threading.Thread(target=self._execute, args=(copy.deepcopy(run),), daemon=True)
            try:
                thread.start()
            except BaseException as error:
                self._busy = False
                run.update(status="failed", error=str(error), finished_at=time.time())
                self._save(run)
                raise
            return dict(id=run["id"], status="running")

    def _execute(self, run):
        try:
            module = self.bundle_root / "fpga" / "linescan_preprocess_v2"
            script = Path(__file__).with_name("process_frame.py")
            library = module / "prebuilt" / "windows-x64" / "linescan_preprocess.dll"
            if not script.is_file() or not library.is_file():
                raise ValueError("缺少预编译库 fpga/linescan_preprocess_v2/prebuilt/windows-x64/linescan_preprocess.dll（不在仓库里，在原始 zip 包中）。可改用 host/detect/candidates.py。")
            _, source, _ = self._input(run["input_id"])
            options = run["options"]
            command = [sys.executable, "-u", str(script), "--module-root", str(module), "--input", str(source),
                       "--native-library", str(library), "--output-dir", str(self._path(run["id"])),
                       "--radius", str(options["radius"]), "--threshold", str(options["threshold"]),
                       "--polarity", options["polarity"]]
            log_path = self.output_root / (run["id"] + ".log")
            if log_path.resolve() != log_path:
                raise ValueError("日志不能使用链接跳转。")
            with log_path.open("xb") as log:
                completed = subprocess.run(command, cwd=module, stdin=subprocess.DEVNULL, stdout=log,
                                           stderr=subprocess.STDOUT, timeout=self.timeout_seconds,
                                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), check=False)
            if completed.returncode:
                metadata_path = self._path(run["id"], "run.json")
                if metadata_path.is_file():
                    failure = _json(metadata_path)
                    if isinstance(failure, dict) and failure.get("status") == "failed" and isinstance(failure.get("error"), str):
                        raise RuntimeError("原生任务失败：" + failure["error"][:512])
                raise RuntimeError(f"原生程序退出码 {completed.returncode}；详见本机运行日志。")
            run = self._validate(run)
        except Exception as error:
            run.update(status="failed", error=f"{type(error).__name__}: {error}")
        finally:
            run["finished_at"] = time.time()
            with self._lock:
                try:
                    self._save(run)
                except (OSError, ValueError) as error:
                    run.update(status="failed", error=f"索引保存失败：{error}")
                self._runs[run["id"]] = run
                self._busy = False

    def _validate(self, run):
        if not isinstance(run, dict):
            raise ValueError("运行元数据必须为对象。")
        if any(run.get(k) != value for k, value in FIXED.items()):
            raise ValueError("来源或验证边界不一致。")
        options = self._options(run.get("options"))
        if options["input_id"] != run.get("input_id"):
            raise ValueError("输入编号不一致。")
        source, source_path, _ = self._input(run["input_id"])
        if source["sha256"] != run.get("input_sha256") or source["display_name"] != run.get("display_name"):
            raise ValueError("运行输入摘要或显示文件名不一致。")
        width, height = source["width"], source["height"]
        paths = {name: self._path(run["id"], name) for name in OUTPUT_NAMES}
        for name, path in paths.items():
            maximum = 65536 if name == "run.json" else MAX_JSONL_BYTES if name == "components.jsonl" else MAX_UPLOAD
            if path.stat().st_size > maximum:
                raise ValueError("运行产物超过文件大小上限；请减小图像尺寸或提高阈值。")
        meta = _json(paths["run.json"])
        if (not isinstance(meta, dict) or meta.get("status") != "complete" or meta.get("source") != str(source_path)
                or (meta.get("width"), meta.get("height")) != (width, height)
                or meta.get("board_tested") is not False or meta.get("accuracy_evaluated") is not False
                or meta.get("implementation") != "native C++ shared with FPGA IP"):
            raise ValueError("原生输出未完整完成或元数据不匹配。")
        for key in ("radius", "threshold", "polarity"):
            if meta.get(key) != options[key] or type(meta.get(key)) is not type(options[key]):
                raise ValueError("原生输出参数不匹配。")
        _pgm(paths["corrected.pgm"], (width, height))
        mask = _pgm(paths["mask.pgm"], (width, height))
        if np.any((mask != 0) & (mask != 255)):
            raise ValueError("掩膜不是二值图像。")
        count = 0
        for item in _components(paths["components.jsonl"], width, height):
            if (item["area"] < 2 or item["peak"] < options["threshold"] or item["frame_id"] != 0
                    or (options["polarity"] != "both" and item["polarity"] != options["polarity"])):
                raise ValueError("候选不符合本次原生筛选条件。")
            count += 1
            if count > width * height // 2:
                raise ValueError("候选总数超过 min_area=2 的帧像素上界。")
        if integer(meta.get("components"), "components", 0, width * height) != count:
            raise ValueError("完整候选数量不匹配。")
        wall = meta.get("native_wall_seconds")
        if type(wall) not in (int, float) or not math.isfinite(wall) or wall < 0:
            raise ValueError("原生处理耗时无效。")
        digest = meta.get("output_words_sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("输出字摘要无效。")
        hashes = {name: _sha(path) for name, path in paths.items()}
        if "output_sha256" in run and run["output_sha256"] != hashes:
            raise ValueError("已完成输出发生变化。")
        for key in ("created_at", "finished_at"):
            if key not in run and (key == "created_at" or run.get("status") == "complete"):
                raise ValueError("运行时间字段缺失。")
            if key in run and (type(run[key]) not in (int, float) or not math.isfinite(run[key]) or run[key] < 0):
                raise ValueError("运行时间无效。")
        result = {key: copy.deepcopy(run[key]) for key in
                  ("id", "created_at", "finished_at", "options", "input_id", "input_sha256", "display_name") if key in run}
        result.update(FIXED)
        result.update(status="complete", width=width, height=height, component_count=count,
                      radius=options["radius"], threshold=options["threshold"], polarity=options["polarity"],
                      bbox_convention="half-open", connectivity=8, min_area=2,
                      implementation=meta["implementation"], native_wall_seconds=wall,
                      stripes=integer(meta.get("stripes"), "stripes", 1, height),
                      retained_input_cache_peak_bytes=integer(meta.get("retained_input_cache_peak_bytes"), "cache", 0, MAX_PIXELS),
                      output_words_sha256=digest, output_sha256=hashes,
                      images={name: _preview(width, height) for name in ("source", "corrected", "mask")})
        result.pop("error", None)
        return result

    def _restore(self):
        for path in self.output_root.glob("input-*.json"):
            if not INPUT_ID.fullmatch(path.stem):
                continue
            try:
                if path.resolve() != path:
                    continue
                record = _json(path)
                if not isinstance(record, dict) or record.get("input_id") != path.stem or record.get("source") != "uploaded-pgm":
                    continue
                integer(record["bytes"], "bytes", 1, MAX_UPLOAD)
                self._inputs[path.stem] = record
                self._input(path.stem)
            except (OSError, ValueError, TypeError, KeyError):
                self._inputs.pop(path.stem, None)
        for path in self.output_root.glob("frame-*.json"):
            if not RUN_ID.fullmatch(path.stem):
                continue
            try:
                if path.resolve() != path:
                    continue
                run = _json(path)
                if not isinstance(run, dict) or run.get("id") != path.stem:
                    raise ValueError("索引编号不匹配。")
                created = run.get("created_at")
                if type(created) not in (int, float) or not math.isfinite(created) or created < 0:
                    raise ValueError("索引时间无效。")
                if run.get("status") != "complete":
                    run.update(status="failed", error=run.get("error") or "服务重启，前次任务未完成。")
                else:
                    run = self._validate(run)
            except (OSError, ValueError, TypeError, KeyError) as error:
                run = dict(id=path.stem, status="failed", created_at=path.stat().st_mtime,
                           error=f"历史结果验证失败：{error}", **FIXED)
            self._runs[path.stem] = run
            self._save(run)
        if self._runs:
            self._current = max(self._runs, key=lambda key: self._runs[key].get("created_at", 0))

    def state(self):
        with self._lock:
            return copy.deepcopy(dict(busy=self._busy, current=self._runs.get(self._current),
                                      runs=sorted(self._runs.values(), key=lambda r: r.get("created_at", 0), reverse=True),
                                      inputs=sorted(self._inputs.values(), key=lambda r: r["created_at"], reverse=True)))

    def _complete(self, identifier):
        self._path(identifier)
        with self._lock:
            if identifier not in self._runs:
                raise KeyError("运行不存在。")
            run = copy.deepcopy(self._runs[identifier])
        if run["status"] != "complete":
            raise RuntimeError("运行尚未成功完成：" + run.get("error", run["status"]))
        try:
            return self._validate(run)
        except (OSError, ValueError, TypeError, KeyError) as error:
            with self._lock:
                run.update(status="failed", error=f"结果验证失败：{error}")
                self._runs[identifier] = run
                self._save(run)
            raise ValueError(run["error"]) from error

    def results(self, identifier, offset=0, limit=100):
        integer(offset, "offset", 0, (1 << 53) - 1)
        integer(limit, "limit", 1, 1000)
        run = self._complete(identifier)
        items = []
        for index, item in enumerate(_components(self._path(identifier, "components.jsonl"), run["width"], run["height"])):
            if index >= offset + limit:
                break
            if index >= offset:
                items.append(item)
        return dict(schema_version="frame-candidates-v1", run=run, components=items,
                    pagination=dict(offset=offset, limit=limit, total=run["component_count"], returned=len(items),
                                    has_more=offset + len(items) < run["component_count"]))

    def image(self, identifier, kind="source", x=0, y=0, width=None, height=None):
        if not isinstance(kind, str) or kind not in ("source", "corrected", "mask"):
            raise ValueError("kind 必须为 source、corrected 或 mask。")
        run = self._complete(identifier)
        integer(x, "x", 0, run["width"] - 1)
        integer(y, "y", 0, run["height"] - 1)
        path = (self._path(run["input_id"], "source.pgm", input_file=True) if kind == "source"
                else self._path(identifier, kind + ".pgm"))
        pixels = _pgm(path, (run["width"], run["height"]))
        if width is None and height is None:
            if x or y:
                raise ValueError("裁剪须同时提供 width 和 height。")
            step = _preview(run["width"], run["height"])["downsample_step"]
            if kind == "mask":
                padded = np.pad(pixels, ((0, (-pixels.shape[0]) % step), (0, (-pixels.shape[1]) % step)))
                pixels = padded.reshape(padded.shape[0] // step, step, padded.shape[1] // step, step).max(axis=(1, 3))
            else:
                pixels = pixels[::step, ::step]
        else:
            integer(width, "width", 1, 1024)
            integer(height, "height", 1, 512)
            if x + width > run["width"] or y + height > run["height"]:
                raise ValueError("裁剪超出源图像。")
            pixels = pixels[y:y + height, x:x + width]
        return _png(pixels)

    def export(self, identifier):
        """One validation and one streaming scan, with a bounded JSON response."""
        run = self._complete(identifier)
        header = dict(schema_version="frame-candidates-export-v1", run=run)
        output = bytearray(json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode("utf-8")[:-1])
        output.extend(b',"components":[')
        for index, item in enumerate(_components(self._path(identifier, "components.jsonl"), run["width"], run["height"])):
            raw = json.dumps(item, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            if len(output) + len(raw) + 3 > MAX_EXPORT_BYTES:
                raise ValueError("完整导出超过8MiB上限；请减小图像尺寸或提高阈值。不会导出部分结果。")
            if index:
                output.extend(b",")
            output.extend(raw)
        output.extend(b"]}")
        if len(output) > MAX_EXPORT_BYTES:
            raise ValueError("完整导出超过8MiB上限；请减小图像尺寸或提高阈值。不会导出部分结果。")
        return bytes(output)
