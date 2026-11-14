"""Offline tests only. Simulated serial device; no physical COM port opened."""
import importlib.util
import json
import queue
import re
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'console'))
from transport import Console
import server


class FakeSerial:
    def __init__(self, name):
        self.name = name
        self.queue = queue.Queue()
        self.pending = b''
        self.closed = False
        self.commands = []
        self.mode = 'ok'

    def read(self, size):
        try:
            return self.queue.get(timeout=.02)
        except queue.Empty:
            return b''

    def write(self, data):
        self.pending += data
        if b'\x03' in data:
            self.pending = b''
            self.queue.put(b'^C\r\n/ # ')
        while b'\r' in self.pending:
            line, self.pending = self.pending.split(b'\r', 1)
            if not line:
                self.queue.put(b'\r\n/ # ')
                continue
            wire = line.decode('ascii')
            self.commands.append(wire)
            begin = re.search(r'__SG_BEGIN_[0-9a-f]+__', wire).group()
            end = re.search(r'__SG_END_[0-9a-f]+__', wire).group()
            body, rc = '', 0
            if 'KERNEL=' in wire:
                body = 'KERNEL=5.15.19-xilinx-v2022.1\nARCH=aarch64\nMODEL=Xilinx ZCU102 Rev1.0\nVENDOR=0x4c54\nDEVICE=0x3000\nDRIVER=\nSD=present\n'
                if self.mode == 'wrong-board':
                    body = body.replace('ZCU102', 'Other')
            elif 'preflight' in wire:
                if self.mode == 'ready':
                    body = 'DDRInitialStatus=0, CameraLinkPixelClocks=0x00000001, CL=0, Color=0, Depth=8\nPreflight passed. Acquisition and DMA were not started.\n'
                else:
                    body = 'DDRInitialStatus=2, CameraLinkPixelClocks=0x00000000, CL=0, Color=0, Depth=0\nFailed: Link has been severed (-67)\n'
                    rc = 1
            elif 'capture --timeout' in wire:
                body = 'DDRInitialStatus=0, CameraLinkPixelClocks=0x00000001, CL=0, Color=0, Depth=8\nSaved complete Mono8 frame: 16 x 8, 128 bytes, block 1, 1 packets: /tmp/fake.pgm\n'
            elif wire.count('version') and ('capture version' in wire or 'stream --version' in wire):
                body = 'sg3000-capture 1.0 ABI 1\n' if 'capture version' in wire else 'sg3000-stream 0.2 ABI 1\n'
            elif 'sha256sum' in wire and self.mode == 'bad-hash':
                body, rc = 'sg3000-capture: FAILED\n', 1
            elif self.mode == 'no-marker':
                self.queue.put((wire + '\r\n').encode())
                continue
            # Echo includes marker names; must not count as execution completion.
            reply = wire + '\r\n' + begin + '\r\n' + body + '\r\n' + end + ':' + str(rc) + '\r\n/ # '
            self.queue.put(reply.encode())
        return len(data)

    def close(self):
        self.closed = True


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.c = Console(self.temp.name, FakeSerial)

    def tearDown(self):
        deadline = time.monotonic() + 3
        while self.c.busy and time.monotonic() < deadline:
            time.sleep(.02)
        if self.c.connected and not self.c.busy:
            self.c.disconnect()
        if self.c.reader:
            self.c.reader.join(1)
        self.temp.cleanup()

    def connect(self):
        self.c.connect('COM99')
        self.c.wake()
        deadline = time.monotonic() + 1
        while not self.c.shell_ready and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(self.c.shell_ready)

    def job(self, action):
        self.c.start(action)
        deadline = time.monotonic() + 4
        while self.c.busy and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertFalse(self.c.busy)
        return self.c.job

    def test_offline_no_auto_connection(self):
        self.assertFalse(self.c.state()['connected'])
        self.assertIsNone(self.c.port)
        with self.assertRaises(ValueError):
            self.c.start('probe')

    def test_identity_gate_and_wrong_model(self):
        self.connect()
        with self.assertRaises(ValueError):
            self.c.start('capture')
        self.c.port.mode = 'wrong-board'
        self.assertEqual(self.job('probe')['status'], 'error')
        self.assertFalse(self.c.identity_ok)
        self.c.port.mode = 'ok'
        self.assertEqual(self.job('probe')['status'], 'success')
        self.assertTrue(self.c.identity_ok)

    def test_prepare_readonly_sd_and_unmount(self):
        self.connect(); self.job('probe')
        self.assertEqual(self.job('prepare')['status'], 'success')
        commands = '\n'.join(self.c.port.commands)
        self.assertIn('mount -t vfat -o ro', commands)
        self.assertIn("tr -d '\\r' < SHA256SUMS | sha256sum -c", commands)
        self.assertIn('umount /mnt/sg3000-console', commands)
        self.assertIn('fi && test -e', commands)
        self.assertIn('sg3000-capture version', commands)
        self.assertNotIn('dd if=', commands)          # D003：不再给二进制打补丁

    def test_prepare_hash_failure_still_unmounts_and_never_loads(self):
        self.connect(); self.job('probe'); self.c.port.mode = 'bad-hash'
        self.assertEqual(self.job('prepare')['status'], 'error')
        commands = '\n'.join(self.c.port.commands)
        self.assertIn('umount /mnt/sg3000-console', commands)
        self.assertNotIn('insmod', commands)

    def test_preflight_unread_fields_not_zero_configuration(self):
        self.connect(); self.job('probe')
        self.assertEqual(self.job('preflight')['status'], 'error')
        p = self.c.checks['preflight']
        self.assertEqual(p['ddr'], 2)
        self.assertEqual(p['clocks'], 0)
        self.assertIsNone(p['config'])
        self.assertFalse(p['passed'])

    def test_failed_register_read_does_not_claim_known_zero(self):
        self.c._parse_preflight('DDRInitialStatus=0, CameraLinkPixelClocks=0x00000000, CL=0, Color=0, Depth=0\nFailed: I/O error (-5)', 1)
        self.assertFalse(self.c.checks['preflight']['reads_confirmed'])

    def test_ready_and_capture_result_location(self):
        self.connect(); self.job('probe'); self.c.port.mode = 'ready'
        self.assertEqual(self.job('preflight')['status'], 'success')
        self.assertEqual(self.c.checks['preflight']['config']['depth'], 8)
        self.assertEqual(self.job('capture')['status'], 'success')
        self.assertEqual(self.c.last_frame['location'], 'board-ram')
        self.assertTrue(self.c.last_frame['path'].startswith('/tmp/sg3000-console/'))

    def test_timeout_does_not_accept_echo_or_stay_ready(self):
        self.connect(); self.c.port.mode = 'no-marker'
        with self.assertRaises(TimeoutError):
            self.c.exchange('echo test', timeout=.1)
        self.assertFalse(self.c.shell_ready)

    def test_long_command_and_invalid_port_rejected(self):
        with self.assertRaises(ValueError):
            self.c.connect('COM10;rm')
        self.connect()
        with self.assertRaises(ValueError):
            self.c.exchange('a'*1000)

    def test_disconnect_reconnect_clears_stale_status(self):
        self.connect(); self.job('probe'); self.c.disconnect(); self.c.reader.join(1)
        self.c.connect('COM99')
        self.assertFalse(self.c.identity_ok)
        self.assertEqual(self.c.checks, {})

    def test_interrupt_marks_request_and_blocks_next_command(self):
        self.connect(); self.c.busy = True
        self.c.interrupt()
        self.assertTrue(self.c.cancel.is_set())
        with self.assertRaises(InterruptedError):
            self.c.exchange('echo no')
        self.c.busy = False

    def test_replay_stays_offline_and_has_explicit_provenance(self):
        self.c.start('replay')
        deadline = time.monotonic() + 1.5
        while 'started' not in self.c.stream and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertFalse(self.c.connected)
        self.assertIsNone(self.c.port)
        self.assertEqual(self.c.stream['channel'], 'recorded-replay')
        self.c.interrupt()
        while self.c.busy:
            time.sleep(.01)
        self.assertEqual(self.c.stream['status'], 'replay-stopped')

    def test_fragmented_stream_json_updates_results(self):
        self.connect()
        self.c.busy = True
        self.c.job = {'action':'stream'}
        records=[json.loads(line[7:]) for line in (ROOT/'evidence/synthetic-stream.jsonl').read_text().splitlines()[:2]]
        for record in records:record['source']='hardware'
        records[1]['flags']=4
        records[1]['sequence']='9'
        self.c.port.queue.put(('SGJSON '+json.dumps(records[0])+'\nSGJS').encode())
        self.c.port.queue.put(('ON '+json.dumps(records[1])+'\n').encode())
        deadline = time.monotonic() + 1
        while 'result' not in self.c.stream and time.monotonic() < deadline:
            time.sleep(.01)
        self.c.busy = False
        self.assertEqual(self.c.stream['result']['sequence'], '9')
        self.assertEqual(self.c.stream['channel'], 'board')

    def test_invalid_stream_options_rejected_without_serial_write(self):
        self.connect(); self.job('probe')
        count = len(self.c.port.commands)
        for options in [{'seconds':'0; reboot'}, {'seconds':301}, {'processor':'shell'}, {'seconds':True}]:
            with self.assertRaises(ValueError):
                self.c.start('stream', options)
        self.assertEqual(len(self.c.port.commands), count)

    def test_malformed_telemetry_does_not_update_status(self):
        for text in ['SGJSON {bad}', 'SGJSON []', 'SGJSON {"schema":2,"event":"ended"}', 'not SGJSON {}']:
            self.c.accept_stream_line(text, 'board')
        self.assertEqual(self.c.stream, {})


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.console = Console(self.temp.name, FakeSerial)
        self.server = server.make_server(0, self.console)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join()
        self.temp.cleanup()

    def test_static_source_and_no_path_traversal(self):
        with urllib.request.urlopen(self.url+'/') as r:
            self.assertIn('采集中控台', r.read().decode())
            self.assertIn("frame-ancestors 'none'", r.headers['Content-Security-Policy'])
        for key, (path, *_) in server.FILES.items():
            self.assertTrue((ROOT.parent/path).is_file(), path)
            with urllib.request.urlopen(self.url+'/api/source?id='+key) as r:
                self.assertTrue(json.load(r)['text'])
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(self.url+'/api/source?id=../../secrets')
        self.assertEqual(caught.exception.code, 400)

    def test_mutation_token_origin_and_host(self):
        for headers in [{}, {'X-SG-Token':server.TOKEN, 'Origin':'https://evil.example'}, {'X-SG-Token':server.TOKEN,'Host':'evil.example'}]:
            req=urllib.request.Request(self.url+'/api/connect',data=b'{"port":"COM99"}',headers=headers)
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(req)
            self.assertEqual(caught.exception.code, 403)
            self.assertFalse(self.console.connected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
