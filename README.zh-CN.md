[English](README.md) | 中文

# PIC32CX-SG41 双 Bank Fail-Safe 看门狗回滚 Bootloader

面向 **PIC32CX1025SG41128**（Cortex-M4，1 MB Flash，256 KB RAM）的串口
UART Bootloader，支持硬件双 Bank（A/B）OTA、APP CRC 校验、版本记录，以及看门狗
（WDT）监护下的自动回滚。

## 1. 项目解决的问题

通过串口升级 MCU 固件时，如果写入中途掉电，或者新固件一启动就跑飞，设备就可能
变砖。本工程保留两块相互独立的 Flash Bank：活动 Bank 运行当前固件，另一块 Bank 作为
OTA 暂存区。新 APP 先写入非活动 Bank 并做 CRC 校验，确认无误后再做硬件 Bank 互换；
如果新 APP 反复崩溃，看门狗监护会自动回滚；两块 Bank 都失败时，设备停留在 Bootloader
升级模式等待重新烧录。

## 2. 主要功能

- **MCC Melody UART Bootloader**：走 SERCOM4，115200 8N1（TX PB12 / PAD0，
  RX PB13 / PAD1）。
- **硬件双 Bank**：通过 `NVMCTRL_BankSwap()`（BKSWRST）实现；复位后由硬件交换两块
  物理 Bank 的活动/非活动角色。
- **APP 区 CRC 校验**：跳转到 APP 之前，用 DSU CRC32 对 APP 区整体校验。
- **版本记录**：BL 记录 APP 版本号；OTA 成功后版本号加一，进入 BL 时打印 `APP Vx.0`。
- **WDT 监护回滚**：
  - BL 跳 APP 前关闭 WDT；由 APP 自己打开 WDT 并自己喂狗。
  - APP 运行稳定后写入成功标记，清除重试计数器。
  - 只有 **WDT 超时复位** 才会让重试计数器加一；POR 上电复位和 NVIC 软件复位不计数。
  - 连续 3 次 WDT 失败后，BL 执行一次 BankSwap；如果换过去的 Bank 也失败 3 次，
    BL 停留在升级模式（两块 Bank 都不可用）。
- **返回 Bootloader**：长按 SW1（PD00）3 秒；APP 写入 RAM 触发魔术字后复位，
  BL 检测到该魔术字后停留在升级模式。

## 3. 硬件与 Flash 布局

### 物理分区（固定地址，互换后不变）

| 区域 | 起始地址 | 结束地址 | 大小 |
|---|---:|---:|---:|
| BL_A | `0x00000000` | `0x00001FFF` | `0x2000`（8 KB） |
| APP_A | `0x00002000` | `0x0007FFFF` | `0x7E000`（504 KB） |
| BL_B | `0x00080000` | `0x00081FFF` | `0x2000`（8 KB） |
| APP_B | `0x00082000` | `0x000FFFFF` | `0x7E000`（504 KB） |

Flash 页大小 = 512 字节，擦除块大小 = 8192 字节。

### 状态扇区（活动 Bank 映射地址 `0x7E000`，8 KB 块）

| 偏移 | 字段 |
|---:|---|
| `0x00` | 期望的 APP CRC32（`0xFFFFFFFF` = 未记录） |
| `0x04` | APP 版本号（`0xFFFFFFFF` = 按 0 处理） |
| `0x08` | WDT 重试计数器（`0xFFFFFFFF` = 按 0 处理） |
| `0x0C` | Bank 互换标志（`0` = 未互换，`1` = 已互换一次） |

### 引脚

| 名称 | 引脚 | 方向 / 有效电平 |
|---|---|---|
| SW1 | PD00 | 输入，内部上拉，低电平有效 |
| LED1 | PC21 | 输出，低电平点亮 |
| LED2 | PA16 | 输出，低电平点亮 |

## 4. 目录结构

```
.
├── BL/                       BL Bootloader 工程
│   ├── .vscode/BL.mplab.json BL 工具链 / 链接选项
│   └── config.mcc/src/
│       ├── main.c
│       ├── custom_trigger.c   固件触发覆盖（必须保留）
│       └── config/default/
│           ├── initialization.c
│           ├── bootloader/
│           │   ├── bootloader_common.h
│           │   ├── bootloader_common.c
│           │   └── bootloader_uart.c
│           └── peripheral/nvmctrl/plib_nvmctrl.c
├── APP/                      APP 应用工程
│   ├── .vscode/APP.mplab.json APP 链接宏 / 堆 / 优化等级
│   └── config.mcc/src/main.c
├── Tools/
│   ├── btl_host.py           官方上位机（只接收 raw binary）
│   ├── make_merged_bin.py    由 HEX 生成 bl.bin / app.bin / merged_bl_app.bin
│   ├── verify_merged_bin.py  对生成的 bin 做独立字节 / CRC 校验
│   └── inspect_hex.py        查看 HEX 记录类型与地址范围
└── docs/                     使用手册与改造说明
```

## 5. 构建前提

- **工具链**：XC32 v5.10，MPLAB VSCode 插件（自带 CMake + Ninja）。
- **调试器**：板载 PKOB4。
- **Python 3**：用于上位机工具和镜像脚本。
- 用 MCC 重新生成代码后，需把 `APP.mplab.json` 的 `ROM_LENGTH` 改回
  `0x7C000`，以保护状态扇区（MCC 生成时会把它重置为 `0xFE000`）。

## 6. 使用方法

### 6.1 编译镜像

1. 在 VSCode/MPLAB 中编译 BL 工程，生成 `BL/out/BL/default.hex`。
2. 编译 APP 工程，生成 `APP/out/APP/default.hex`。
3. 生成 raw binary：

```
python Tools/make_merged_bin.py
```

4. 独立校验生成的镜像：

```
python Tools/verify_merged_bin.py
```

脚本打印 `ALL CHECKS PASSED` 即校验通过。

### 6.2 串口升级

`btl_host.py` 只接收 **raw binary**（不解析 Intel HEX）。

只升级 APP，写入非活动 Bank：

```
python Tools/btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x82000 -f BL/out/BL/app.bin
```

向空白的非活动 Bank 烧录 BL + APP 合并镜像：

```
python Tools/btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x80000 -f BL/out/BL/merged_bl_app.bin
```

- `-a 0x82000` = 非活动 Bank 的 APP 区（始终指向非活动 Bank）。
- `-a 0x80000` = 非活动 Bank 的 BL 起点，用于 BL+APP 合并镜像。
- `-s` = CRC 通过后发送 `0xA4` BankSwap 复位；不加该参数则发送普通的 `0xA3` 复位。

### 6.3 串口协议

帧结构：守卫字 `0x5048434D`（小端 `4D 43 48 50`），4 字节 size，1 字节命令码，
后跟 payload。帧尾不带 CRC；整片区域的 CRC 在 VERIFY 命令中传入。

| 命令码 | 名称 | Payload | 动作 |
|---:|---|---|---|
| `0xA0` | UNLOCK | 起始地址 4B 小端 + 长度 4B 小端 | 解锁擦除 / 写入范围 |
| `0xA1` | DATA | 地址 4B 小端 + 数据块（8192 B） | 擦除块并写入 Flash |
| `0xA2` | VERIFY | CRC32 4B 小端 | 在已解锁范围内做 DSU CRC32 比对 |
| `0xA3` | RESET | 无 | 普通 `NVIC_SystemReset()`，不互换 Bank |
| `0xA4` | BKSWAP_RESET | 无 | 先回 OK，再执行 `NVMCTRL_BankSwap()` |
| `0xA5` | DEVCFG_DATA | 配置位数据 | 本工程未使能熔丝编程 |
| `0xA6` | READ_VERSION | 无 | 先回 `50`，再回主版本号、次版本号 |

| 应答码 | 含义 |
|---:|---|
| `0x50` | OK，命令已接收 |
| `0x51` | ERROR，UNLOCK 地址 / 长度非法 |
| `0x52` | INVALID，未知命令码 |
| `0x53` | CRC_OK，VERIFY 通过 |
| `0x54` | CRC_FAIL，VERIFY 失败 |

### 6.4 示例

读取 BL 版本（size = 0；命令码后需补 4 个 `00` 字节）：

```
发送:  4D 43 48 50 00 00 00 00 A6 00 00 00 00
应答:  50 03 07
```

未知命令：

```
发送:  4D 43 48 50 00 00 00 00 AB 00 00 00 00
应答:  52
```

非法 UNLOCK 地址：

```
发送:  4D 43 48 50 08 00 00 00 A0 00 20 08 00 00 00 00 00
应答:  51
```

## 7. WDT 回滚行为（仅活动 Bank 有固件，非活动 Bank 为空）

| WDT 复位次数 | 行为 |
|---:|---|
| 第 1 / 2 次 | retry_cnt = 1 / 2，跳回当前 Bank 的 APP |
| 第 3 次 | retry_cnt = 3，BankSwap 切到另一块 Bank，retry_cnt 清零 |
| 互换之后 | 如果另一块 Bank 的 APP 区为空，BL 检测到空 Flash，停留在升级模式 |

## 8. 注意事项

- BL 不会主动广播 `READY` 横幅；需主动发送 READ_VERSION 命令确认 BL 已进入升级模式。
- size 为 0 的命令如果只发裸 9 字节帧，可能没有应答；命令码后要补 4 个 `00` 字节
  （上位机工具会自动处理）。
- OTA 日志里出现 `Verification success` / `Reboot Done` 后，不要仅凭这两句判断
  当前活动 Bank；应在调试器内存窗口读 `NVMCTRL_STATUS.AFIRST` 确认当前是哪块 Bank。

## 9. 相关文档

- `docs/Bootloader双Bank使用手册.md` — 完整使用手册（中文）。
- `docs/BL工程改造为Fail-Safe双Bank_修改说明.md` — BL 双 Bank 改造说明（中文）。

## 10. 许可证

基于 [MIT 许可证](LICENSE) 发布。
