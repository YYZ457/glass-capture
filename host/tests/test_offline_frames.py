"""Actual file -> native DLL -> complete artifacts; all tests are offline."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

import numpy as np
import pytest

WORKBENCH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WORKBENCH / "console"))
import offline_frames as frames
import process_frame as worker


@pytest.fixture
def service(tmp_path):
    module = tmp_path / "linescan_preprocess_v2"
    for name in ("python", "examples", "prebuilt"):
        shutil.copytree(WORKBENCH.parent / "linescan_preprocess_v2" / name, module / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.zip", "*.elf"))
    return frames.FrameService(tmp_path)


def upload(service, pixels, header=None, name="glass.pgm"):
    height, width = pixels.shape
    raw = (header or f"P5\n{width} {height}\n255\n".encode()) + pixels.tobytes()
    return service.register_pgm(io.BytesIO(raw), len(raw), name), raw


def wait(service):
    deadline = time.monotonic() + 25
    while service.state()["busy"] and time.monotonic() < deadline:
        time.sleep(.01)
    assert not service.state()["busy"]
    return service.state()["current"]


def png(raw):
    assert raw[:8] == b"\x89PNG\r\n\x1a\n"
    pos, blocks, size = 8, [], None
    while pos < len(raw):
        length = struct.unpack(">I", raw[pos:pos + 4])[0]
        kind, payload = raw[pos + 4:pos + 8], raw[pos + 8:pos + 8 + length]
        assert struct.unpack(">I", raw[pos + 8 + length:pos + 12 + length])[0] == zlib.crc32(kind + payload) & 0xffffffff
        if kind == b"IHDR":
            width, height, depth, color, *_ = struct.unpack(">IIBBBBB", payload)
            assert (depth, color) == (8, 0)
            size = width, height
        if kind == b"IDAT":
            blocks.append(payload)
        pos += length + 12
    width, height = size
    rows = np.frombuffer(zlib.decompress(b"".join(blocks)), np.uint8).reshape(height, width + 1)
    assert not rows[:, 0].any()
    return rows[:, 1:]


def test_real_uploaded_16k_seams_oracle_schema_paging_crops_and_restart(service):
    pixels = np.full((135, 16384), 100, np.uint8)
    pixels[59:70, 252:261] = 220
    pixels[125:134, 510:517] = 10
    pixels[:3, -4:] = 230
    item, raw = upload(service, pixels, name="C:\\private\\glass.pgm")
    assert item["width"] == 16384 and item["height"] == 135
    started = service.start(dict(input_id=item["input_id"]))
    run = wait(service)
    assert run["status"] == "complete", run.get("error")
    assert run["source"] == "uploaded-pgm" and run["provenance"] == "file-native"
    assert run["input_sha256"] == hashlib.sha256(raw).hexdigest()
    assert run["display_name"] == "glass.pgm"
    assert run["component_count"] == 7
    assert run["output_words_sha256"] == "35ebca3127ab78ebdfe700368ec338d8952399e1999abca31450aa9118fe9a5e"
    full = service.results(started["id"])
    assert any(c["bbox"] == [252, 59, 261, 70] and c["area"] == 99 for c in full["components"])
    assert full["run"]["decision"] == "unknown" and full["run"]["board_tested"] is False
    assert [c["id"] for c in service.results(started["id"], 2, 2)["components"]] == ["c000003", "c000004"]
    assert not service.results(started["id"], 900)["components"]
    exported = json.loads(service.export(started["id"]))
    assert exported == dict(schema_version="frame-candidates-export-v1", run=full["run"], components=full["components"])
    assert len(service.export(started["id"])) < frames.MAX_EXPORT_BYTES
    import jsonschema
    jsonschema.validate(full, json.loads((WORKBENCH / "interfaces" / "frame-candidates-v1.schema.json").read_text(encoding="utf-8")))
    source_crop = png(service.image(started["id"], "source", 249, 57, 20, 20))
    assert np.array_equal(source_crop, pixels[57:77, 249:269])
    assert np.array_equal(png(service.image(started["id"], "corrected", 249, 57, 20, 20)), source_crop)
    mask = frames._pgm(service.output_root / started["id"] / "mask.pgm")
    preview = png(service.image(started["id"], "mask"))
    assert preview.shape == (17, 2048)
    for y, x in zip(*np.nonzero(mask)):
        assert preview[y // 8, x // 8] == 255
    for name, digest in run["output_sha256"].items():
        assert frames._sha(service.output_root / run["id"] / name) == digest
    restored = frames.FrameService(service.bundle_root)
    assert restored.results(started["id"]) == full
    assert restored.state()["inputs"] == service.state()["inputs"]


def test_small_frame_and_no_detection_r0(service):
    item, _ = upload(service, np.arange(35, dtype=np.uint8).reshape(5, 7))
    service.start(dict(input_id=item["input_id"], radius=0))
    run = wait(service)
    assert run["status"] == "complete", run.get("error")
    assert run["component_count"] == 0
    assert png(service.image(run["id"])).shape == (5, 7)


class Fragmented(io.BytesIO):
    def read(self, count=-1):
        return super().read(min(count, (self.tell() % 11) + 1))


@pytest.mark.parametrize("delimiter", [b"\n", b"\r\n", b" ", b"\t"])
def test_fragment_upload_and_binary_whitespace_pixels(service, delimiter):
    pixels = bytes([10, 13, 32, 9, 35, 0])
    raw = b"P5\n# comment\n3 2\n255" + delimiter + pixels
    source = Fragmented(raw + b"next-request")
    registered = service.register_pgm(source, len(raw), "../safe.pgm")
    assert source.tell() == len(raw)
    actual = service._input(registered["input_id"])[2]
    assert actual.tobytes() == pixels
    assert service.state()["inputs"][0]["display_name"] == "safe.pgm"


@pytest.mark.parametrize("raw", [b"", b"P2\n1 1\n255\n\x00", b"P5\n1 1\n256\n\x00", b"P5\n1 1\n255\n",
    b"P5\n1 1\n255\n\x00x", b"P5\n0 1\n255\n", b"P5\n16385 1\n255\n", b"P5\n1 65537\n255\n",
    b"P5\n16384 513\n255\n", b"P5\n#" + b"x" * 4096 + b"\n1 1\n255\n\x00",
    b"P5\n1 1\n255#comment\n\x00"])
def test_bad_upload_never_registered_or_published(service, raw):
    with pytest.raises(ValueError):
        service.register_pgm(io.BytesIO(raw), max(1, len(raw)), "bad.pgm")
    assert service.state()["inputs"] == []
    assert list(service.output_root.iterdir()) == []


@pytest.mark.parametrize("length", [True, 0, -1, 8 * 1024 * 1024 + 4097, 1.5, "14"])
def test_upload_length_bounds(service, length):
    with pytest.raises(ValueError):
        service.register_pgm(io.BytesIO(b"x"), length, "x.pgm")


@pytest.mark.parametrize("options", [None, [], True, {}, {"input_id": "../evil"}, {"radius": True},
    {"radius": 8}, {"radius": 1.0}, {"threshold": 0}, {"threshold": "25"}, {"polarity": "BOTH"},
    {"input": "C:/file.pgm"}, {"output_dir": "../../"}, {"native_library": "cmd.exe"}])
def test_bad_processing_options(service, options):
    item, _ = upload(service, np.zeros((2, 2), np.uint8))
    if isinstance(options, dict) and options and "input_id" not in options:
        options = dict(input_id=item["input_id"], **options)
    with pytest.raises(ValueError):
        service.start(options)
    assert not service.state()["runs"]


@pytest.mark.parametrize("failure", ["exit", "timeout", "partial", "missing"])
def test_failed_jobs_are_unreadable_and_next_job_works(service, monkeypatch, failure):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    original = frames.subprocess.run
    def fake(command, **kwargs):
        assert kwargs["timeout"] == 120
        if failure == "timeout":
            raise subprocess.TimeoutExpired(command, 120)
        if failure == "partial":
            folder = Path(command[command.index("--output-dir") + 1])
            folder.mkdir()
            (folder / "mask.pgm.partial").write_bytes(b"partial")
        return subprocess.CompletedProcess(command, 1 if failure == "exit" else 0)
    monkeypatch.setattr(frames.subprocess, "run", fake)
    started = service.start(dict(input_id=item["input_id"]))
    assert wait(service)["status"] == "failed"
    with pytest.raises(RuntimeError):
        service.results(started["id"])
    with pytest.raises(RuntimeError):
        service.image(started["id"])
    monkeypatch.setattr(frames.subprocess, "run", original)
    service.start(dict(input_id=item["input_id"]))
    assert wait(service)["status"] == "complete"


def test_restart_interrupted_and_input_tamper(service):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    identifier = "frame-" + "a" * 32
    record = dict(id=identifier, status="running", created_at=time.time(), **frames.FIXED)
    service._save(record)
    restored = frames.FrameService(service.bundle_root)
    assert restored.state()["current"]["status"] == "failed"
    assert "重启" in restored.state()["current"]["error"]
    path = restored.output_root / item["input_id"] / "source.pgm"
    raw = path.read_bytes()
    path.write_bytes(raw[:-1] + b"\x01")
    with pytest.raises(ValueError):
        restored.start(dict(input_id=item["input_id"]))
    assert frames.FrameService(service.bundle_root).state()["inputs"] == []


@pytest.mark.parametrize("identifier", ["../x", "input-" + "a" * 32, "frame-a", True, "C:/file"])
def test_ids_never_become_paths(service, identifier):
    with pytest.raises(ValueError):
        service.results(identifier)


def test_output_change_invalidates_completed_state(service):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    service.start(dict(input_id=item["input_id"]))
    run = wait(service)
    path = service.output_root / run["id"] / "corrected.pgm"
    path.write_bytes(path.read_bytes()[:-1] + b"\x01")
    with pytest.raises(ValueError):
        service.results(run["id"])
    assert service.state()["current"]["status"] == "failed"


def test_tall_preview_pool_is_bounded_without_losing_single_hits():
    # Shape calculation is independent of frame aspect ratio, unlike old demo.
    assert frames._preview(1, 65536) == dict(source_width=1, source_height=65536,
        preview_width=1, preview_height=1024, downsample_step=64)


def test_actual_tall_mask_pool_bounds_and_hits(service):
    pixels = np.full((4097, 1), 100, np.uint8)
    pixels[499:501] = 220
    item, _ = upload(service, pixels)
    service.start(dict(input_id=item["input_id"], radius=1, threshold=25, polarity="bright"))
    run = wait(service)
    assert run["status"] == "complete", run.get("error")
    mask = frames._pgm(service.output_root / run["id"] / "mask.pgm")
    overview = png(service.image(run["id"], "mask"))
    assert overview.shape == (820, 1)
    assert run["images"]["mask"]["downsample_step"] == 5
    assert mask.any()
    for y, x in zip(*np.nonzero(mask)):
        assert overview[y // 5, x // 5] == 255


def test_real_subprocess_timeout_then_recovery(service):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    service.timeout_seconds = .00001
    service.start(dict(input_id=item["input_id"]))
    run = wait(service)
    assert run["status"] == "failed" and "TimeoutExpired" in run["error"]
    service.timeout_seconds = 120
    service.start(dict(input_id=item["input_id"]))
    assert wait(service)["status"] == "complete"


def test_busy_serializes_jobs_but_allows_input_registration(service, monkeypatch):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    entered, release = threading.Event(), threading.Event()
    original = frames.subprocess.run
    def delayed(command, **kwargs):
        entered.set()
        assert release.wait(5)
        return original(command, **kwargs)
    monkeypatch.setattr(frames.subprocess, "run", delayed)
    started = service.start(dict(input_id=item["input_id"]))
    try:
        assert entered.wait(5)
        with pytest.raises(ValueError):
            service.start(dict(input_id=item["input_id"]))
        with pytest.raises(RuntimeError):
            service.results(started["id"])
        upload(service, np.ones((2, 3), np.uint8))
        assert len(service.state()["inputs"]) == 2
    finally:
        release.set()
    assert wait(service)["status"] == "complete"


@pytest.mark.parametrize("record", [[], None, {"id": "wrong"},
    {"id": "frame-" + "b" * 32, "status": "running", "created_at": "bad"}])
def test_corrupt_history_is_failed_without_crashing(service, record):
    path = service.output_root / ("frame-" + "b" * 32 + ".json")
    path.write_text(json.dumps(record), encoding="utf-8")
    restored = frames.FrameService(service.bundle_root)
    assert restored.state()["current"]["status"] == "failed"


def test_corrupt_native_metadata_invalidates_complete_run(service):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    service.start(dict(input_id=item["input_id"]))
    run = wait(service)
    (service.output_root / run["id"] / "run.json").write_text("[]", encoding="utf-8")
    with pytest.raises(ValueError):
        service.results(run["id"])
    assert service.state()["current"]["status"] == "failed"


def test_dense_real_native_input_fails_before_candidate_file_exceeds_cap(service):
    pixels = np.full((768, 768), 100, np.uint8)
    pixels[::3, ::4] = 220
    pixels[::3, 1::4] = 220
    item, _ = upload(service, pixels)
    service.start(dict(input_id=item["input_id"], radius=1, threshold=25, polarity="bright"))
    run = wait(service)
    assert run["status"] == "failed", run
    assert "候选文件超过4MiB" in run["error"]
    folder = service.output_root / run["id"]
    assert (folder / "components.jsonl.partial").stat().st_size <= frames.MAX_JSONL_BYTES
    assert all(p.stat().st_size < 10 * 1024 * 1024 for p in folder.iterdir())
    assert not (folder / "components.jsonl").exists()
    with pytest.raises(RuntimeError):
        service.export(run["id"])


def test_candidate_writer_boundary_is_checked_before_write(monkeypatch):
    from dataclasses import dataclass
    @dataclass
    class Candidate:
        area: int = 2
    buffer = io.BytesIO()
    one = (json.dumps({"area": 2}) + "\n").encode()
    monkeypatch.setattr(worker, "MAX_JSONL_BYTES", len(one))
    assert worker.write_components(buffer, [Candidate()], 0) == (1, len(one))
    with pytest.raises(worker.CandidateLimitExceeded):
        worker.write_components(buffer, [Candidate()], len(one))
    assert buffer.getvalue() == one


def test_export_never_returns_partial_when_response_limit_exceeded(service, monkeypatch):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    service.start(dict(input_id=item["input_id"]))
    run = wait(service)
    # A zero-candidate response still contains metadata and must honor the cap.
    monkeypatch.setattr(frames, "MAX_EXPORT_BYTES", 1)
    with pytest.raises(ValueError):
        service.export(run["id"])


@pytest.mark.parametrize("kwargs", [dict(kind="../x"), dict(width=1), dict(x=1), dict(x=True),
    dict(width=True, height=1), dict(width=1025, height=1), dict(width=1, height=513),
    dict(x=2, y=0, width=2, height=1)])
def test_crop_bounds(service, kwargs):
    item, _ = upload(service, np.zeros((2, 3), np.uint8))
    service.start(dict(input_id=item["input_id"]))
    run = wait(service)
    with pytest.raises(ValueError):
        service.image(run["id"], **kwargs)
