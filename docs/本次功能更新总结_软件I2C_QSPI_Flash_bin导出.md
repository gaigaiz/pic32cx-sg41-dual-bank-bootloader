# 本次功能更新总结：软件 I2C 温度采集 / 外部 QSPI Flash / 编译后自动导出 raw bin

> 适用工程：PIC32CX1025SG41128（Cortex-M4F，1 MB Flash，256 KB RAM），XC32 v5.10。
> 本文三块功能都在 APP 侧；BL 只负责双 Bank 启动与升级接收。
> 文中命令码、地址、引脚、大小均来自当前源码核对；编译、上板验证由开发者自行完成。

## 0. 一览

| 功能 | 做了什么 | 关键接口 / 产物 | 源码位置（APP） |
|---|---|---|---|
| 软件 I2C 温度采集 | 用 GPIO 开漏模拟 I2C，读板载 MCP9804 温度 | PD08=SCL、PD09=SDA；7 位地址 `0x18`，温度寄存器 `0x05` | `main.c` I2C/MCP9804 段 |
| 外部 QSPI 串口 Flash | 单线 SPI 读写 SST26VF064BA，并把记录列表持久化到外部 Flash、上电重载 | PA08–PA11 QIO0–3、PB10 SCK、PB11 CS；记录区 `0x1000` 起 3 个 4 KB 扇区 | `main.c` QSPI/Record 段 |
| 编译后自动导出 raw bin | CMake 构建后调用提取脚本，从 Intel HEX 按地址区间生成可烧录 `.bin` | `bl.bin` / `app.bin` / `merged_bl_app.bin`；`user.cmake` + `Tools/make_merged_bin.py` | `APP/cmake/APP/default/user.cmake`、`Tools/` |

---

## 1. 软件 I2C 温度采集

### 1.1 硬件与接线

- 传感器：板载 MCP9804（U900），板内 I2C 设备。
- 引脚：`PD08` = SCL（MCC 引脚名 `Tem_SCL`），`PD09` = SDA（`Tem_SDA`）。
- 设备地址：7 位地址 `0x18`（A2/A1/A0 接地）；写地址 `0x30`，读地址 `0x31`。
- 寄存器：环境温度寄存器 `0x05`。
- 时序：半位宽 5 µs，约 100 kHz 标准模式。

### 1.2 实现方式

没有走 SERCOM I2C 外设，直接用 GPIO 位带（bit-bang）模拟 I2C：

- 线状态采用开漏风格：释放线（切输入 + 内部上拉）为高，拉低为输出驱动低。板上若无外部上拉，内部上拉作为兜底（初始化时给 PD08/PD09 置 `PULLEN`）。
- 时序函数：`I2C_Start()`、`I2C_Stop()`、`I2C_WriteByte()`（返回从机是否 ACK）、`I2C_ReadByte(masterAck)`（继续读回 ACK，最后一字节发 NACK）。
- 读温度流程：Start → 写写地址 → 写寄存器 `0x05` → 重复起始（repeated start）→ 写读地址 → 读 MSB（ACK）→ 读 LSB（NACK）→ Stop。
- 换算：原始 12 位温度，负数按补码处理，换算成以 0.01 ℃ 为单位的整数（`tempC100`），串口打印 `+/-X.XX C`。

### 1.3 串口命令与周期行为

- `Tem Get`：打开温度上报；打开后不再重复响应。
- `Tem End`：关闭温度上报。
- 主循环按 1 秒周期：仅在上报开启时读传感器；成功则打印温度，失败打印 `Temp: I2C ERR`。
- 若同时处于记录状态，温度样本写入记录列表（见第 2.4 节）。

### 1.4 注意点

- I2C 全程阻塞式轮询，未用中断；读不到 ACK 即判失败并 Stop 返回。
- 温度值以整数 0.01 ℃ 表示，避免浮点；负温补码在 `MCP9804_ReadTempC100()` 内处理。

---

## 2. 外部 QSPI 串口 Flash

### 2.1 硬件与规格

- 芯片：板载 SST26VF064BA（U700），64 Mbit / 8 MiB，24 位地址，256 字节一页。
- 引脚：`PA08`–`PA11` = QIO0–QIO3，`PB10` = SCK，`PB11` = CS。
- 首版按单线 SPI（1-1-1）、Mode 0 驱动，未开 Quad 四线模式。
- 定位：只用于存镜像 / 数据，MCU 不从这块外部 Flash 启动。

### 2.2 必须先解锁：上电默认块保护

SST26VF064B/BA 上电后默认处于写保护状态。**每次复位后，在做擦除 / 编程之前必须先发一次全局解锁命令 `0x98`（ULBPR, Global Block Protection Unlock）**；漏掉这一步时，擦除和页编程会被 Flash 静默忽略，表现为写不进去、回读仍是 `0xFF`。

解锁流程：先 `WREN(0x06)` 写使能，再发 `ULBPR(0x98)` 清除所有块保护位。

### 2.3 使用的命令集

| 命令码 | 名称 | 作用 |
|---|---|---|
| `0x9F` | JEDEC ID | 读厂商 / 器件 ID，链路探测 |
| `0x05` | RDSR | 读状态寄存器，取 BUSY 位（bit0） |
| `0x06` | WREN | 写使能，每次擦除 / 编程前先发 |
| `0x02` | PP | 页编程（一次不跨 256 B 页边界） |
| `0x20` | Sector Erase | 扇区擦除（24 位地址） |
| `0x03` | READ | 顺序读回数据 |
| `0x98` | ULBPR | 全局块保护解锁（见 2.2） |

操作细节：

- `QSPI_FlashWaitReady()`：轮询状态寄存器 BUSY 位，等待内部擦除 / 编程完成；等待期间喂 WDT，带超时计数，超时返回 `false` 而不是死等。
- 页编程跨页拆分：NOR Flash 一条页编程命令不能跨过 256 B 页边界，`Record_FlashWriteBuffer()` 按页边界把缓冲拆成多段分别编程。
- 自检 `QSPI_FlashSelfTest()`：读 JEDEC ID → 解锁块保护 → 擦首扇区 → 写 16 字节 → 回读比对；通过打印 `QSPI RW OK`，任一环节失败给出对应 `erase/program/read fail` 或 `RW FAIL`。

### 2.4 记录列表持久化到外部 Flash

RAM 内记录缓冲：

- 容量 `RECORD_CAPACITY = 256` 条，环形缓冲；每条记录类型（命令 / 温度）、秒级 uptime、温度值、命令字符串（24 字符）。
- 命令来源：`Start recording` / `End recording` / `Tem Get` / `Tem End`；温度样本在上报开启且记录进行中写入。

外部 Flash 存储布局：

| 项 | 值 |
|---|---|
| 记录区起始地址 | `0x001000`（扇区 0 `0x000000` 留给上电自检） |
| 扇区大小 | 4096 字节 |
| 占用扇区数 | 3 |
| 头标志 | `RECORD_MAGIC = 0x52454331`（`"REC1"`） |
| 头结构 | magic、count、保留 2 个字 |

保存（`End recording` 触发 `Record_SaveToFlash()`）：

1. 先擦除记录区 3 个扇区；
2. 写头部（magic + count）；
3. 按旧→新顺序逐条写记录条目；
4. 成功返回 `true`，串口提示 `Saved to flash`；失败提示 `Flash save fail`。

重载（上电 `Record_LoadFromFlash()`）：

1. 读头部；magic 不符或 count 超容量则直接留空返回；
2. count 为 0 返回；
3. 按 count 读回条目填进 RAM 缓冲；读失败则清空，避免半截数据。

### 2.5 启动调用顺序

`main()` 中依次：I2C 总线初始化 → QSPI 自检（ID + 16 字节回环）→ 从外部 Flash 重载上次保存的记录列表 → 开 WDT。

---

## 3. 编译后自动导出 raw bin

### 3.1 诉求

APP 编译完成后，需要一份能直接喂给 `btl_host.py` 的 raw binary（`.bin`），不用每次手动跑镜像脚本。`btl_host.py` 只收 raw binary，不解析 Intel HEX。

### 3.2 走过的两条死路（勿重复）

**死路一：裸 `objcopy -O binary` 从 ELF 全量转 bin。**
配置位段（`0x804000` 起）和 RAM 段被按虚拟地址铺开，bin 膨胀到约 8 MB。

**死路二：objcopy `--only-section` 按节名白名单过滤。**
XC32 / 链接器给碎片化节名附加了 `%nn` 后缀（如 `.text%28`、`.dinit%32`、`.rodata%90`），白名单的 `.text.*` 匹配不到第 6 位是 `%` 的节。这些节被漏出，对应位置填 `0x00`；`.vectors` 命中所以向量头看起来正常，但代码块和 RAM 初值镜像已缺失，烧录后 CRC / 校验通过、板子无现象，属静默失败。

实测被漏掉的节：

| Flash 地址 | 节名 | 大小 | 说明 |
|---|---|---|---|
| `0x2D3C` | `.text%28` | `0x2A0` | 代码块 |
| `0x3440` | `.dinit%32` | `0x0C0` | `.data` 全局变量 Flash 初值镜像（漏掉尤其致命） |
| `0x4094` | `.text%60` | `0x040` | 代码块 |
| `0x4310` | `.text%71` | `0x030` | 代码块 |
| `0x456E` | `.rodata%90` | `0x016` | 只读数据 |
| `0x4584` | `.rodata%91` | `0x014` | 只读数据 |
| `0x4620` | `.text%103` | `0x004` | 代码块 |

### 3.3 最终方案：从 HEX 按地址区间提取

构建后步骤调用已验证的脚本 `Tools/make_merged_bin.py`，从工具链生成的 Intel HEX 提取主 Flash 段：

- HEX 每条记录自带绝对地址，脚本按地址区间筛选，**不依赖节名**，因此不会漏节。
- 天然跳过 `0x804000` 配置位区和 RAM 段，避免按地址跨度铺开造成的膨胀。
- 缺地址处填 `0xFF`（NOR Flash 擦除态）。

脚本分区常量（须与 `bootloader_common.h` / APP 链接宏一致）：

| 常量 | 值 | 含义 |
|---|---|---|
| `BL_SIZE` | `0x2000` | 单 Bank 内 BL 槽大小（8192 B） |
| `APP_OFFSET` | `0x2000` | Bank 内 APP 起始偏移 |
| `BANK_SIZE` | `0x80000` | 单 Bank 大小（512 KB） |
| `ERASE_BLOCK` | `0x2000` | NVMCTRL 擦除块（8192 B） |
| `CONFIG_BASE` | `0x800000` | 配置位区（`0x804000` 起），从镜像排除 |
| `FLASH_END` | `0x100000` | 内部 Flash 结束 |

输入 / 输出：

- 输入：`BL/out/BL/default.hex`、`APP/out/APP/default.hex`。
- 输出（`BL/out/BL/`）：`bl.bin`（8192 B）、`app.bin`、`merged_bl_app.bin`。

### 3.4 CMake 接线（user.cmake）

`user.cmake` 放在生成的 `CMakeLists.txt` 同目录（`APP/cmake/APP/default/`）；生成文件在 include `main.cmake` 之后自动 include 它，**不改 MCC 生成文件**。接线要点：

1. 用 `get_property(... BUILDSYSTEM_TARGETS)` 取目标列表，按前缀 `^APP_default_image_` 匹配 ELF 目标，不硬编码 MCC 生成的哈希后缀。
2. 按目录层级定位仓库根与 `Tools/make_merged_bin.py`。
3. `find_program(PYTHON_EXECUTABLE NAMES python python3 py)` 找 Python。
4. 找不到 Python 或脚本时 `message(WARNING)` 告警并跳过，不中断 CMake 配置。
5. 定义 ALL 自定义导出目标，`DEPENDS` + `add_dependencies` 挂到现有 hex 目标 `APP_default_Bin2Hex` 上，保证**先生成 `.hex`，再跑提取**；这个顺序必须显式声明，并行构建不会默认保证。

### 3.5 产物与校验

当前产物核对结果：

- `bl.bin`：8192 字节，BL 向量 MSP=`0x2003FFF8`、复位向量=`0x00000009`。
- `app.bin`：9770 字节，APP 程序区间 `0x2000`–`0x462A`，向量 MSP=`0x2003FFF8`、复位向量=`0x00003821`（Thumb 入口 `0x00003820`）。
- `merged_bl_app.bin`：24576 字节（`0x6000`）。
- `Tools/verify_merged_bin.py` 独立回读三份 bin 与 HEX 主 Flash 逐字节比对，通过标志 `ALL CHECKS PASSED`。

### 3.6 迁移到其他项目

通用移植步骤、需核对参数表、单 APP 项目简化方式，见同目录《raw-bin自动导出方案_移植指南.md》；本项目改动的来龙去脉见《APP编译后自动导出raw-bin_修改说明.md》。要点：只搬 `user.cmake` 不够，提取脚本本体必须一并拷贝；环境、分区不变时接线写法可照搬。

---

## 4. 边界与备注

- 三块功能均落在 APP 工程；BL 不参与 I2C 采集和外部 Flash 数据存储。
- 外部 QSPI Flash 仅作数据 / 镜像存储，不作为 MCU 启动源。
- 配置位（`0x804000`）不进入 OTA 镜像，与 `BTL_FUSE_PROGRAM_ENABLE` 未勾选的现状一致。
- 本文为源码级静态总结；CMake 重新配置、编译、上板观察由开发者自行完成。
- 器件命令与时序以 SST26VF064BA、MCP9804 各自数据手册为准；板级引脚分配以本工程 README 引脚表为准。
