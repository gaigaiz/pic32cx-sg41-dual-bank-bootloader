# PIC32CX-SG41 Fail-Safe 双 Bank Bootloader 使用手册

> 适用工程：`BL`（Bootloader）、`APP`（应用固件）
> 芯片：PIC32CX1025SG41128
> 方案：Microchip MCC Melody 原生 UART Bootloader + 硬件 NVMCTRL BKSWRST 双 Bank 安全升级
> 上位机：官方 `btl_host.py`，串口 115200 8N1
> 本文描述的是当前工程的真实配置和日常操作流程；改造背景见同目录《BL工程改造为Fail-Safe双Bank_修改说明.md》。

---

## 1. 方案一句话说明

芯片内部 Flash 分成两个 512 KB 的 Bank。每个 Bank 开头放一份相同的 Bootloader（BL_A / BL_B），后面放 APP（APP_A / APP_B）。

- 平时只运行其中一个 Bank（活动 Bank），另一个 Bank（不活动 Bank）作为升级缓冲区。
- 需要升级时，先回到活动 Bank 的 BL，把新 APP 写到不活动 Bank 的 APP 区，CRC 校验通过后执行 Bank Swap，硬件交换两个 Bank 的启动角色并复位。
- 复位后新 Bank 成为活动 Bank，BL 从该 Bank 启动并跳到新 APP。
- 旧 APP 一直保留在另一个 Bank，写入过程断电或写坏都不会破坏正在运行的旧固件。

本方案不做 APP 后台接收固件、不做 APP 写 Flash、不做复位后整段拷贝；所有擦除、写入、CRC、Bank 交换都由活动 Bank 的 BL 通过串口协议完成。

---

## 2. 硬件与引脚

### 2.1 串口（升级通道，BL 和 APP 共用 SERCOM4）

| 信号 | 芯片引脚 | SERCOM4 通道 | 说明 |
|---|---|---|---|
| TX | PB12 | PAD0 | BL / APP 发送到上位机 |
| RX | PB13 | PAD1 | 上位机发送到 BL / APP |

串口参数：115200 bps，8 数据位，无校验，1 停止位，无流控。波特率寄存器值 `63019`。

### 2.2 按键与指示灯

| 名称 | 引脚 | 方向 | 有效电平 | 说明 |
|---|---|---|---|---|
| SW1 | PD00 | 输入，内部上拉 | 按下为低电平 0，松开为高电平 1 | 长按满 3 秒、抬起时触发回 BL |
| LED1 | PC21 | 输出 | 低电平点亮，高电平熄灭 | `LED1_Clear()` 亮，`LED1_Set()` 灭 |
| LED2 | PA16 | 输出 | 低电平点亮，高电平熄灭 | `LED2_Clear()` 亮，`LED2_Set()` 灭 |

指示灯上电默认输出高电平（熄灭）。低电平有效，不要把 `Set` / `Clear` 的电平含义搞反。

### 2.3 调试器

使用板载 PKOB4（Curiosity/Starter Kits），调试器序列号以工程本地配置为准。双 Bank 成型后，日常升级走串口 `btl_host.py`，不要用调试器反复直烧覆盖整片 Flash。

---

## 3. 工程目录与关键文件

工程根目录：仓库根目录（下文命令均在根目录下以相对路径执行）

```
bootloder\
├── BL\                                   BL 工程
│   ├── .vscode\BL.mplab.json             BL 工程属性、工具链、链接选项
│   ├── config.mcc\src\
│   │   ├── main.c                        BL 主流程（保留调试 banner）
│   │   ├── custom_trigger.c              Firmware Trigger 强覆盖（必须保留）
│   │   ├── config\default\
│   │   │   ├── initialization.c          启动顺序：trigger 判断 → 跳 APP
│   │   │   ├── bootloader\
│   │   │   │   ├── bootloader_common.h   Flash 分区、命令地址、触发 RAM 定义
│   │   │   │   ├── bootloader_common.c    run_Application、CRC、弱触发函数
│   │   │   │   └── bootloader_uart.c     串口协议解析、擦写 Flash、0xA4 分支
│   │   │   ├── btl.ld                    BL 链接脚本（ROM 0x0 / 8KB，RAM 跳过前 16 字节）
│   │   │   └── peripheral\nvmctrl\plib_nvmctrl.c  NVMCTRL_BankSwap 封装
│   └── out\BL\default.elf / default.hex  BL 编译产物
│
├── APP\                                  APP 工程
│   ├── .vscode\APP.mplab.json             APP 链接宏、heap、优化等级
│   ├── config.mcc\src\main.c              APP 主程序、版本 banner、SW1 长按状态机、点灯
│   ├── config.mcc\src\config\default\PIC32CX1025SG41128.ld  APP 链接脚本
│   └── out\APP\default.elf / default.hex  APP 编译产物
│
├── Tools\
│   ├── btl_host.py                       官方上位机（只接收 raw binary）
│   ├── make_merged_bin.py                从两份 HEX 生成 bl.bin / app.bin / merged_bl_app.bin
│   ├── verify_merged_bin.py              独立回读校验三个 bin 与 HEX 一致性
│   └── inspect_hex.py                    分析 HEX 记录类型与地址区间（排查异常镜像用）
│
└── BL\out\BL\
    ├── bl.bin                            BL raw binary（8 KB，8KB 对齐填充 0xFF）
    ├── app.bin                           APP raw binary（Bank 内偏移 0x2000 成为文件偏移 0）
    └── merged_bl_app.bin                 合并镜像：偏移 0 放 BL，偏移 0x2000 放 APP
```

> 注意：`Tools\` 目录下可能残留一份旧的 `merged_bl_app.bin`，日常烧录一律以 `BL\out\BL\merged_bl_app.bin` 为准。

### 3.1 当前 BL 工程关键配置（`uart_bootloader.yml`）

| 配置项 | 值 | 含义 |
|---|---|---|
| `BTL_DUAL_BANK` | `true` | 开启 Fail-Safe Dual Bank |
| `BTL_SIZE` | `8192` | 每个 Bank 开头保留 8 KB BL |
| `BTL_TRIGGER_ENABLE` | `true` | 允许 APP 写 RAM 魔数回到 BL |
| `BTL_TRIGGER_LEN` | `16` | RAM 前 16 字节保留给触发魔数 |
| `BTL_FUSE_PROGRAM_ENABLE` | 关闭 | 本流程不通过 BL 写配置位 |
| 升级串口 | `SERCOM4_USART` | 与 2.1 节引脚一致 |
| Flash 操作外设 | `NVMCTRL` | 内部 Flash 擦除/写入 |

### 3.2 当前 APP 工程链接宏（`APP.mplab.json`）

```
ROM_ORIGIN = 0x2000      APP 向量表起点（Bank 内偏移）
ROM_LENGTH = 0x7E000     单个 Bank 内 APP 可用长度（0x2000 ~ 0x7FFFF）
RAM_ORIGIN = 0x20000010  跳过 RAM 前 16 字节触发保留区
RAM_LENGTH = 0x3FFF0     RAM 可用长度
heap-size  = 512
优化等级    = -O1
```

BL 链接脚本 `btl.ld` 对应值：`ROM_START=0x0`、`ROM_SIZE=8192`、`RAM_START=0x20000000+16`、`RAM_SIZE=0x40000-16`。

---

## 4. Flash 分区与地址模型

### 4.1 物理分区（物理地址固定，不因 Swap 改变）

芯片 Flash 共 `0x100000`（1 MB），两个 Bank 各 `0x80000`（512 KB）。Flash 页 `512` 字节，擦除块 `8192` 字节。

| 物理区域 | 起始地址 | 结束地址 | 大小 |
|---|---:|---:|---:|
| BL_A | `0x00000000` | `0x00001FFF` | `0x2000`（8 KB） |
| APP_A | `0x00002000` | `0x0007FFFF` | `0x7E000`（504 KB） |
| BL_B | `0x00080000` | `0x00081FFF` | `0x2000`（8 KB） |
| APP_B | `0x00082000` | `0x000FFFFF` | `0x7E000`（504 KB） |

```
物理 Bank A（低半区）                     物理 Bank B（高半区）
┌──────────────────────────────┐       ┌──────────────────────────────┐
│ BL_A    0x00000000 ~ 0x00001FFF│       │ BL_B    0x00080000 ~ 0x00081FFF│
│ APP_A   0x00002000 ~ 0x0007FFFF│       │ APP_B   0x00082000 ~ 0x000FFFFF│
└──────────────────────────────┘       └──────────────────────────────┘
```

### 4.2 活动 / 不活动映射（串口命令看到的地址区间）

复位后 NVMCTRL 按 `STATUS.AFIRST` 位决定哪个物理 Bank 映射到地址 `0x0`：

- `AFIRST = 1`：Bank A 映射到 `0x0`，Bank A 为活动 Bank；Bank B 为不活动 Bank。
- `AFIRST = 0`：Bank B 映射到 `0x0`，Bank B 为活动 Bank；Bank A 为不活动 Bank。

映射区间角色固定，Swap 只交换物理 Bank 落在哪一侧：

| 命令使用的映射地址 | 含义 | 与物理 Bank 的关系 |
|---|---|---|
| `0x2000` | 当前活动 Bank 自己的 APP | 活动 A → 物理 APP_A；活动 B → 物理 APP_B |
| `0x82000` | 当前不活动 Bank 的 APP | 活动 A → 物理 APP_B；活动 B → 物理 APP_A |
| `0x80000` | 当前不活动 Bank 的 BL 起点 | 活动 A → 物理 BL_B；活动 B → 物理 BL_A |

> 关键规则：`0x82000` 永远指向"当前不活动 Bank"，不是固定写物理 Bank A，也不是固定写物理 Bank B。执行命令时谁是活动 Bank，决定了这个地址最终落到哪一侧。日常 OTA 无论当前活动 A 还是 B，APP-only 升级一律用 `0x82000`，合并镜像一律用 `0x80000`。

### 4.3 活动 Bank 判定寄存器

- NVMCTRL_STATUS 地址：`0x41004012`（16 位）。
- bit4 = AFIRST：`1` = 活动 A，`0` = 活动 B。
- 调试器 Halt CPU 后在 Memory 窗口读取该地址即可。脚本打印的 `Verification success`、`Reboot Done` 不能用来判断当前活动 Bank。

### 4.4 复位向量头（上板核对用）

BL 向量头（`bl.bin` 前 8 字节，当前产物）：

```
F8 FF 03 20 09 00 00 00
```

- 第一个字 MSP = `0x2003FFF8`
- 第二个字复位向量 = `0x00000009`，Thumb 实际入口 `0x00000008`

APP 向量头（`app.bin` 前 8 字节，随每次编译变化，以最新 `mem.map` 为准）：

```
F8 FF 03 20 91 23 00 00
```

- MSP = `0x2003FFF8`
- 复位向量 = `0x2391`，Thumb 入口 `0x2390`（对应 `mem.map` 中 `Reset_Handler 0x2390`）

---

## 5. 固件产物与生成流程

### 5.1 三个 raw binary 的作用

| 产物 | 位置 | 内容 | 用途 |
|---|---|---|---|
| `bl.bin` | `BL\out\BL\bl.bin` | BL 镜像，补齐到 8 KB，尾部 `0xFF` | 调试、合并镜像组成部分 |
| `app.bin` | `BL\out\BL\app.bin` | 仅 APP，Bank 内偏移 `0x2000` 变为文件偏移 0 | 日常 APP-only OTA |
| `merged_bl_app.bin` | `BL\out\BL\merged_bl_app.bin` | 文件偏移 `0x0~0x1FFF` = BL，偏移 `0x2000` 起 = APP | 空白不活动 Bank 首次补 BL + APP |

`btl_host.py` 只接收 raw binary，不解析 Intel HEX；不要把 `default.hex` 直接传给 `-f`。

### 5.2 每次编译后必须刷新 bin（手动流程）

`make_merged_bin.py` 读取固定路径的两份 HEX，不会自动扫描最新文件、不比较时间戳。顺序不能颠倒：

1. 在 VSCode / MPLAB 中编译 BL 工程，生成 `BL\out\BL\default.hex`。
2. 编译 APP 工程，生成 `APP\out\APP\default.hex`。
3. 运行镜像生成脚本：

```
python Tools\make_merged_bin.py
```

4. 运行独立校验脚本：

```
python Tools\verify_merged_bin.py
```

校验通过标志：末尾打印 `ALL CHECKS PASSED`。脚本会核对：

- `bl.bin` 恰好 8192 字节；
- BL / APP 的 MSP 向量是否为 `0x2003FFF8`；
- 合并镜像两个槽位与 `bl.bin`、`app.bin` 逐字节一致；
- 三个 bin 与 HEX 主 Flash 段逐字节比对；
- 未使用尾部必须为 `0xFF`；
- 不会把 `0x804000` 配置位区域打包进 OTA 镜像。

### 5.3 HEX 异常排查工具

如果导出的 bin 体积异常膨胀（数 MB），先用 `inspect_hex.py` 查看 HEX 记录分布：

```
python Tools\inspect_hex.py BL\out\BL\default.hex APP\out\APP\default.hex
```

重点看是否存在 `0x804000` 配置位段或 RAM 段被错误展开。

### 5.4 工具链与 Python 环境

- 编译器：XC32 v5.10（`C:\Program Files\Microchip\xc32\v5.10\bin`）。
- 构建：VSCode MPLAB 插件自带 CMake 4.4.3 与 Ninja 1.13.2，系统 PATH 中没有也可以直接使用插件工程构建。
- Python：3.13，上位机依赖 `pyserial`，未安装时先执行：

```
python -m pip install pyserial
```

---

## 6. 编译操作

### 6.1 编译 BL 工程

1. VSCode 打开 `BL` 工程目录。
2. 选择 `default` 配置，执行构建。
3. 构建后核对 `BL\_build\BL\default\mem.map`：
   - `.vectors` 位于 `0x0`；
   - BL ROM 总量不超过 8192 字节；
   - 能找到 `NVMCTRL_BankSwap` 符号引用；
   - `custom_trigger.o` 参与链接。

### 6.2 编译 APP 工程

1. VSCode 打开 `APP` 工程目录。
2. 选择 `default` 配置，执行构建。
3. 构建后核对 `APP\_build\APP\default\mem.map`：
   - `.vectors` 位于 `0x2000`；
   - ROM 结束地址不超过 `0x7FFFF`；
   - ROM 长度对应 `0x7E000`，不能再是 `0xFE000`；
   - `.vectors` 的 VTOR 重定位已在 startup 中完成。

### 6.3 改固件时必须同步的内容

修改 APP 业务代码后：

1. 按需更新 `main.c` 顶部版本宏（`APP_VERSION_MAJOR / MINOR / PATCH`），否则串口 banner 一直是 `APP v1.0.0`，新旧固件无法从串口文字区分。
2. 重新编译 APP。
3. 重新运行 `make_merged_bin.py` 与 `verify_merged_bin.py` 刷新并校验 bin。
4. 确认 `app.bin` 时间戳晚于 `default.hex`，向量头与最新 `mem.map` 一致。

修改 BL 代码后，除重新编译 BL 外，还要评估是否需要把新 BL 通过合并镜像补写到不活动 Bank；两个 Bank 的 BL 必须保持版本一致。

---

## 7. 首次烧录（空白片）

首次上电芯片两个 Bank 都是 `0xFF`，不能直接发 Bank Swap。按下面顺序操作。

### 7.1 调试器烧入 Bank A 基线

1. VSCode 调试器连接 PKOB4，Halt CPU。
2. 烧录 BL 的 `default.hex`（落在物理 `0x0`，即 BL_A）。
3. 烧录 APP 的 `default.hex`（落在物理 `0x2000`，即 APP_A）。
4. 调试器下载配置中不要开启整片擦除（Erase All Before Program）；本阶段只是空白片首次写入，后续不要再用调试器整片覆盖。
5. 复位后确认 APP_A 按预期运行（指示灯、串口 banner）。

### 7.2 核对 BL 向量头

调试器 Memory 窗口检查：

- `0x00000000`：`F8 FF 03 20`（MSP）
- `0x00000004`：BL 复位向量（当前 `09 00 00 00`）

如果这两个地址仍是 `FF FF FF FF`，说明 BL 没有烧进去，不要继续下载 APP。

### 7.3 回到 BL，用合并镜像补齐 Bank B

1. 在运行的 APP 中长按 SW1 满 3 秒后抬起，进入升级模式（触发机制见第 9 章）。
2. 串口先发送 READ_VERSION，收到 `50 03 07` 确认 BL 在升级模式。
3. 执行合并镜像烧录（写入不活动 Bank 的 `0x80000`，写完自动 Swap）：

```
python Tools\btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x80000 -f BL\out\BL\merged_bl_app.bin
```

4. 观察脚本输出顺序：Reading Bootloader Version → Unlocking → Programming 100% → Verification success → Swapping Bank And Rebooting → Reboot Done。
5. 复位后新活动 Bank 的 APP 应正常运行。
6. 掉电重新上电，确认活动 Bank 状态保持、APP 仍能启动。

### 7.4 首次烧录完成判定

- Bank A 的 BL + APP 可独立启动；
- Bank B 的 BL + APP 可在 Swap 后独立启动；
- 两个 Bank 交替复位、掉电重启都能跑通；
- 调试器读取 `0x2000` 与 `0x82000` 向量头均为有效 APP 向量（非全 `0xFF`）。

---

## 8. 日常 APP 升级（双 Bank OTA）

### 8.1 标准流程

前提：两个 Bank 都已有有效 BL，当前运行在某个活动 Bank 的 APP 上。

第 1 步：回到 BL 升级模式

长按 SW1 满 3 秒后抬起。APP 写入 RAM 触发魔数并软复位，活动 Bank 的 BL 检测到魔数后留在串口升级模式。串口发送 READ_VERSION，收到 `50 03 07` 才算进入 BL。

第 2 步（建议执行）：确认当前活动 Bank

调试器 Halt，读 `0x41004012` 的 bit4，记录 AFIRST 状态。虽然日常命令地址与活动 Bank 无关，但记录活动侧便于事后核对写入落点。

第 3 步：写入不活动 Bank 的 APP

```
python Tools\btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x82000 -f BL\out\BL\app.bin
```

- `0x82000` = 当前不活动 Bank 的 APP 区；
- `-s` = CRC 通过后发送 `0xA4` BKSWRST，交换 Bank 并复位；
- 脚本自动把文件补齐到 8192 字节倍数，尾部填 `0xFF`。

第 4 步：验证新固件

复位后观察新活动 Bank 的 APP：

- 指示灯符合新固件预期；
- 串口 banner 版本号已更新；
- 普通复位、掉电重启均正常；
- 调试器核对新活动 Bank APP 向量头与最新 `app.bin` 一致。

### 8.2 让两个 Bank 版本保持一致

一次 `0x82000 -s` 只刷新不活动 Bank。若当前片上两个 Bank 的 APP 版本不同，需要在 A、B 之间来回各刷一轮：

1. 当前活动 A：`0x82000 -s` 写不活动 Bank B 的 APP_B，Swap 到 B，验证 APP_B 正常。
2. 再次长按 SW1 回 BL（此时由 BL_B 接收命令）。
3. 当前活动 B：再次 `0x82000 -s`，此时不活动是 Bank A，写入 APP_A，Swap 回 A，验证 APP_A 正常。
4. 调试器核对物理 `0x2000` 与 `0x82000` 向量头一致，两侧版本相同。

### 8.3 活动 Bank 本地更新（不带 Swap）

只有工厂空片首次写活动 Bank APP、或只更新活动 Bank 元数据时才使用 `0x2000`，且**不要带 `-s`**：

```
python Tools\btl_host.py -v -i COM3 -d PIC32CX_SG41 -a 0x2000 -f BL\out\BL\app.bin
```

- 不带 `-s` 时脚本最后发送 `0xA3` 普通复位，不交换 Bank；
- 双 Bank 日常升级禁止使用 `0x2000` 搭配 `-s`：那会把新固件写进正在运行的活动 Bank 自身，Swap 后启动对面旧固件，表现为现象回退。

### 8.4 命令参数说明（`btl_host.py`）

| 参数 | 含义 |
|---|---|
| `-v` | 打印详细过程 |
| `-i COM3` | 串口号，按实际替换 |
| `-d PIC32CX_SG41` | 设备型号（必须与设备表一致） |
| `-a 0x82000` | 目标起始地址，原样透传给 BL 的 UNLOCK / DATA |
| `-f <file>` | raw binary 文件路径，不支持 HEX |
| `-s` | CRC 通过后发 `0xA4` Bank Swap Reset；不加则发 `0xA3` 普通复位 |
| `-b` | 允许写 BL 保护区；`0x80000`、`0x82000` 场景不需要 |
| `-r 115200` | 波特率，默认 115200，无需改动 |

---

## 9. 回 BL 触发机制

### 9.1 RAM 触发魔数

BL 与 APP 约定：RAM 起始 `0x20000000` 开始 16 字节（4 个 uint32）写入 `0x5048434D`，然后软复位。

| RAM 地址 | 内容 |
|---|---|
| `0x20000000` | `0x5048434D` |
| `0x20000004` | `0x5048434D` |
| `0x20000008` | `0x5048434D` |
| `0x2000000C` | `0x5048434D` |

### 9.2 BL 侧检测（`custom_trigger.c`）

`custom_trigger.c` 强覆盖 MCC 的 `__WEAK bootloader_Trigger()`：

1. 依次读取 4 个魔数；
2. 任意一个不匹配，返回 `false`，BL 继续走跳 APP 流程；
3. 四个全部匹配，先清空这 16 字节为 0，再返回 `true`，BL 留在串口升级模式。

该文件必须保留；删除后弱函数恒返回 `false`，APP 将无法触发回 BL。

### 9.3 APP 侧触发（`main.c`）

`SW1_EnterBootloader()` 完成写魔数与复位；`SW1_KeyTasks()` 是按键状态机：

- 状态 `IDLE`：检测按下，进入消抖；
- 状态 `DEBOUNCE_PRESS`：20 ms 消抖确认按住，记录按下时刻；
- 状态 `PRESSED`：按键抬起时计算按住时长，达到 `3000 ms` 才调用触发函数，短按不触发。

触发后执行 `NVIC_SystemReset()`，复位后 BL 命中魔数进入升级模式。

### 9.4 BL 启动顺序（`initialization.c` → `SYS_Initialize`）

```
NVMCTRL_Initialize()
PORT_Initialize()
if (bootloader_Trigger() == false)
{
    run_Application(APP_START_ADDRESS);   // 0x2000，未命中魔数才跳转
}
CLOCK_Initialize()
EVSYS_Initialize()
SYSTICK_TimerInitialize()
PAC_Initialize()
SERCOM4_USART_Initialize()
```

`run_Application()` 唯一有效性判断：读取 `0x2000` 处 MSP，若为 `0xFFFFFFFF` 直接返回、留在 BL；否则设置 MSP 后 `bx reset_vector` 跳进 APP。BL 跳转前不做 APP 区 CRC、版本比较、启动次数判断。

---

## 10. 串口协议参考

### 10.1 帧结构

```
┌──────────┬──────────┬────────┬───────────────┐
│ guard 4B │ size 4B  │ cmd 1B │ payload 变长   │
│ 小端      │ 小端     │        │               │
└──────────┴──────────┴────────┴───────────────┘
```

- guard：`0x5048434D`，小端线上字节为 `4D 43 48 50`。
- size：紧跟 cmd 之后 payload 的字节数，小端 4 字节，不含 cmd 本身。
- cmd：命令码 1 字节。
- 协议没有帧尾 CRC；整段 Flash 的 CRC 在 VERIFY 命令中单独传递。

### 10.2 命令码

| 命令码 | 名称 | payload | 动作 |
|---|---|---|---|
| `0xA0` | UNLOCK | 起始地址 4B（小端）+ 长度 4B（小端） | 解锁本次擦写区间 |
| `0xA1` | DATA | 地址 4B（小端）+ 数据块（8192 字节） | 擦除该块并写入 Flash |
| `0xA2` | VERIFY | CRC32 4B（小端） | 对 UNLOCK 区间计算 DSU CRC32 比对 |
| `0xA3` | RESET | 无 | 普通 `NVIC_SystemReset()`，不交换 Bank |
| `0xA4` | BKSWAP_RESET | 无 | 先回 OK，等串口发完，调用 `NVMCTRL_BankSwap()` |
| `0xA5` | DEVCFG_DATA | 配置位数据 | 本工程未启用 Fuse Programming |
| `0xA6` | READ_VERSION | 无 | 回 `50` 后依次发主版本、次版本 |

### 10.3 应答码

| 应答码 | 含义 |
|---|---|
| `0x50` | OK，命令接受成功 |
| `0x51` | ERROR，UNLOCK 地址 / 长度范围非法 |
| `0x52` | INVALID，未知命令码 |
| `0x53` | CRC_OK，VERIFY 校验通过 |
| `0x54` | CRC_FAIL，VERIFY 校验失败 |

### 10.4 UNLOCK 范围校验规则

`bootloader_uart.c` 中 UNLOCK 判断：

- 起始地址按 8 KB 擦除块对齐；
- 长度按 512 字节页对齐；
- `end > begin`，且 `end <= 0x100000`；
- 只校验整片 Flash 范围，不区分当前活动 / 不活动 Bank；
- 校验通过后 `unlock_begin / unlock_end` 生效，后续 DATA 地址必须落在该区间。

### 10.5 手动 HEX 测试示例（串口助手）

串口助手设置：COM3，115200，8N1，HEX 发送、HEX 接收，不追加回车换行。

读取 BL 版本（BL 版本 v3.7，应答 `50 03 07`）：

```
发送：4D 43 48 50 00 00 00 00 A6 00 00 00 00
应答：50 03 07
```

非法 UNLOCK 地址（超过整片 Flash，应答 `51`）：

```
发送：4D 43 48 50 08 00 00 00 A0 00 00 00 00 00 00 20 00
应答：51
```

合法 UNLOCK 活动 Bank APP 区（应答 `50`）：

```
发送：4D 43 48 50 08 00 00 00 A0 00 20 00 00 00 00 02 00
应答：50
```

合法 UNLOCK 不活动 Bank APP 区（应答 `50`）：

```
发送：4D 43 48 50 08 00 00 00 A0 00 20 08 00 00 00 02 00
应答：50
```

未知命令（应答 `52`，size=0，命令码后补 4 个占位 `00`）：

```
发送：4D 43 48 50 00 00 00 00 FF 00 00 00 00
应答：52
```

> 已知状态机特性：size = 0 的命令如果只发最小 9 字节裸帧，可能得不到应答；命令码后需要补 4 个占位 `00`。官方 `btl_host.py` 组包时天然规避了这一点，手工测试时注意补齐。

### 10.6 升级完整时序（`btl_host.py` 自动完成）

```
PC                                            MCU (BL)
│── READ_VERSION 0xA6 ──────────────────────▶│
│◀────────────── 50 03 07 ──────────────────│
│── UNLOCK 0xA0 (begin,size) ──────────────▶│
│◀────────────── 50 ────────────────────────│
│── DATA 0xA1 (addr, block) ────────────────▶│  擦除块 + 写页
│◀────────────── 50 ────────────────────────│
│        重复 DATA 直到整个文件写完           │
│── VERIFY 0xA2 (CRC32) ──────────────────▶│  DSU 计算区间 CRC
│◀────────────── 53 (CRC_OK) ──────────────│
│── BKSWAP_RESET 0xA4（带 -s 时）──────────▶│  NVMCTRL_BankSwap + 复位
│◀────────────── 50 ────────────────────────│
```

不加 `-s` 时最后一步改为发送 `0xA3` 普通复位。

---

## 11. 启动与 Bank 交换过程

### 11.1 普通上电（无触发魔数）

```
复位
 → NVMCTRL 按 AFIRST 选活动 Bank，映射到 0x0
 → 活动 Bank 的 BL 启动
 → bootloader_Trigger() 未命中魔数
 → run_Application(0x2000)
 → 跳到活动 Bank 的 APP
```

### 11.2 APP 主动回 BL

```
APP 业务运行
 → 长按 SW1 满 3 秒抬起
 → 写 4 × 0x5048434D 到 0x20000000
 → NVIC_SystemReset()
 → 复位后 BL 检测到魔数，清 16 字节，留在串口升级模式
```

### 11.3 OTA 交换

```
活动 Bank BL 接收新 APP → 写入不活动 Bank 0x82000
 → VERIFY 返回 CRC_OK
 → 0xA4：NVMCTRL_CTRLB 写 CMD_BKSWRST + CMDEX_KEY
 → 硬件翻转 AFIRST 并复位
 → 复位后原先不活动 Bank 成为活动 Bank
 → 该 Bank BL 跳到新 APP
```

### 11.4 一次完整 A → B → A 循环示意（版本号仅为演示）

```
初始：活动 A，APP_A = 旧版，APP_B = 旧版
  ① 活动 A，0x82000 -s 写入 APP_B = 新版 → Swap → 活动 B，跑新版 APP_B
  ② 回 BL（BL_B 接收命令），0x82000 -s 写入 APP_A = 新版 → Swap → 活动 A，跑新版 APP_A
结果：APP_A、APP_B 都是新版，两侧可交替启动
```

---

## 12. 故障排查

### 12.1 串口完全没有应答

按顺序检查：

1. 当前是否真的在 BL 升级模式：APP 运行时不会应答 BL 命令。先长按 SW1 回 BL，再发 READ_VERSION。
2. 串口参数：115200、8N1、无校验，TX/RX 不要接反（PB12 TX、PB13 RX）。
3. BL 启动后不会主动发送 `READY` 之外的广播字符串；必须主动发 READ_VERSION 才会收到版本应答。
4. 手工发 size=0 命令时，命令码后补 4 个 `00`。
5. 对照参考工程排查时钟使能、SERCOM 时钟掩码、DPLL/DFLL 等待循环是否卡死在 BL 初始化阶段。

### 12.2 收到 `51` ERROR

- UNLOCK 起始地址没有按 8 KB 对齐；
- 长度没有按 512 字节对齐；
- 结束地址超过 `0x100000`；
- 后续 DATA 地址不在本次 UNLOCK 区间内。

### 12.3 收到 `52` INVALID

命令码不在 `0xA0~0xA6` 列表内，或帧头 / size 解析错位。重新核对 guard 与 size 字段。

### 12.4 收到 `54` CRC_FAIL

- 传输字节不完整或文件与地址不匹配；
- 目标 Flash 块此前内容异常导致区间 CRC 不一致；
- 重新发整包；不要在 CRC 失败后发送 `0xA4`。

### 12.5 刷完后现象回退到旧固件（双灯 / 旧指示灯）

典型错误：在活动 Bank 状态下使用了 `-a 0x2000 -s`。新固件写进了活动 Bank 自身的 APP，Swap 后启动对面 Bank 的旧 APP。

正确处理：

1. 回到 BL；
2. 日常升级一律使用 `-a 0x82000 -s`；
3. 刷完后确认新活动 Bank 的指示灯 / 版本号符合预期；
4. 需要两侧版本一致时，按 8.2 节来回各刷一轮。

### 12.6 脚本 success 不代表新固件已经在跑

`Verification success` 只证明本次写入区间字节与文件一致、DSU CRC 一致；`Reboot Done` 只证明 `0xA4` 命令被 BL 收到并应答。必须通过以下方式独立确认：

- 指示灯 / 串口 banner 现象；
- 读 `0x41004012` 的 AFIRST；
- 读活动 Bank APP 向量头，与最新 `app.bin` 比对；
- 掉电重启后状态保持。

### 12.7 VSCode / 调试器直烧风险

双 Bank 成型后，APP 更新走串口，不要频繁用调试器直烧：

- 若开启 Erase All Before Program，编程前会整片擦除，再只写当前 HEX 出现的段，对面 Bank 会被清空，复位后 `0x0` 为空 Flash，CPU 跑飞；
- 调试器按物理地址落点：APP 工程直烧落在物理 `0x2000`（Bank A），不会自动写到当前活动 Bank 的 `0x82000`；
- 当前工程 `mplab.json` 的 tool properties 为空，走后端默认策略，以下载日志中 `The following memory area(s) will be programmed:` 和是否出现 Chip Erase / Erase All 为准；
- 只有空白片首次烧 BL、早期单 Bank 调试才使用调试器直烧，且不要勾选整片擦除。

### 12.8 调试器报 `PC = 0xfffffffe`、无法解析调用栈

常见原因：`0x0` 地址没有有效 BL（整片擦除后只下载了 APP，或 BL 被覆盖），复位取指跳到无效地址。处理顺序：

1. 调试器 Halt，检查 `0x0` 向量头是否为 BL 向量 `F8 FF 03 20 09 00 00 00`；
2. 若为全 `0xFF`，重新烧入 BL；
3. 若向量头存在仍报错，再读取 CFSR / HFSR / LR / SP / PC 判断是否真实 HardFault 或栈污染；
4. ELF 调试符号本身存在，不要把该报错当成"没开调试符号"处理。

### 12.9 导出的 bin 体积异常膨胀

先用 `inspect_hex.py` 排查 HEX 是否包含 `0x804000` 配置位段或 RAM 段。本工程 `make_merged_bin.py` 只提取主 Flash 段、跳过配置位区域，不要直接对 ELF 做简单 `objcopy -O binary`。

---

## 13. 坏固件与回滚能力边界

### 13.1 当前工程具备的保护

1. 写入不活动 Bank 期间，活动 Bank 旧固件不被擦写；传输中断、断电只损坏不活动 Bank 的镜像。
2. Swap 前 VERIFY 失败（`54`）不会执行 Bank 交换。
3. 新活动 Bank 的 APP 向量头为空（MSP = `0xFFFFFFFF`）时，该 Bank 自己的 BL 不会跳转，留在串口升级模式，可以继续接命令回滚。

### 13.2 当前工程不具备的能力（必须知晓）

这套 MCC 双 Bank 默认工程没有实现自动回退机制：

- BL 跳 APP 前不计算 APP 区整体 CRC、不校验版本号；
- 没有启动失败次数计数器；
- 没有窗口看门狗（WDT）超时监护；
- 没有"自检成功标记页"；
- 运行期没有监督器自动触发第二次 Bank Swap；
- 没有硬件强制进 BL 的引脚入口。

因此，若新固件向量头合法，但一进入 APP 就死循环、HardFault、外设初始化锁死，CPU 跳出 BL 后不会再返回；另一个 Bank 的 BL 也不会被 CPU 自动启动。反复复位会再次跳进同一个坏 APP。

### 13.3 向量合法、APP 死机时的手动恢复

1. 调试器 Attach、Halt CPU。
2. 向 `0x20000000` 起 16 字节写入 4 个 `0x5048434D`（小端字节序列 `4D 43 48 50` 重复 4 次）。
3. 软复位。当前活动 Bank 的 BL 检测到魔数后留在升级模式，串口应能应答 READ_VERSION。
4. 用 `0x82000 -s` 把好固件写入不活动 Bank，Swap 切回旧 Bank。
5. 兜底方案才是调试器按物理地址重烧，关闭整片擦除，避免擦掉对面完好 Bank。

### 13.4 若产品需要真正自动回退，需要额外开发

后续如要实现"坏固件启动若干次后自动退回旧 Bank"，需要自行补充：启动计数器 Flash 页、WDT 窗口监护、APP 镜像 CRC / 有效标记校验、自检成功确认标记、硬件强制 BL 入口。这些不在当前工程范围内，开工前需要单独评审方案。

---

## 14. 上板验证清单

### 14.1 编译后静态检查

- [ ] BL `mem.map`：`.vectors` 在 `0x0`，ROM 不超过 8 KB。
- [ ] APP `mem.map`：`.vectors` 在 `0x2000`，结束地址不超过 `0x7FFFF`。
- [ ] `make_merged_bin.py` 执行无报错，`verify_merged_bin.py` 输出 `ALL CHECKS PASSED`。
- [ ] `app.bin` 时间戳晚于 APP `default.hex`。
- [ ] BL / APP 的 RAM 起点均为 `0x20000010`，前 16 字节保留给触发魔数。

### 14.2 BL 协议测试

- [ ] READ_VERSION 应答 `50 03 07`。
- [ ] 未知命令应答 `52`。
- [ ] 非法 UNLOCK 地址应答 `51`。
- [ ] 合法 UNLOCK `0x2000`、`0x82000` 应答 `50`。

### 14.3 基线启动测试

- [ ] 无魔数复位，BL 自动跳到活动 Bank APP。
- [ ] APP 指示灯、串口 banner 符合预期。
- [ ] 长按 SW1 满 3 秒抬起后回到 BL，READ_VERSION 应答正常。
- [ ] 普通复位（不写魔数）不会再次进入 BL。

### 14.4 双 Bank 闭环测试

- [ ] 合并镜像写入 `0x80000`，Swap 后新 Bank APP 启动。
- [ ] APP-only 写入 `0x82000`，Swap 后新 APP 启动。
- [ ] 两个 Bank 交替启动均正常。
- [ ] 掉电重新上电，AFIRST 状态保持，APP 正常启动。
- [ ] 人为制造 CRC 失败，BL 返回 `54`，不执行 Swap。
- [ ] 物理 `0x2000` 与 `0x82000` 向量头都不是全 `0xFF`。

---

## 15. 当前已知限制与挂起事项

| 编号 | 事项 | 现状 |
|---|---|---|
| 1 | BL 调试 banner | `main.c` 启动时仍打印 `BL READY\r\n`，量产前如需静默 BL 可移除；当前保留用于调试观察。 |
| 2 | SW1 长按窗口 | 当前实现满 3 秒即触发，只有 3 秒下限，没有 4 秒最长按住上限。 |
| 3 | 工程内 post-build 自动导出 bin | 尚未配置编译后自动生成 raw bin；每次编译后需手动运行 `make_merged_bin.py` 与 `verify_merged_bin.py`。 |
| 4 | APP 版本号 | 当前固定 `v1.0.0`，新旧固件串口文字相同；区分版本需改版本宏、指示灯或读取向量头。 |
| 5 | 改造说明文档旧口径 | 旧说明文档中仍残留 `0xfe000` 长度口径，以本手册 3.2 节 `0x7e000` 为准。 |
| 6 | 自动回退 | 未实现 WDT、启动计数器、镜像有效标记和自动 BankSwap，见第 13 章。 |

---

## 16. 命令速查

### 16.1 合并镜像写入不活动 Bank（BL + APP，带 Swap）

```
python Tools\btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x80000 -f BL\out\BL\merged_bl_app.bin
```

### 16.2 日常 APP-only 写入不活动 Bank（带 Swap）

```
python Tools\btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x82000 -f BL\out\BL\app.bin
```

### 16.3 活动 Bank 本地 APP 更新（不带 Swap，工厂 / 元数据场景）

```
python Tools\btl_host.py -v -i COM3 -d PIC32CX_SG41 -a 0x2000 -f BL\out\BL\app.bin
```

### 16.4 生成与校验镜像

```
python Tools\make_merged_bin.py
python Tools\verify_merged_bin.py
```

### 16.5 HEX 区间排查

```
python Tools\inspect_hex.py BL\out\BL\default.hex APP\out\APP\default.hex
```

---

## 17. 关键文件对照

| 作用 | 文件 |
|---|---|
| BL MCC 组件配置 | `BL\config.mcc\mcc\config_default\components\uart_bootloader.yml` |
| BL 分区 / 地址 / 触发定义 | `BL\config.mcc\src\config\default\bootloader\bootloader_common.h` |
| BL 协议解析与 Flash 擦写 | `BL\config.mcc\src\config\default\bootloader\bootloader_uart.c` |
| BL 跳转与 CRC | `BL\config.mcc\src\config\default\bootloader\bootloader_common.c` |
| BL 启动顺序 | `BL\config.mcc\src\config\default\initialization.c` |
| BL 链接脚本 | `BL\config.mcc\src\config\default\btl.ld` |
| Bank Swap PLIB | `BL\config.mcc\src\config\default\peripheral\nvmctrl\plib_nvmctrl.c` |
| Firmware Trigger 强覆盖 | `BL\config.mcc\src\custom_trigger.c` |
| BL 主程序 | `BL\config.mcc\src\main.c` |
| APP 链接宏 | `APP\.vscode\APP.mplab.json` |
| APP 主程序 / 按键 / 版本 | `APP\config.mcc\src\main.c` |
| 上位机脚本 | `Tools\btl_host.py` |
| 镜像生成脚本 | `Tools\make_merged_bin.py` |
| 镜像校验脚本 | `Tools\verify_merged_bin.py` |
| HEX 排查脚本 | `Tools\inspect_hex.py` |

4D 43 48 50 00 00 00 00 A6 00 00 00 00
