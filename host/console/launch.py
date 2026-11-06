"""Reuse our local server or launch it hidden; never connect hardware automatically."""
import json
import subprocess
import sys
import time
import urllib.request
import webbrowser
import argparse
from pathlib import Path

root = Path(__file__).resolve().parents[1]
url = 'http://127.0.0.1:8767/'
VERSION = '0.4'
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def inspect_server():
    try:
        with opener.open(url + 'api/bootstrap', timeout=1) as r:
            return json.load(r)
    except Exception:
        return None


def existing():
    info = inspect_server()
    return isinstance(info, dict) and info.get('root') == str(root) and info.get('version') == VERSION


def main():
    global url
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8767)
    parser.add_argument('--no-open', action='store_true')
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        raise SystemExit('端口需要为 1024..65535。')
    url = f'http://127.0.0.1:{args.port}/'
    try:
        import serial
        import numpy
    except ImportError as exc:
        raise SystemExit('缺少中控台依赖，请用当前 Python 安装 console/requirements.txt：' + str(exc))
    info = inspect_server()
    if info and info.get('root') != str(root):
        raise SystemExit('该端口正在运行另一个中控台目录。请先关闭那个服务，或用 --port 选择其他端口。')
    if info and info.get('version') != VERSION:
        with opener.open(url + 'api/state', timeout=2) as response:
            state = json.load(response)
        if state.get('connected') or state.get('busy'):
            raise SystemExit('旧版中控台仍连接硬件或正在运行，请在旧页面结束操作并关闭服务，再重新启动。')
        request = urllib.request.Request(url+'api/shutdown', data=b'{}', headers={
            'Content-Type':'application/json', 'X-SG-Token':info['token']})
        try:
            with opener.open(request, timeout=3) as response:
                json.load(response)
        except Exception as exc:
            raise SystemExit('旧版服务尚不能关闭，请等待离线任务完成后重试：' + str(exc))
        for _ in range(30):
            if inspect_server() is None:
                break
            time.sleep(.1)
        else:
            raise SystemExit('旧版服务没有退出，请关闭旧页面服务后重试。')
    if not existing():
        (root / 'runtime').mkdir(exist_ok=True)
        with (root / 'runtime' / 'server.log').open('ab') as log:
            process = subprocess.Popen([sys.executable, '-X', 'utf8', str(root/'console'/'server.py'), '--port', str(args.port)],
                                       cwd=root, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        for _ in range(100):
            if existing():
                break
            if process.poll() is not None:
                raise SystemExit('中控台未能启动，请查看 runtime/server.log；端口可能被占用。')
            time.sleep(.15)
        else:
            raise SystemExit('启动等待超时，请查看 runtime/server.log。')
    if not args.no_open:
        webbrowser.open(url)
    print('中控台已就绪：' + url)


if __name__ == '__main__':
    main()
