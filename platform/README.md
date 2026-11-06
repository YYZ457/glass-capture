# platform · 板子怎么启动  ⚠️ 空

**这些东西目前只存在于 ZCU102 的 SD 卡上。SD 卡坏了，就没人知道板子是怎么启动的。**

## 要收进来的

| 内容 | 在哪找 | 备注 |
|---|---|---|
| `BOOT.BIN` | SD 卡第一个分区 | 含 FSBL、PMU 固件、bitstream（如有）、U-Boot |
| `image.ub` 或 `Image` + `system.dtb` | SD 卡第一个分区 | 内核与设备树 |
| `boot.scr` | SD 卡第一个分区 | U-Boot 启动脚本 |
| PetaLinux 工程里的 `project-spec/` | 当初建工程的电脑 | 尤其是 `meta-user/recipes-bsp/device-tree/files/system-user.dtsi` |
| 内核配置 `.config` | 板上 `/proc/config.gz` 或工程 | 内核版本 5.15.19-xilinx-v2022.1 |

## 最快的做法

1. 关机，取出 SD 卡，在电脑上把第一个分区的所有文件复制到 `platform/sdcard-boot/`
2. 在板上执行 `zcat /proc/config.gz > kernel.config`（如果存在）
3. 记录驱动构建要用的内核源码版本：`board/driver/module.sh` 里写着 `linux-xlnx b0c1be30…`

大文件（BOOT.BIN、image.ub）不要进 git，记录 SHA256 即可，原件另外备份。
