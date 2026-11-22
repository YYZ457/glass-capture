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
            elif 'stream --help' in wire:
                body = 'sg3000-stream 0.2\n' if self.mode == 'old-stream' else 'sg3000-stream 0.2\n--save-pgm PATH --max-bytes 0\n'
            elif 'MEM_AVAIL=' in wire:
                if self.mode == 'low-tmp':
                    body = 'TMP_DF=102400\nMEM_AVAIL=3800000\n'
                elif self.mode == 'initramfs':
                    body = 'TMP_DF=0\nMEM_AVAIL=3800000\n'
                else:
                    body = 'TMP_DF=2097152\nMEM_AVAIL=3800000\n'
            elif 'df -k /mnt/sg3000-console' in wire:
                body = '16777216\n'
            elif '--save-pgm' in wire:
                m = re.search(r'--save-pgm (\S+)', wire)
                path = m.group(1) if m else '/tmp/short.pgm'
                disc = '1' if self.mode == 'bad-continuity' else '0'
                started = 'SGJSON {"schema":1,"event":"started","source":"hardware","processor":"stats","queue_depth":3,"max_frame_bytes":"50331648","continuity_verified":false}'
                saved = f'SAVED_PGM width=16384 height=10000 bytes=163840027 path={path}'
                ended = f'SGJSON {{"schema":1,"event":"ended","source":"hardware","processor":"stats","error":0,"source_error":0,"processing_error":0,"output_error":0,"stop_error":0,"cancelled":false,"received":"5","processed":"5","bytes":"163840000","id_discontinuities":"{disc}","queue_full_events":"0","queue_peak":1,"elapsed_ms":"1000"}}'
                body = f'{started}\n{saved}\n{ended}\n' if disc == '0' else f'{started}\n{ended}\n'
            elif 'sha256sum' in wire and self.mode == 'bad-hash':
                body, rc = 'sg3000-capture: FAILED\n', 1
            elif 'ls -t /tmp/sg3000-console/*.pgm' in wire:
                body = '' if self.mode == 'no-pgm' else '/tmp/sg3000-console/frame-test.pgm\n'
            elif 'SAVED_SD_BYTES=' in wire:
                if self.mode == 'bad-sd-copy':
                    body, rc = 'cp: write error\n', 1
                else:
                    body = 'SAVED_SD_BYTES=163840027\n' if 'short-' in wire else 'SAVED_SD_BYTES=32768018\n'
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

    def test_save_sd_copies_verifies_and_always_unmounts(self):
        self.connect()
        self.c.port.mode = 'no-pgm'
        self.assertEqual(self.job('save_sd')['status'], 'error')
        self.assertNotIn('mount -t vfat /dev/mmcblk0p1', '\n'.join(self.c.port.commands))

        self.c.port.mode = 'bad-sd-copy'
        self.assertEqual(self.job('save_sd')['status'], 'error')
        cmds_fail = '\n'.join(self.c.port.commands)
        self.assertIn('umount /mnt/sg3000-console', cmds_fail)

        self.c.port.commands.clear()
        self.c.port.mode = 'ready'
        self.assertEqual(self.job('save_sd')['status'], 'success')
        cmds = '\n'.join(self.c.port.commands)
        self.assertIn('mount -t vfat /dev/mmcblk0p1 /mnt/sg3000-console', cmds)
        self.assertIn('SAVED_SD_BYTES=', cmds)
        self.assertIn('sync; umount /mnt/sg3000-console', cmds)
        self.assertTrue(self.c.last_frame['sd_saved'])
        self.assertEqual(self.c.last_frame['sd_name'], 'frame-test.pgm')
        self.assertEqual(self.c.last_frame['size_bytes'], 32768018)

    def test_short_record_validates_copies_verifies_and_unmounts(self):
        self.connect(); self.job('probe')
        count = len(self.c.port.commands)
        for bad in [{'seconds': 0}, {'seconds': 61}, {'seconds': True}, {'storage': 'usb'}]:
            with self.assertRaises(ValueError):
                self.c.start('short_record', bad)
        self.assertEqual(len(self.c.port.commands), count)

        def run_short(opts):
            self.c.start('short_record', opts)
            deadline = time.monotonic() + 4
            while self.c.busy and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertFalse(self.c.busy)
            return self.c.job

        self.c.port.mode = 'old-stream'
        self.assertEqual(run_short({'seconds': 10, 'storage': 'ram_sd'})['status'], 'error')

        self.c.port.mode = 'low-tmp'
        self.assertEqual(run_short({'seconds': 20, 'storage': 'ram_sd'})['status'], 'error')

        self.c.port.mode = 'bad-continuity'
        self.c.port.commands.clear()
        self.assertEqual(run_short({'seconds': 20, 'storage': 'ram_sd'})['status'], 'error')
        self.assertNotIn('mount -t vfat /dev/mmcblk0p1', '\n'.join(self.c.port.commands))

        self.c.port.mode = 'initramfs'
        self.c.port.commands.clear()
        self.assertEqual(run_short({'seconds': 20, 'storage': 'ram_sd'})['status'], 'success')

        self.c.port.mode = 'ok'
        self.c.port.commands.clear()
        self.assertEqual(run_short({'seconds': 20, 'storage': 'ram_sd'})['status'], 'success')
        cmds = '\n'.join(self.c.port.commands)
        self.assertIn('--block-policy strict', cmds)
        self.assertIn('--save-pgm /tmp/sg3000-console/short-', cmds)
        self.assertIn('SAVED_SD_BYTES=', cmds)
        self.assertIn('sync; umount /mnt/sg3000-console', cmds)
        self.assertEqual(self.c.last_frame['width'], 16384)
        self.assertEqual(self.c.last_frame['height'], 10000)
        self.assertEqual(self.c.last_frame['location'], 'sd-card')
        self.assertTrue(self.c.last_frame['sd_saved'])

        self.c.port.mode = 'bad-continuity'
        self.c.port.commands.clear()
        self.assertEqual(run_short({'seconds': 10, 'storage': 'direct_sd'})['status'], 'error')
        self.assertIn('sync; umount /mnt/sg3000-console', '\n'.join(self.c.port.commands))

        self.c.port.mode = 'initramfs'
        self.c.port.commands.clear()
        self.assertEqual(run_short({'seconds': 20, 'storage': 'ram_sd'})['status'], 'success')

        self.c.port.mode = 'ok'
        self.c.port.commands.clear()
        self.assertEqual(run_short({'seconds': 10, 'storage': 'direct_sd'})['status'], 'success')
        cmds_dir = '\n'.join(self.c.port.commands)
        self.assertIn('--save-pgm /mnt/sg3000-console/short-', cmds_dir)
        self.assertIn('sync; umount /mnt/sg3000-console', cmds_dir)


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
            html = r.read().decode()
            self.assertIn('采集中控台', html)
            self.assertIn('id="save-sd"', html)
            self.assertIn('id="start-short"', html)
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
