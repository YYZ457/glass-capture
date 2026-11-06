"""Bounded, single-pass P5 Mono8 source for completed SG3000 capture files."""
from pathlib import Path

class PgmStream:
    def __init__(self,path):
        self.file=Path(path).open('rb');self.started=False
        try:
            head=self.file.read(4096);pos=0;tokens=[]
            for _ in range(4):
                while pos<len(head):
                    if head[pos] in b' \t\r\n':pos+=1
                    elif head[pos]==35:
                        end=head.find(b'\n',pos)
                        if end<0:raise ValueError('unfinished comment')
                        pos=end+1
                    else:break
                start=pos
                while pos<len(head) and head[pos] not in b' \t\r\n#':pos+=1
                if pos==start or pos==len(head):raise ValueError('invalid or oversized header')
                tokens.append(head[start:pos])
            if tokens[0]!=b'P5' or tokens[3]!=b'255' or any(not t.isdigit() for t in tokens[1:3]):
                raise ValueError('P5 Mono8 maxval255 required')
            self.width,self.height=map(int,tokens[1:3])
            if not 1<=self.width<=16384 or not 1<=self.height<=65536:raise ValueError('frame dimensions out of supported range')
            if head[pos] not in b' \t\r\n':raise ValueError('missing raster delimiter')
            pos+=2 if head[pos:pos+2]==b'\r\n' else 1
            self.file.seek(0,2)
            if self.file.tell()-pos!=self.width*self.height:raise ValueError('truncated frame or trailing data')
            self.file.seek(pos)
        except BaseException:
            self.file.close();raise
    def chunks(self,chunk_bytes=65536):
        if type(chunk_bytes) is not int or not 1<=chunk_bytes<=1<<20:raise ValueError('chunk size must be 1..1048576')
        if self.started:raise RuntimeError('source already consumed')
        self.started=True;remaining=self.width*self.height
        while remaining:
            data=self.file.read(min(remaining,chunk_bytes))
            if not data:raise ValueError('file changed or truncated during read')
            remaining-=len(data);yield data
        if self.file.read(1):raise ValueError('file grew during read')
    def close(self):self.file.close()
    def __enter__(self):return self
    def __exit__(self,*args):self.close()
