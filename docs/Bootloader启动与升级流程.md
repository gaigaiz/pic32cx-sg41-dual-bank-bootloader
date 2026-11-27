# Bootloader 启动与升级流程

> 工程：PIC32CX1025SG41128（Cortex-M4F，1 MB 内部 Flash，256 KB RAM），双 Bank Fail-Safe UART Bootloader。
> 串口：SERCOM4，115200 8N1，TX PB12 / RX PB13。
> 本文按当前 BL 源码梳理复位启动、升级模式、OTA 写入、BankSwap 与失败回滚的完整链路；命令操作步骤见《Bootloader双Bank使用手册.md》。

## 1. 总体角色

- **BL（Bootloader）**：烧在每个 Bank 开头 8 KB 内，负责上电判断跳不跳 APP、接收串口升级、写内部 Flash、校验 CRC、交换 Bank、失败计数回滚。
- **APP**：业务固件，落在每个 Bank 的 `0x2000` 起；运行中长按 SW1 满 3 秒可写触发魔数并软复位回 BL。
- **双 Bank**：两块 512 KB 区域互为备份。活动 Bank 跑当前固件，另一块 Bank 用来放待升级固件；交换后角色对调。

## 2. Flash 分区与状态扇区

### 2.1 分区布局（内部 Flash，1 MB）

| 区域 | 活动 Bank A 物理地址 | 活动 Bank B 物理地址 | 说明 |
|---|---|---|---|
| BL 槽 | `0x000000`–`0x001FFF` | `0x080000`–`0x081FFF` | 8 KB，Bootloader 本体 |
| APP 区 | `0x002000`–`0x07DFFF` | `0x082000`–`0x0FDFFF` | 业务固件 |
| 状态扇区 | `0x07E000`–`0x07FFFF` | `0x0FE000`–`0x0FFFFF` | 本 Bank 的 CRC / 版本 / 失败计数 / 交换标志 |

- Flash 页大小 `PAGE_SIZE = 512` 字节；擦除块 `ERASE_BLOCK_SIZE = 8192` 字节。
- APP 起始地址 `APP_START_ADDRESS = 0x2000`（Bank 内偏移）。
- APP 校验区间长度 `APP_REGION_SIZE = 0x7E000 - 0x2000 = 0x7C000`。
- 非活动 Bank 起点 `INACTIVE_BANK_START = 0x80000`。

### 2.2 状态扇区布局（一个 8 KB 擦除块，只用前 16 字节）

| 偏移 | 字段 | 含义 | `0xFFFFFFFF` 处理 |
|---|---|---|---|
| `+0x00` | expected CRC | 记录的 APP 区预期 CRC32；未记录时跳过 CRC 比对 | 视为未记录 |
| `+0x04` | APP version | APP 版本号，OTA 成功后 +1 | 视为 0 |
| `+0x08` | retry_cnt | WDT 启动失败次数计数 | 视为 0 |
| `+0x0C` | bankswapped | 是否已因失败交换过 Bank（0/1） | 视为 0（未交换） |
| 其余 | 填充 | `0xFF` | — |

非活动 Bank 的状态扇区副本在 `INACTIVE_BANK_START + 0x7E000`（即 `0xFE000`）。

## 3. 复位后主流程

```
上电 / 复位
  │
  │  NVMCTRL 按 AFIRST 位选活动 Bank，映射到物理 0x0
  ▼
活动 Bank 的 BL 启动，进入 SYS_Initialize()
  │
  ├─ 初始化 NVMCTRL / PORT / CLOCK
  │
  ├─ 决策 A：bootloader_Trigger()  检测 RAM 触发魔数
  │     ├─ 命中魔数 ──────────────► 清掉 16 字节魔数，留在升级模式
  │     └─ 未命中
  │
  ├─ 决策 B：bootloader_BootCheck()  检查当前活动 Bank 的 APP 能否启动
  │     ├─ APP 区为空 / CRC 不符 / 失败次数到顶且两侧都坏 ─► 留在升级模式
  │     └─ 检查通过
  │
  ├─ WDT_Disable()，把看门狗交回给 APP 重新开启
  ▼
run_Application(0x2000)  跳进活动 Bank 的 APP
```

留在升级模式后，继续初始化 EVSYS / SYSTICK / PAC / SERCOM4，进入 `bootloader_UART_Tasks()` 串口状态机循环。

## 4. 阶段一：启动决策（BL 侧）

### 4.1 初始化顺序（`initialization.c` → `SYS_Initialize`）

1. `NVMCTRL_Initialize()`、`PORT_Initialize()`、`CLOCK_Initialize()`；
2. 先做触发判断 `bootloader_Trigger()`；
3. 未命中再做启动检查 `bootloader_BootCheck()`；
4. 检查通过：`WDT_Disable()` 后 `run_Application(APP_START_ADDRESS)`；
5. 若留在 BL：继续 `EVSYS_Initialize()`、`SYSTICK_TimerInitialize()`、`PAC_Initialize()`、`SERCOM4_USART_Initialize()`。

`main.c` 在初始化完成后串口打印 `BL READY` 和当前 APP 版本号，随后循环调用 `bootloader_UART_Tasks()`。

### 4.2 决策 A：RAM 触发魔数（`custom_trigger.c`）

BL 与 APP 约定：RAM 起始 `0x20000000` 起 16 字节（4 个 uint32）全部等于 `0x5048434D`（小端 `"MCHP"`）。

- 依次读 4 个字；任意一个不匹配 → 返回 `false`，继续走跳 APP 流程。
- 四个全部匹配 → 先把这 16 字节清 0（防止下次复位又误进升级模式），再返回 `true`，BL 留在串口升级模式。

> `bootloader_common.c` 里的 `bootloader_Trigger()` 是 `__WEAK` 恒返回 `false`；`custom_trigger.c` 强覆盖了它。该文件必须保留，删掉后 APP 无法触发回 BL。

### 4.3 决策 B：启动检查（`bootloader_BootCheck()`）

按顺序判断：

1. **APP 区是否为空**：读 `0x2000` 处 MSP，若为 `0xFFFFFFFF` → 返回 `false`，留 BL。
2. **CRC 比对**：用 DSU 对 APP 区 `[0x2000, 0x2000+APP_REGION_SIZE)` 计算 CRC32；若状态扇区记录了预期 CRC（非 `0xFFFFFFFF`）且与实测不符 → 返回 `false`，留 BL。空扇区（调试器首刷）跳过 CRC 比对。
3. **WDT 失败计数**：只有本次复位原因是 WDT 超时（`RSTC_RCAUSE` 的 WDT 位）才把 `retry_cnt + 1` 并回写状态扇区；POR、软复位不增加计数。
4. **失败阈值与回滚**：
   - `retry_cnt >= 3`（`WDT_RETRY_THRESHOLD`）且 `bankswapped == 0`：先把状态扇区写成 `retry_cnt=0`、`bankswapped=1`，再调用 `NVMCTRL_BankSwap()` 交换 Bank 并复位（本次失败只换一次）。
   - `retry_cnt >= 3` 且 `bankswapped == 1`：说明对面 Bank 也已经失败过，两侧都不可用 → 返回 `false`，留在 BL。
5. 以上都通过 → 返回 `true`，允许跳 APP。

### 4.4 跳进 APP（`run_Application(address)`）

1. 从 `address` 读 MSP（`address+0`）和复位向量（`address+4`）；
2. 若 MSP 为 `0xFFFFFFFF` → 直接返回，留在 BL；
3. 否则调用 `SYS_DeInitialize()` 释放 BL 占用的资源，设置 MSP，再 `bx reset_vector` 跳进 APP。

> 注意：`run_Application()` 本身只判断 MSP 是否为空，不做 CRC、版本比较、启动次数判断；这些在 `bootloader_BootCheck()` 里完成。

## 5. 阶段二：串口升级状态机（`bootloader_uart.c`）

留在升级模式后，`bootloader_UART_Tasks()` 循环跑三个局部任务：

```
bootloader_UART_Tasks() 循环（uartBLActive 期间）
  ├─ input_task()    收串口字节，组帧
  ├─ flash_task()    收到一整块数据且就绪后，擦除块 + 写页
  └─ command_task()  收完整包后按命令码处理
```

### 5.1 收帧（`input_task`）

- 帧结构：守卫字 4B（`0x5048434D`，线上小端 `4D 43 48 50`）+ size 4B（小端）+ 命令码 1B + payload 变长。
- 逐字节比对守卫字，不匹配就丢弃重等；凑齐头部后按 size 收 payload。
- 100 ms 无新字节则头部超时，丢弃当前半包。
- 收满一包置 `packet_received = true`，交给命令任务。

### 5.2 写 Flash（`flash_task`）

- 数据块大小 = 一个擦除块（8192 B）；实际写入字节数 = `data_size - 4`（去掉 4 字节地址）。
- 流程：`NVMCTRL_RegionUnlock(addr)` → 擦除该块（`NVMCTRL_BlockErase`）→ 按 512 B 页循环 `NVMCTRL_PageWrite`；等待 Flash 忙时继续收串口字节。

### 5.3 命令分发（`command_task`）

| 命令码 | 名称 | payload | BL 动作 | 应答 |
|---|---|---|---|---|
| `0xA0` | UNLOCK | 起始地址 4B + 长度 4B（小端） | 校验并记录本次可擦写区间 `[unlock_begin, unlock_end)` | 合法 `0x50`，非法 `0x51` |
| `0xA1` | DATA | 地址 4B + 数据块（8192 B） | 地址落在解锁区间内则缓存数据，置 `flash_data_ready` 交给写 Flash | 合法 `0x50`，越界 `0x51` |
| `0xA2` | VERIFY | CRC32 4B（小端） | 对解锁区间用 DSU 重算 CRC32 与传入值比对 | 一致 `0x53`，不符 `0x54` |
| `0xA3` | RESET | 无 | 回 OK 并等串口发完，`NVIC_SystemReset()` 普通复位，不交换 Bank | `0x50` |
| `0xA4` | BKSWAP_RESET | 无 | 回 OK 等发完 → 记录新 APP 的 CRC/版本 → `NVMCTRL_BankSwap()` | `0x50` |
| `0xA6` | READ_VERSION | 无 | 回 OK 后依次发主版本、次版本（当前 v3.7） | `0x50` + 版本两字节 |
| 其他 | 未知命令 | — | 不识别 | `0x52` |

应答码：`0x50` OK、`0x51` ERROR（地址/长度非法）、`0x52` INVALID（未知命令）、`0x53` CRC_OK、`0x54` CRC_FAIL。

UNLOCK 区间校验：起始地址按 8 KB 擦除块对齐，长度按 512 B 页对齐；`end > begin` 且 `end <= 0x100000`；后续 DATA 地址必须落在该区间内。

## 6. 阶段三：一次 OTA 升级时序

以 `btl_host.py` 自动流程为例（写入非活动 Bank 的 APP，写完交换）：

```
PC (btl_host.py)                              MCU (活动 Bank 的 BL)
│── READ_VERSION 0xA6 ───────────────────────▶│
│◀──────────────── 50 03 07 ───────────────│
│── UNLOCK 0xA0 (begin=0x82000, size) ─────▶│  记录可写区间
│◀──────────────── 50 ───────────────────────│
│── DATA 0xA1 (addr, 8KB 块) ───────────────▶│  擦除块 + 写页
│◀──────────────── 50 ───────────────────────│
│        重复 DATA 直到整个文件写完            │
│── VERIFY 0xA2 (CRC32) ────────────────────▶│  DSU 计算区间 CRC 比对
│◀──────────────── 53 (CRC_OK) ──────────────│
│── BKSWAP_RESET 0xA4（带 -s）──────────────▶│  记录 CRC/版本 → BankSwap
│◀──────────────── 50 ───────────────────────│
```

- 不带 `-s`：最后一步发 `0xA3` 普通复位，不交换 Bank（用于只刷活动 Bank 自身 APP 的场景）。
- 写非活动 Bank APP 的地址是 `0x82000`；写空白非活动 Bank 的 BL+APP 合并镜像用 `0x80000`。

### 6.1 `0xA4` 交换前的记录动作

`BKSWAP_RESET` 在真正交换前：

1. 对非活动 Bank 的 APP 区 `[INACTIVE_BANK_START + 0x2000, +APP_REGION_SIZE)` 计算 CRC32；
2. `bootloader_RecordAppCrc()` 把该 CRC、版本号 +1、`retry_cnt=0`、`bankswapped=0` 写入**非活动 Bank** 的状态扇区；
3. 然后 `NVMCTRL_BankSwap()` 翻转活动 Bank 并复位。

这样交换后，新活动 Bank 的状态扇区里已经带着新固件的预期 CRC 和清零的失败计数。

## 7. 阶段四：BankSwap 与失败回滚

### 7.1 BankSwap 动作（`NVMCTRL_BankSwap`）

向 `NVMCTRL_CTRLB` 写 `CMD_BKSWRST | CMDEX_KEY`，硬件翻转 AFIRST 位并复位；复位后原先的非活动 Bank 成为活动 Bank，其 BL 启动并跳到新 APP。

### 7.2 失败回滚规则

- 只有 WDT 超时复位才累加 `retry_cnt`；正常上电、软复位不累加。
- 同一 Bank 连续 3 次 WDT 启动失败：
  - 若还没交换过（`bankswapped=0`）：交换到对面 Bank，清计数、置交换标志；
  - 若对面也已经失败过（`bankswapped=1`）：两侧都不可用，BL 留在升级模式等待串口重刷。
- OTA 成功（`0xA4`）会把新活动 Bank 的 `retry_cnt`、`bankswapped` 清零并记录新 CRC。

### 7.3 典型链路

```
A → B：活动 A 刷非活动 B 的新 APP，CRC_OK 后 Swap → 活动 B 跑新 APP_B
B → A：回 BL（BL_B 接收），刷非活动 A 的新 APP，Swap → 活动 A 跑新 APP_A
两侧版本一致后，交替复位 / 掉电重启都应能启动。
```

## 8. APP 主动回 BL 链路（`APP/main.c`）

1. `SW1_KeyTasks()` 按键状态机：IDLE → 20 ms 消抖确认按住 → PRESSED；
2. 按键抬起时计算按住时长，达到 3000 ms 才调用 `SW1_EnterBootloader()`，短按不触发；
3. `SW1_EnterBootloader()` 往 `0x20000000` 起写 4 个 `0x5048434D`；
4. `NVIC_SystemReset()`（内部 DSB 保证 RAM 写入完成后再复位）；
5. 复位后 BL 的 `bootloader_Trigger()` 命中魔数，清 16 字节，留在串口升级模式。

## 9. 边界与已核实范围

- `run_Application()` 的有效性判断只看 APP 向量头 MSP 是否为 `0xFFFFFFFF`；CRC、版本、失败计数判断集中在 `bootloader_BootCheck()`。
- BL 不做镜像签名/加密校验；CRC 仅用于 OTA 后整段比对和启动前完整性核对。
- 配置位（`0x804000` 起）不进入 OTA 镜像，本工程未启用 Fuse Programming。
- 自动回滚只覆盖"WDT 启动失败 3 次触发一次 BankSwap"；若两侧 Bank 的 APP 都损坏，BL 停在升级模式，需串口或调试器重刷，没有第三次自动自救。
- 本文为源码级静态梳理；实际编译、上板启动与串口验证由开发者完成。

## 10. 源码文件索引（BL）

| 文件 | 职责 |
|---|---|
| `BL/config.mcc/src/main.c` | 初始化后打印版本，循环进入串口升级任务 |
| `BL/config.mcc/src/config/default/initialization.c` | `SYS_Initialize` 初始化顺序、Trigger/BootCheck/跳转决策 |
| `BL/config.mcc/src/custom_trigger.c` | 强覆盖触发魔数检测 |
| `BL/config.mcc/src/config/default/bootloader/bootloader_common.c` | `run_Application`、`BootCheck`、CRC、失败计数、Bank 记录、版本读取 |
| `BL/config.mcc/src/config/default/bootloader/bootloader_common.h` | 分区、状态扇区、阈值等常量定义 |
| `BL/config.mcc/src/config/default/bootloader/bootloader_uart.c` | 收帧、命令分发、Flash 写入状态机 |
| `BL/config.mcc/src/config/default/peripheral/nvmctrl/plib_nvmctrl.c` | `NVMCTRL_BankSwap` 等 NVM 操作 |
