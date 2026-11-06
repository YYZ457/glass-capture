"""Durable launch: reuse only the same release, never restart attached hardware."""
import sys
from pathlib import Path
import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'console'))
import launch

@pytest.mark.parametrize('version,path,expected', [('0.4',str(launch.root),True),('0.3',str(launch.root),False),('0.4','other-root',False)])
def test_reuse_requires_same_directory_and_release(monkeypatch,version,path,expected):
    monkeypatch.setattr(launch,'inspect_server',lambda:dict(root=path,version=version))
    assert launch.existing() is expected

def test_old_connected_server_never_restarted(monkeypatch):
    import io
    monkeypatch.setattr(sys,'argv',['launch','--no-open'])
    monkeypatch.setattr(launch,'inspect_server',lambda:dict(root=str(launch.root),version='0.3'))
    monkeypatch.setattr(launch.opener,'open',lambda *a,**k:io.BytesIO(b'{"connected":true,"busy":false}'))
    monkeypatch.setattr(launch.subprocess,'Popen',lambda *a,**k:pytest.fail('must not start another process'))
    with pytest.raises(SystemExit,match='仍连接硬件'):
        launch.main()

def test_foreign_server_is_not_replaced(monkeypatch):
    monkeypatch.setattr(sys,'argv',['launch','--no-open'])
    monkeypatch.setattr(launch,'inspect_server',lambda:dict(root='different-folder',version='0.3'))
    monkeypatch.setattr(launch.subprocess,'Popen',lambda *a,**k:pytest.fail('must not start another process'))
    with pytest.raises(SystemExit,match='另一个中控台'):
        launch.main()
