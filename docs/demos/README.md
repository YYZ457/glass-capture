# 可执行走读（Showboat）

这里的文档用 [Showboat](https://simonwillison.net/2026/Feb/10/showboat-and-rodney/) 生成：文字说明和真实执行的命令输出交替出现，所有输出都可以重新运行并对比，不需要板子。目的是还"理解债"——把 AI 写的代码讲清楚，并让讲解跟着代码一起被验证。

建议阅读顺序：

| 文档 | 回答的问题 |
|---|---|
| [capture_protocol.md](capture_protocol.md) | 采一帧图时，程序对采集卡按什么顺序读写了哪些寄存器？出错时怎么收场？寄存器知识有多可靠？ |
| [stream_to_host.md](stream_to_host.md) | 块进了内存之后怎么处理、怎么经网口（I6）发到电脑、落成什么文件？处理跟不上时会怎样？ |
| [board_tests.md](board_tests.md) | `make -C board test` 在电脑上证明了什么、证明不了什么？拿掉停流排空 FIFO 的修复，测试能不能抓住？ |

复验全部（仓库根目录，需要 gcc、python3 和 uv）：

    for f in capture_protocol stream_to_host board_tests; do uvx showboat verify docs/demos/$f.md --workdir board || echo "$f 有变化"; done

`verify` 报 diff 说明代码行为变了：要么是有意的修改（用 `--output` 生成新版本替换），要么是讲解已经过时、需要重写对应段落。

`src/` 里是只给这些文档用的小工具，不参与 board 构建：

- `trace_one_frame.c`：在假卡外包一层日志，打印单帧采集的每一次寄存器读写（`build_trace.sh` 编译它）
- `peek_i6.py`：逐条打印 I6 网口消息头，复用 `host/receiver/rx_tcp.py` 的解析函数
- `rx_demo.sh`：起真正的接收端，跑 5 个合成块，列出落盘文件

写新文档时注意：说明文字里的示例命令要用缩进代码块，不要用带围栏的代码块。Showboat 会把所有带语言标记的围栏块当成可执行块。
