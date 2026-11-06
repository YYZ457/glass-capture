"""Loopback file-upload integration. Injected services never access hardware."""
import http.client
import importlib.util
import json
from pathlib import Path
import shutil
import socket
import struct
import sys
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import ProxyHandler, Request, build_opener
import zlib

import numpy as np
import pytest

WORKBENCH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WORKBENCH / "console"))
import defects
import offline_frames

spec = importlib.util.spec_from_file_location("frame_http_test_server", WORKBENCH / "console" / "server.py")
server_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server_module)


class DisconnectedConsole:
    busy = False
    connected = False

    def connect(self, *_):
        raise AssertionError("Frame HTTP tests must never connect serial")

    def disconnect(self):
        raise AssertionError("Frame HTTP tests must never disconnect serial")

    def state(self, after=0):
        return dict(connected=False, busy=self.busy, events=[])


class Client:
    def __init__(self, server, frames, synthetic):
        self.server, self.frames, self.synthetic = server, frames, synthetic
        self.base = f"http://127.0.0.1:{server.server_port}"
        self.opener = build_opener(ProxyHandler({}))
        code, _, bootstrap = self.get("/api/bootstrap")
        assert code == 200
        self.token = bootstrap["token"]

    def request(self, path, method="GET", data=None, headers=None):
        request = Request(self.base + path, data=data, method=method, headers=headers or {})
        try:
            response = self.opener.open(request, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            body = response.read()
            content_type = response.headers.get("Content-Type", "")
            return response.code, response.headers, json.loads(body) if content_type.startswith("application/json") else body

    def get(self, path, headers=None):
        return self.request(path, headers=headers)

    def post(self, path, options=None, raw=None, headers=None, token=True):
        fields = {"Content-Type": "application/json"}
        if token:
            fields["X-SG-Token"] = self.token
        fields.update(headers or {})
        data = raw if raw is not None else json.dumps({} if options is None else options).encode("utf-8")
        return self.request(path, "POST", data, fields)

    def upload(self, pixels, name="glass.pgm", headers=None):
        height, width = pixels.shape
        body = f"P5\n{width} {height}\n255\n".encode("ascii") + pixels.tobytes()
        fields = {"Content-Type": "image/x-portable-graymap", "Origin": self.base}
        fields.update(headers or {})
        return self.post("/api/frames/upload?" + urlencode(dict(name=name)), raw=body, headers=fields)

    def wait(self, kind="frames"):
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            code, _, state = self.get(f"/api/{kind}/state")
            assert code == 200
            if not state["busy"]:
                return state["current"]
            time.sleep(.02)
        raise AssertionError("HTTP native job did not finish")

    def framed_upload(self, declared_length, body=b"", extra_headers=None, half_close=False):
        """Explicit framing allows oversized headers and prematurely ended body."""
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=10)
        try:
            connection.putrequest("POST", "/api/frames/upload?name=framed.pgm")
            connection.putheader("X-SG-Token", self.token)
            connection.putheader("Content-Type", "application/octet-stream")
            connection.putheader("Content-Length", str(declared_length))
            for key, value in (extra_headers or {}).items():
                connection.putheader(key, value)
            connection.endheaders(body)
            if half_close:
                connection.sock.shutdown(socket.SHUT_WR)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()


@pytest.fixture
def client(tmp_path):
    module = tmp_path / "linescan_preprocess_v2"
    for name in ("python", "examples", "prebuilt"):
        shutil.copytree(WORKBENCH.parent / "linescan_preprocess_v2" / name, module / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.zip", "*.elf"))
    shutil.copytree(WORKBENCH.parent / "sample_output_16k", tmp_path / "sample_output_16k")
    frames = offline_frames.FrameService(tmp_path)
    synthetic = defects.DefectService(tmp_path, tmp_path / "SG3000Workbench" / "runtime" / "defects")
    server = server_module.make_server(port=0, console=DisconnectedConsole(), defects=synthetic, frames=frames)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .02}, daemon=True)
    thread.start()
    try:
        yield Client(server, frames, synthetic)
    finally:
        deadline = time.monotonic() + 25
        while (frames.state()["busy"] or synthetic.state()["busy"]) and time.monotonic() < deadline:
            time.sleep(.02)
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


def png(raw):
    assert raw[:8] == b"\x89PNG\r\n\x1a\n"
    pos, blocks, dimensions = 8, [], None
    while pos < len(raw):
        length = struct.unpack(">I", raw[pos:pos + 4])[0]
        kind, payload = raw[pos + 4:pos + 8], raw[pos + 8:pos + 8 + length]
        assert struct.unpack(">I", raw[pos + 8 + length:pos + 12 + length])[0] == zlib.crc32(kind + payload) & 0xffffffff
        if kind == b"IHDR":
            width, height, depth, color, *_ = struct.unpack(">IIBBBBB", payload)
            assert (depth, color) == (8, 0)
            dimensions = width, height
        if kind == b"IDAT":
            blocks.append(payload)
        pos += length + 12
    width, height = dimensions
    rows = np.frombuffer(zlib.decompress(b"".join(blocks)), np.uint8).reshape(height, width + 1)
    assert not rows[:, 0].any()
    return rows[:, 1:]


def test_authenticated_pgm_upload_native_results_schema_source_crop_export(client):
    code, headers, root = client.get("/")
    assert code == 200 and headers["Content-Type"].startswith("text/html") and b"<html" in root.lower()
    code, _, bootstrap = client.get("/api/bootstrap")
    assert code == 200 and bootstrap["version"] == "0.4"
    assert {"frames", "frameschema"} <= {file["id"] for file in bootstrap["files"]}
    pixels = np.full((75, 513), 100, np.uint8)
    pixels[59:70, 252:261] = 220
    code, headers, uploaded = client.upload(pixels, name="相机 导出#1.pgm")
    assert code == 201 and (uploaded["width"], uploaded["height"]) == (513, 75)
    assert headers["Cache-Control"] == "no-store"
    assert headers["X-Content-Type-Options"] == "nosniff"
    code, _, started = client.post("/api/frames/start", dict(input_id=uploaded["input_id"], radius=7,
        threshold=25, polarity="bright"), headers={"Origin": client.base})
    assert code == 202 and started["status"] == "running"
    run = client.wait()
    assert run["status"] == "complete", run.get("error")
    assert run["display_name"] == "相机 导出#1.pgm"
    assert run["source"] == "uploaded-pgm" and run["provenance"] == "file-native"
    assert run["decision"] == "unknown" and run["board_tested"] is False and run["accuracy_evaluated"] is False
    code, _, result = client.get("/api/frames/results?" + urlencode(dict(id=started["id"], limit=100)))
    assert code == 200 and result["pagination"]["total"] == 1
    assert result["components"][0]["bbox"] == [252, 59, 261, 70]
    import jsonschema
    schema = json.loads((WORKBENCH / "interfaces" / "frame-candidates-v1.schema.json").read_text(encoding="utf-8"))
    jsonschema.validate(result, schema)
    code, headers, raw = client.get("/api/frames/image?" + urlencode(dict(id=started["id"], kind="source")))
    assert code == 200 and headers["Content-Type"] == "image/png"
    assert np.array_equal(png(raw), pixels)
    crop = dict(id=started["id"], kind="source", x=249, y=57, width=20, height=16)
    code, _, raw = client.get("/api/frames/image?" + urlencode(crop))
    assert code == 200 and np.array_equal(png(raw), pixels[57:73, 249:269])
    code, _, mask = client.get("/api/frames/image?" + urlencode(dict(id=started["id"], kind="mask")))
    assert code == 200 and png(mask)[59:70, 252:261].all()
    code, headers, exported = client.get("/api/frames/export?" + urlencode(dict(id=started["id"])))
    assert code == 200 and headers["Content-Type"].startswith("application/json")
    assert exported == dict(schema_version="frame-candidates-export-v1", run=result["run"], components=result["components"])


@pytest.mark.parametrize("body", [b"P2\n2 2\n255\n0000", b"P5\n2 2\n255\n\x00\x01\x02",
    b"P5\n2 2\n255\n\x00\x01\x02\x03x", b"P5\n0 2\n255\n", b"P5\n2 2\n65535\n0000"])
def test_invalid_or_truncated_raster_is_rejected_with_exact_body_length(client, body):
    code, _, error = client.post("/api/frames/upload", raw=body, headers={"Content-Type": "application/octet-stream"})
    assert code == 400 and error["error"]
    assert client.frames.state()["inputs"] == []
    assert client.frames.state()["runs"] == []


def test_short_http_body_rejected_after_eof_without_waiting_for_timeout(client):
    raw = b"P5\n2 2\n255\n\x00\x01"
    started = time.monotonic()
    code, error = client.framed_upload(len(raw) + 2, raw, half_close=True)
    assert code == 400 and error["error"]
    assert time.monotonic() - started < 5
    assert client.frames.state()["inputs"] == []


@pytest.mark.parametrize("content_type", ["application/json", "text/plain", "image/png", "multipart/form-data"])
def test_wrong_content_type_rejected_before_reading_upload(client, content_type):
    # Supply a declared body byte but no body: content-type must be rejected
    # before reading, otherwise this request would wait for the 15s timeout.
    connection = http.client.HTTPConnection("127.0.0.1", client.server.server_port, timeout=3)
    try:
        connection.putrequest("POST", "/api/frames/upload")
        connection.putheader("X-SG-Token", client.token)
        connection.putheader("Content-Type", content_type)
        connection.putheader("Content-Length", "1")
        connection.endheaders()
        response = connection.getresponse()
        assert response.status == 400 and json.loads(response.read())["error"]
    finally:
        connection.close()


@pytest.mark.parametrize("headers,token", [({}, False), ({"X-SG-Token": "invalid"}, True),
    ({"Origin": "http://attacker.invalid"}, True), ({"Origin": "null"}, True),
    ({"Host": "attacker.invalid"}, True)])
def test_upload_requires_token_and_rejects_foreign_origin_or_host(client, headers, token):
    code, _, error = client.post("/api/frames/upload", raw=b"", headers=headers, token=token)
    assert code == 403 and error["error"]
    assert not client.frames.state()["inputs"]


def test_valid_local_client_without_origin_is_supported(client):
    raw = b"P5\n2 1\n255\n\x0a\x23"
    code, _, uploaded = client.post("/api/frames/upload?name=no-origin.pgm", raw=raw,
                                   headers={"Content-Type": "application/octet-stream"})
    assert code == 201 and uploaded["width"] == 2


def test_oversized_upload_and_transfer_encoding_rejected_before_reading(client):
    code, error = client.framed_upload(offline_frames.MAX_UPLOAD + 1)
    assert code == 400 and error["error"]
    code, error = client.framed_upload(1, extra_headers={"Transfer-Encoding": "chunked"})
    assert code == 400 and error["error"]
    assert not client.frames.state()["inputs"]


@pytest.mark.parametrize("path", [
    "/api/frames/results?id=..%2F..%2Fprivate", "/api/frames/export?id=C%3A%2FWindows%2Fwin.ini",
    "/api/frames/image?id=..%2Fsource.pgm", "/api/frames/results?id=run-" + "a" * 32,
    "/api/frames/results?id=input-" + "a" * 32, "/api/frames/results?id=frame-" + "a" * 32,
    "/api/frames/results?id=x&limit=1001", "/api/frames/results?id=x&offset=true"
])
def test_paths_bad_ids_and_invalid_page_scalars_are_rejected(client, path):
    code, _, error = client.get(path)
    assert code == 400 and error["error"]


@pytest.mark.parametrize("options", [{"input": "C:/image.pgm"}, {"input_id": "../source.pgm"},
    {"input_id": "input-" + "a" * 32}, {"input_id": "x", "output_dir": "../escape"}])
def test_start_never_accepts_arbitrary_filesystem_paths(client, options):
    code, _, error = client.post("/api/frames/start", options)
    assert code == 400 and error["error"]
    assert not client.frames.state()["runs"]


def test_filename_is_display_only_and_cannot_choose_storage_path(client):
    code, _, uploaded = client.upload(np.zeros((2, 3), np.uint8), "../../outside.pgm")
    assert code == 201
    assert client.frames.state()["inputs"][0]["display_name"] == "outside.pgm"
    assert (client.frames.output_root / uploaded["input_id"] / "source.pgm").is_file()
    assert not (client.frames.output_root.parent / "outside.pgm").exists()


def test_busy_frame_blocks_synthetic_start_duplicate_frame_and_shutdown(client, monkeypatch):
    code, _, uploaded = client.upload(np.zeros((2, 3), np.uint8))
    assert code == 201
    entered, release = threading.Event(), threading.Event()
    original = offline_frames.subprocess.run
    def hold(*args, **kwargs):
        entered.set()
        assert release.wait(10)
        return original(*args, **kwargs)
    monkeypatch.setattr(offline_frames.subprocess, "run", hold)
    try:
        code, _, started = client.post("/api/frames/start", dict(input_id=uploaded["input_id"], radius=0))
        assert code == 202 and entered.wait(3)
        for path, options in (("/api/defects/start", {}), ("/api/frames/start", dict(input_id=uploaded["input_id"])),
                              ("/api/shutdown", {})):
            code, _, error = client.post(path, options)
            assert code == 400 and error["error"]
        for path in ("/api/frames/results", "/api/frames/export", "/api/frames/image"):
            code, _, error = client.get(path + "?" + urlencode(dict(id=started["id"])))
            assert code == 400 and error["error"]
        assert client.get("/api/frames/state")[2]["busy"] is True
        assert client.synthetic.state()["runs"] == []
    finally:
        release.set()
    assert client.wait()["status"] == "complete"


def test_old_synthetic_route_still_runs_original_seven_candidates(client):
    code, _, started = client.post("/api/defects/start", {})
    assert code == 202
    run = client.wait("defects")
    assert run["status"] == "complete", run.get("error")
    assert run["component_count"] == 7 and run["provenance"] == "synthetic-native"
    code, _, result = client.get("/api/defects/results?" + urlencode(dict(id=started["id"])))
    assert code == 200 and result["schema_version"] == "defects-v1" and len(result["components"]) == 7
    assert client.frames.state()["runs"] == []
