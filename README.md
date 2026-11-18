# STM32F411RE UART Bootloader

一个面向 **NUCLEO-F411RE** 的串口升级项目：Bootloader 位于 Flash 前 32 KiB，应用位于其后的单槽区域。通过 ST-LINK 虚拟串口（USART2，PA2/PA3）发送应用固件，设备按包写入 Flash，验证 CRC32 与向量表后才写入有效标记并启动应用。

> 当前状态：协议与状态机可在 Linux 主机编译并通过故障注入测试；STM32 PlatformIO 工程和 HAL 端口已编写，**尚未在真实开发板编译、烧录、断电验证**。请不要将其作为已完成实机升级或回滚项目写进简历。

## Flash 布局

| 区域 | 地址 | 大小 |
| --- | --- | --- |
| Bootloader（sector 0–1） | `0x08000000` | 32 KiB |
| 升级头与预留区（sector 2 前 512 B） | `0x08008000` | 512 B |
| 应用向量表和固件 | `0x08008200` | 最大 479.5 KiB |

单槽设计：开始升级即擦除旧应用；掉电或校验失败时 Bootloader 等待重新升级，**不支持自动回滚**。应用向量表按 512 字节对齐。量产设备需增加独立备份分区或外部 Flash、签名验证、供电与写入寿命评估。CRC32 用于发现传输损坏，不提供身份认证。

## 数据协议

每帧：`'B' 'L' | cmd:u8 | seq:u16 LE | length:u16 LE | payload[0..128] | crc16:u16 LE`。`BEGIN(1)` 负载为 `image_size:u32 LE, image_crc32:u32 LE`；`DATA(2)` 序号从 0 起递增，非末包 128 字节；`END(3)` 负载为空。设备用同命令、原序号及 1 字节状态码回应。CRC16 为 Modbus 算法；整包 CRC32 为 IEEE 算法。发送端超时可重试同包；设备对上一包重复请求返回 ACK，不重复写 Flash。擦除和写入期间请勿中断供电。

## 构建与测试

```sh
python3 -m unittest discover -s tests -v
gcc -std=c11 -Wall -Wextra -Werror -Ifirmware/common firmware/common/boot.c tests/test_boot.c -o /tmp/test_boot && /tmp/test_boot
```

使用 VS Code + PlatformIO（首次构建会下载 ST STM32 平台和 STM32Cube 依赖）：

```sh
pio run -e bootloader
pio run -e application
pio run -e bootloader -t upload   # 首次用 ST-LINK 烧写 Bootloader
python3 tools/upgrade.py --port /dev/ttyACM0 --image .pio/build/application/firmware.bin
```

Windows 端口例如 `COM5`。启动工具后按提示按开发板 RESET，并在 8 秒内按回车开始升级。请**先核对 bin 的链接起始地址**为 `0x08008200`，核对板卡确为 512 KiB Flash 的 F411RE。首次上板建议由 ST-LINK 保持调试通道，观察串口日志并进行小固件验证。不要将此分区用于 F103 或其他 Flash 布局不同的板卡。

## 面试可展示的证据

主机测试、Flash 分区与 map 文件、升级前后串口日志、错误包/断线测试，以及真实板卡上的跳转录像。实机完成后再填固件大小、升级耗时和中断恢复结果。
