#!/bin/sh
# 在 board/ 目录下运行：起真正的接收端 rx_tcp.py，跑 5 个合成块，列出落盘结果后清理。
set -e
R=$(mktemp -d)
python3 -I ../host/receiver/rx_tcp.py --port 18772 --out "$R" > /dev/null 2>&1 &
P=$!
sleep 0.5
out/sg3000-stream --source synthetic --processor candidates --max-blocks 5 \
    --synthetic-delay-ms 10 --tcp 127.0.0.1:18772 --tcp-frame-every 2 > /dev/null
sleep 0.5
kill $P
cd "$R"
find . -type f | sort
echo; echo "== candidates/0000000002_2.json =="; cat candidates/0000000002_2.json
echo; echo "== frames/0000000003_2.pgm 的文件头 =="; head -c 15 frames/0000000003_2.pgm | od -An -c
cd / && rm -rf "$R"
