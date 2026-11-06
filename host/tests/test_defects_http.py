"""Loopback HTTP integration checks; never connect to serial or board hardware."""
import importlib.util
import json
from pathlib import Path
import sys
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import ProxyHandler, Request, build_opener

import pytest

from test_defects import bundle, defects, png


WORKBENCH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WORKBENCH / "console"))
spec = importlib.util.spec_from_file_location("workbench_http_server", WORKBENCH / "console" / "server.py")
server_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server_module)


class DisconnectedConsole:
    busy = False
    connected = False

    def connect(self, *_):
        raise AssertionError("HTTP defect tests must never connect serial")

    def disconnect(self):
        raise AssertionError("HTTP defect tests must never disconnect serial")

    def state(self, after=0):
        return dict(connected=False, busy=self.busy, events=[])


class Client:
    def __init__(self, server, service, console):
        self.server, self.service, self.console = server, service, console
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
            value = json.loads(body) if content_type.startswith("application/json") else body
            return response.code, response.headers, value

    def get(self, path, headers=None):
        return self.request(path, headers=headers)

    def post(self, path, body=None, headers=None, token=True, raw=None):
        fields = {"Content-Type": "application/json"}
        if token:
            fields["X-SG-Token"] = self.token
        fields.update(headers or {})
        data = raw if raw is not None else json.dumps({} if body is None else body).encode("utf-8")
        return self.request(path, method="POST", data=data, headers=fields)

    def wait(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            code, _, state = self.get("/api/defects/state")
            assert code == 200
            if not state["busy"]:
                return state["current"]
            time.sleep(.02)
        raise AssertionError("native HTTP job did not finish")


@pytest.fixture
def client(bundle):
    service = defects.DefectService(bundle, bundle / "SG3000Workbench" / "runtime" / "defects")
    console = DisconnectedConsole()
    server = server_module.make_server(port=0, console=console, defects=service)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .02}, daemon=True)
    thread.start()
    try:
        yield Client(server, service, console)
    finally:
        # A test failure must not leave a worker accessing an already removed fixture.
        deadline = time.monotonic() + 20
        while service.state()["busy"] and time.monotonic() < deadline:
            time.sleep(.02)
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


def test_actual_native_job_over_http_results_and_png(client):
    code, headers, started = client.post("/api/defects/start", {"radius": 7, "threshold": 25, "polarity": "both"},
                                         headers={"Origin": client.base})
    assert code == 202 and started["status"] == "running"
    assert headers["Cache-Control"] == "no-store"
    run = client.wait()
    assert run["status"] == "complete", run.get("error")
    assert run["component_count"] == 7 and run["provenance"] == "synthetic-native"
    assert run["board_tested"] is False
    code, _, result = client.get("/api/defects/results?" + urlencode({"id": started["id"], "offset": 0, "limit": 100}))
    assert code == 200 and result["pagination"]["total"] == 7
    assert len(result["components"]) == 7
    code, headers, image = client.get("/api/defects/image?" + urlencode({"id": started["id"], "kind": "mask"}))
    assert code == 200 and headers["Content-Type"] == "image/png"
    assert png(image).shape == (17, 2048)
    code, _, image = client.get("/api/defects/image?" + urlencode({"id": started["id"], "kind": "corrected", "x": 249, "y": 57, "width": 20, "height": 20}))
    assert code == 200 and png(image).shape == (20, 20)


def test_http_replay_and_pagination(client):
    code, _, replayed = client.post("/api/defects/replay")
    assert code == 200 and replayed == dict(id="replay-16k", status="complete")
    code, _, result = client.get("/api/defects/results?id=replay-16k&offset=5&limit=2")
    assert code == 200
    assert result["run"]["provenance"] == "recorded-replay"
    assert [item["id"] for item in result["components"]] == ["c000006", "c000007"]
    assert result["pagination"] == dict(offset=5, limit=2, total=7, returned=2, has_more=False)


@pytest.mark.parametrize("headers,token", [({}, False), ({"X-SG-Token": "invalid"}, True),
    ({"Origin": "http://attacker.invalid"}, True), ({"Host": "attacker.invalid"}, True)])
def test_post_token_origin_and_host_rejected(client, headers, token):
    # Authorization runs before body parsing; use a zero-byte request so Windows
    # cannot reset a rejected connection because it still has unread payload.
    code, _, result = client.post("/api/defects/start", headers=headers, token=token, raw=b"")
    assert code == 403 and result["error"]
    assert client.service.state()["runs"] == []


@pytest.mark.parametrize("headers", [{"Host": "attacker.invalid"}, {"Origin": "http://attacker.invalid"}])
def test_get_host_and_origin_rejected(client, headers):
    code, _, body = client.get("/api/defects/state", headers=headers)
    assert code == 403 and body["error"]


@pytest.mark.parametrize("query", [
    "id=replay-16k&offset=-1", "id=replay-16k&limit=0", "id=replay-16k&limit=1001",
    "id=replay-16k&offset=true", "id=replay-16k&limit=1.5", "id=..%2F..%2Fsample_output_16k",
    "id=%2Fetc%2Fpasswd", "id=run-" + "a" * 32, "id=", "id=replay-16k&offset=9007199254740992"
])
def test_results_paging_invalid_or_traversal_rejected(client, query):
    client.post("/api/defects/replay")
    code, _, body = client.get("/api/defects/results?" + query)
    assert code == 400 and body["error"]


@pytest.mark.parametrize("query", ["id=replay-16k&kind=..%2Frun.json", "id=..%2Freplay-16k",
    "id=replay-16k&width=1025&height=1", "id=replay-16k&width=true&height=1",
    "id=replay-16k&x=16383&width=2&height=1", "id=replay-16k&width=20"])
def test_image_invalid_or_traversal_rejected(client, query):
    client.post("/api/defects/replay")
    code, _, body = client.get("/api/defects/image?" + query)
    assert code == 400 and body["error"]


@pytest.mark.parametrize("source,marker", [("tilecontract", '"'), ("integration", "离线"),
    ("defectschema", "defects-v1"), ("entry", "main")])
def test_source_whitelist_keeps_old_and_new_entries(client, source, marker):
    code, _, result = client.get("/api/source?" + urlencode({"id": source}))
    assert code == 200 and marker in result["text"]
    assert result["line"] >= 1 and result["path"]


@pytest.mark.parametrize("source", ["../console/server.py", "../../sample_output_16k/run.json", "C:/Windows/win.ini"])
def test_source_arbitrary_paths_rejected(client, source):
    code, _, result = client.get("/api/source?" + urlencode({"id": source}))
    assert code == 400 and result["error"]


@pytest.mark.parametrize("body", [b"[]", b"[1,2]", b"true", b"null", b"{broken", b"\"string\""])
def test_invalid_json_or_nonobject_body_rejected(client, body):
    code, _, result = client.post("/api/defects/start", raw=body)
    assert code == 400 and result["error"]
    assert client.service.state()["runs"] == []


def test_busy_native_job_blocks_shutdown_and_duplicate_requests(client, monkeypatch):
    gate, entered = threading.Event(), threading.Event()
    original = defects.subprocess.run
    def hold(*args, **kwargs):
        entered.set()
        assert gate.wait(10)
        return original(*args, **kwargs)
    monkeypatch.setattr(defects.subprocess, "run", hold)
    try:
        code, _, _ = client.post("/api/defects/start", {"radius": 0})
        assert code == 202 and entered.wait(3)
        for endpoint in ("/api/shutdown", "/api/defects/start", "/api/defects/replay"):
            code, _, result = client.post(endpoint)
            assert code == 400 and result["error"]
        assert client.get("/api/defects/state")[2]["busy"] is True
    finally:
        gate.set()
    assert client.wait()["status"] == "complete"


def test_serial_console_busy_blocks_shutdown_without_serial_access(client):
    client.console.busy = True
    code, _, result = client.post("/api/shutdown")
    assert code == 400 and result["error"]
    assert client.get("/api/defects/state")[0] == 200
