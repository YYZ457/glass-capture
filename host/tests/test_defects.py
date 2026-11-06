"""Offline service contract tests; the main test executes the actual native DLL."""
import concurrent.futures
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import threading
import time
import zlib

import numpy as np
import pytest


WORKBENCH = Path(__file__).resolve().parents[1]
SOURCE_BUNDLE = WORKBENCH.parent
spec = importlib.util.spec_from_file_location("workbench_defects", WORKBENCH / "console" / "defects.py")
defects = importlib.util.module_from_spec(spec)
spec.loader.exec_module(defects)


@pytest.fixture
def bundle(tmp_path):
    shutil.copytree(SOURCE_BUNDLE / "sample_output_16k", tmp_path / "sample_output_16k")
    module = tmp_path / "linescan_preprocess_v2"
    for name in ("python", "examples", "prebuilt"):
        shutil.copytree(SOURCE_BUNDLE / "linescan_preprocess_v2" / name, module / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.zip", "*.elf"))
    return tmp_path


@pytest.fixture
def service(bundle):
    return defects.DefectService(bundle, bundle / "SG3000Workbench" / "runtime" / "defects")


def wait(service):
    deadline = time.monotonic() + 20
    while service.state()["busy"] and time.monotonic() < deadline:
        time.sleep(.02)
    assert not service.state()["busy"], "native worker did not finish"
    return service.state()["current"]


def png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, compressed, dimensions = 8, b"", None
    while pos < len(data):
        size = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, payload = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + size]
        crc = struct.unpack(">I", data[pos + 8 + size:pos + 12 + size])[0]
        assert crc == zlib.crc32(kind + payload) & 0xffffffff
        if kind == b"IHDR":
            width, height, depth, color, *_ = struct.unpack(">IIBBBBB", payload)
            assert (depth, color) == (8, 0)
            dimensions = width, height
        if kind == b"IDAT":
            compressed += payload
        pos += size + 12
    width, height = dimensions
    raster = np.frombuffer(zlib.decompress(compressed), np.uint8).reshape(height, width + 1)
    assert np.all(raster[:, 0] == 0)
    return raster[:, 1:]


def test_real_default_native_demo_and_restart(service):
    started = service.start()
    assert started["status"] == "running"
    run = wait(service)
    assert run["status"] == "complete", run.get("error")
    result = service.results(started["id"])
    assert (run["width"], run["height"], run["component_count"]) == (16384, 135, 7)
    assert run["provenance"] == "synthetic-native"
    assert run["board_tested"] is False and run["decision"] == "unknown"
    assert run["output_words_sha256"] == "35ebca3127ab78ebdfe700368ec338d8952399e1999abca31450aa9118fe9a5e"
    assert result["pagination"]["total"] == 7
    assert all(c["signed_peak"] == c["peak"] * (1 if c["polarity"] == "bright" else -1) for c in result["components"])
    import jsonschema
    schema = json.loads((WORKBENCH / "interfaces" / "defects-v1.schema.json").read_text(encoding="utf-8"))
    jsonschema.validate(result, schema)
    restarted = defects.DefectService(service.bundle_root, service.output_root)
    assert restarted.results(started["id"]) == result
    assert restarted.state()["busy"] is False


def test_replay_paging_png_crop_and_no_sample_writes(service):
    sample = service.bundle_root / "sample_output_16k"
    before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in sample.iterdir()}
    assert service.replay() == dict(id="replay-16k", status="complete")
    result = service.results("replay-16k", 2, 2)
    assert result["run"]["provenance"] == "recorded-replay"
    assert result["pagination"] == dict(offset=2, limit=2, total=7, returned=2, has_more=True)
    assert [c["id"] for c in result["components"]] == ["c000003", "c000004"]
    assert service.results("replay-16k", 999)["components"] == []
    mask = png(service.image("replay-16k", "mask"))
    assert mask.shape == (17, 2048)
    full = defects._pgm(sample / "mask.pgm")
    for y, x in zip(*np.nonzero(full)):
        assert mask[y // 8, x // 8] == 255
    cropped = png(service.image("replay-16k", "corrected", 249, 57, 20, 20))
    assert np.array_equal(cropped, defects._pgm(sample / "corrected.pgm")[57:77, 249:269])
    assert before == {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in sample.iterdir()}


@pytest.mark.parametrize("options", [True, [], {"radius": True}, {"radius": -1}, {"radius": 8},
    {"radius": 1.0}, {"threshold": False}, {"threshold": 0}, {"threshold": 256},
    {"threshold": "25"}, {"polarity": 0}, {"polarity": "BOTH"}, {"input": "C:/anything"},
    {"native_library": "cmd.exe"}, {"output_dir": "../outside"}])
def test_invalid_options(service, options):
    with pytest.raises(ValueError):
        service.start(options)
    assert service.state() == dict(busy=False, current=None, runs=[])


@pytest.mark.parametrize("args", [("../run-x",), (True,), ("replay-16k", True),
    ("replay-16k", 0, False), ("replay-16k", 0, 1001), ("replay-16k", -1)])
def test_invalid_result_scalars(service, args):
    with pytest.raises(ValueError):
        service.results(*args)


@pytest.mark.parametrize("kwargs", [dict(kind="../run.json"), dict(x=True), dict(width=True, height=1),
    dict(width=1025, height=1), dict(width=1, height=513), dict(width=10), dict(x=1),
    dict(x=16383, width=2, height=1), dict(y=134, width=1, height=2)])
def test_invalid_image_arguments(service, kwargs):
    service.replay()
    with pytest.raises(ValueError):
        service.image("replay-16k", **kwargs)


def test_busy_race_serialization_and_lifecycle(service, monkeypatch):
    gate, entered = threading.Event(), threading.Event()
    original = defects.subprocess.run
    def blocked(*args, **kwargs):
        entered.set()
        assert gate.wait(10)
        return original(*args, **kwargs)
    monkeypatch.setattr(defects.subprocess, "run", blocked)
    try:
        with concurrent.futures.ThreadPoolExecutor(6) as pool:
            def attempt(_):
                try:
                    return service.start({"radius": 0})
                except ValueError:
                    return None
            jobs = list(pool.map(attempt, range(6)))
        assert sum(job is not None for job in jobs) == 1
        assert entered.wait(3) and service.state()["busy"]
        with pytest.raises(ValueError):
            service.replay()
        run_id = next(job["id"] for job in jobs if job)
        with pytest.raises(RuntimeError):
            service.results(run_id)
    finally:
        gate.set()
    run = wait(service)
    assert run["status"] == "complete", run.get("error")
    assert run["component_count"] == 0
    assert service.results(run_id)["components"] == []


@pytest.mark.parametrize("mode", ["timeout", "exit", "missing"])
def test_failures_never_report_success(service, monkeypatch, mode):
    def fail(*args, **kwargs):
        if mode == "timeout":
            raise subprocess.TimeoutExpired(args[0], .01)
        return subprocess.CompletedProcess(args[0], 12 if mode == "exit" else 0)
    monkeypatch.setattr(defects.subprocess, "run", fail)
    job = service.start()
    run = wait(service)
    assert run["status"] == "failed" and run["error"]
    with pytest.raises(RuntimeError):
        service.results(job["id"])
    assert not service.state()["busy"]


@pytest.mark.parametrize("field,value", [("bbox", [0, 0, 20000, 3]), ("bbox", [0, 0, 0, 2]),
    ("area", True), ("area", 999), ("peak", -1), ("peak", 256), ("polarity", "both"),
    ("frame_id", -1), ("frame_id", True)])
def test_malformed_components_visible(service, field, value):
    path = service.bundle_root / "sample_output_16k" / "components.jsonl"
    items = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    items[0][field] = value
    path.write_text("".join(json.dumps(item) + "\n" for item in items), encoding="utf-8")
    with pytest.raises(ValueError):
        service.replay()
    assert service.state()["current"]["status"] == "failed"


def test_uint64_frame_safe_json(service):
    path = service.bundle_root / "sample_output_16k" / "components.jsonl"
    items = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    items[0]["frame_id"] = (1 << 64) - 1
    path.write_text("".join(json.dumps(item) + "\n" for item in items), encoding="utf-8")
    service.replay()
    assert service.results("replay-16k")["components"][0]["frame_id"] == "18446744073709551615"


def test_header_bounds_and_changed_artifacts(service):
    service.replay()
    path = service.bundle_root / "sample_output_16k" / "corrected.pgm"
    path.write_bytes(b"P5\n#" + b"x" * 5000 + b"\n1 1\n255\n\0")
    with pytest.raises(ValueError):
        service.image("replay-16k")
    assert service.state()["current"]["status"] == "failed"


def test_results_store_not_capped_by_page_limit(service):
    sample = service.bundle_root / "sample_output_16k"
    first = (sample / "components.jsonl").read_text(encoding="utf-8").splitlines()[0]
    (sample / "components.jsonl").write_text((first + "\n") * 1007, encoding="utf-8")
    meta = json.loads((sample / "run.json").read_text(encoding="utf-8"))
    meta["components"] = 1007
    (sample / "run.json").write_text(json.dumps(meta), encoding="utf-8")
    service.replay()
    page = service.results("replay-16k", 1000, 1000)
    assert page["pagination"] == dict(offset=1000, limit=1000, total=1007, returned=7, has_more=False)
    assert page["components"][-1]["id"] == "c001007"


@pytest.mark.parametrize("damage", ["unfinished", "mismatch", "trailing", "nonbinary", "count", "threshold"])
def test_incomplete_or_inconsistent_artifacts(service, damage):
    sample = service.bundle_root / "sample_output_16k"
    meta = json.loads((sample / "run.json").read_text(encoding="utf-8"))
    if damage == "unfinished":
        meta["status"] = "running"
    elif damage == "mismatch":
        meta["width"] = 32
    elif damage == "count":
        meta["components"] = 6
    elif damage == "threshold":
        meta["threshold"] = 255
    elif damage == "trailing":
        with (sample / "corrected.pgm").open("ab") as file:
            file.write(b"extra")
    elif damage == "nonbinary":
        path = sample / "mask.pgm"
        data = path.read_bytes()
        path.write_bytes(data[:-1] + b"\x01")
    (sample / "run.json").write_text(json.dumps(meta), encoding="utf-8")
    with pytest.raises(ValueError):
        service.replay()
    assert service.state()["current"]["status"] == "failed"


def test_running_recovery_and_output_confinement(service):
    run_id = "run-" + "a" * 32
    run = dict(id=run_id, status="running", provenance="synthetic-native", created_at=time.time())
    (service.output_root / (run_id + ".json")).write_text(json.dumps(run), encoding="utf-8")
    restarted = defects.DefectService(service.bundle_root, service.output_root)
    assert restarted.state()["current"]["status"] == "failed"
    with pytest.raises(ValueError):
        defects.DefectService(service.bundle_root, service.output_root.parent)


def test_results_schema(service):
    import jsonschema
    service.replay()
    schema = json.loads((WORKBENCH / "interfaces" / "defects-v1.schema.json").read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    jsonschema.validate(service.results("replay-16k"), schema)
