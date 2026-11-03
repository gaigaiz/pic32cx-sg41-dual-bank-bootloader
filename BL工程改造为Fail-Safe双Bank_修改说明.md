# BL 工程改造为 Fail-Safe Dual Bank（双 Bank 安全升级）修改说明

## 1. 目标方案

本方案将当前单槽直接覆盖式 UART Bootloader，改造为 Microchip MCC 原生的 Fail-Safe Update / Dual Bank Bootloader。

改造后的升级模型：

1. 芯片复位后，BL 先运行在当前活动 Bank 的起始地址。
2. 没有 Firmware Trigger 时，BL 跳转到当前活动 Bank 的 APP。
3. APP 需要升级时，只写 RAM 触发字并复位，让 CPU 回到 BL。
4. BL 接管 UART，把新固件写入另一个不活动 Bank。
5. CRC 校验通过后，上位机发送 Bank Swap Reset 命令。
6. BL 调用 NVMCTRL Bank Swap，硬件完成 Bank 交换并复位。
7. 复位后新的 Bank 成为活动 Bank，BL 从中跳转到新 APP。

本方案**不做**以下设计：

- 不实现 APP 运行时后台通过 UART 持续接收固件。
- 不实现 APP 自己写备份区。
- 不实现复位后由 BL 把备份区整段 Flash 拷贝到运行区。
- 不新增自定义帧协议、自定义 ACK/NACK、自定义 CRC 或自定义槽位标志。
- 不使用旧工程遗留的 `0x3000` 地址口径。

---

## 2. 当前工程基线（改造前）

### 2.1 当前 BL 工程现状

当前 BL 工程位置：`BL/`

已确认的现状：

- 串口：`SERCOM4_USART`，115200、8N1、无校验。
- Flash 操作外设：`NVMCTRL`。
- Firmware Trigger：已使能，RAM 触发区 16 字节。
- BL 大小：`8192` 字节。
- 当前 APP 跳转地址：`0x2000`。
- `custom_trigger.c` 已强覆盖 MCC 弱函数 `bootloader_Trigger()`，必须保留。
- PLIB 中已经存在 `NVMCTRL_BankSwap()`，但当前 `bootloader_uart.c` 没有调用它。
- 当前 `bootloader_uart.c` 只处理以下 UART 命令：
  - `0xA0` UNLOCK
  - `0xA1` DATA
  - `0xA2` VERIFY
  - `0xA3` RESET
  - `0xA6` READ_VERSION
- 收到未知命令时回复 `0x52 INVALID`，当前没有 `0xA4 BKSWAP_RESET` 分支。

### 2.2 当前 APP 工程现状

当前 APP 工程位置：`APP/`

- 当前链接宏：
  - `ROM_ORIGIN=0x2000`
  - `ROM_LENGTH=0xfe000`
- `0xfe000` 会覆盖整个 1 MB Flash，跨到第二个 Bank，双 Bank 方案中必须缩小到单个 APP Bank 的可用长度。
- APP 当前没有 Flash 擦除/写入业务代码，这一点符合本方案，不需要新增 APP 后台收包逻辑。

### 2.3 上位机现状

上位机脚本位置：`Tools/btl_host.py`

- 设备表中已经包含 `PIC32CX_SG41`。
- 该设备已被列为支持 Bank Swap。
- 脚本已经定义：
  - `BL_CMD_BKSWAP_RESET = 0xA4`
- 传入 `-s / --swap` 时，CRC 校验完成后发送 `0xA4`；不传 `-s` 时发送普通复位命令 `0xA3`。

---

## 3. 目标 Flash 分区

芯片 Flash 总大小：`0x100000`（1 MB）。

双 Bank 各占 `0x80000`（512 KB）。每个 Bank 开头保留 8 KB BL，剩余空间给 APP。

### 3.1 Bank A（默认初始活动 Bank）

| 区域 | 起始地址 | 结束地址 | 大小 |
|---|---:|---:|---:|
| Bank A BL | `0x00000000` | `0x00001FFF` | `0x2000`（8 KB） |
| Bank A APP | `0x00002000` | `0x0007FFFF` | `0x7E000`（504 KB） |

### 3.2 Bank B（初始不活动 Bank）

| 区域 | 起始地址 | 结束地址 | 大小 |
|---|---:|---:|---:|
| Bank B BL | `0x00080000` | `0x00081FFF` | `0x2000`（8 KB） |
| Bank B APP | `0x00082000` | `0x000FFFFF` | `0x7E000`（504 KB） |

### 3.3 分区对齐核对

- Flash 页大小：`512` 字节。
- Flash 擦除块大小：`8192` 字节。
- BL 大小：`8192` 字节，按擦除块对齐。
- 活动 APP 起点：`0x2000`，按擦除块对齐。
- 不活动 Bank BL 起点：`0x80000`，按擦除块对齐。
- 不活动 Bank APP 起点：`0x82000`，按擦除块对齐。

---

## 4. BL 工程 MCC 配置修改

### 4.1 打开 BL 工程的 uart_bootloader 组件

在 BL 工程的 MCC 配置界面中，找到 `uart_bootloader` 组件，按下表修改配置。

| 配置项 | 目标值 | 说明 |
|---|---|---|
| `Use Dual Bank For Safe Flash Update`（`BTL_DUAL_BANK`） | 勾选 / `true` | 开启 Fail-Safe Dual Bank |
| `Bootloader Memory Used start address`（`BTL_MEM_START_ADDR`） | `0` | BL 从 Bank 本地地址 0 开始 |
| `Bootloader Size (Bytes)`（`BTL_SIZE`） | `8192` | 每个 Bank 开头保留 8 KB BL |
| `Application Start Address (Hex)`（`APP_START_ADDRESS`） | `0x2000` | 活动 Bank APP 跳转地址 |
| `Enable Bootloader Trigger From Firmware`（`BTL_TRIGGER_ENABLE`） | `true` | 允许 APP 写 RAM 触发字后回到 BL |
| `Number Of Bytes To Reserve From Start Of RAM`（`BTL_TRIGGER_LEN`） | `16` | RAM 前 16 字节留给触发魔数 |
| `Bootloader NVM Memory Used`（`MEM_USED`） | `NVMCTRL` | 使用芯片内部 Flash 控制器 |
| `Bootloader Peripheral Used`（`PERIPH_USED`） | `SERCOM4_USART` | 保持当前升级串口 |
| `Enable Fuse Programming`（`BTL_FUSE_PROGRAM_ENABLE`） | 保持关闭 | 当前不需要通过 BL 修改配置位 |

### 4.2 保持当前已正确的依赖

Project Graph 中应保持以下关系：

- `uart_bootloader` 依赖 `nvmctrl`。
- `uart_bootloader` 依赖 `sercom4`。
- `dsu` 继续用于 CRC 校验。
- `core`、`cmsis`、`evsys`、`port`、`clock`、`nvic`、`systick` 保持当前生成状态。

不需要新增自定义 Flash 驱动，也不需要新增自定义 OTA 组件。

### 4.3 执行 Generate

完成上述 MCC 配置后执行 Generate，让 MCC 重新生成 BL 源码和链接脚本。

> 注意：`bootloader_uart.c`、`bootloader_common.h`、`btl.ld` 属于 MCC 生成文件。优先以 MCC 生成结果为准；不要在未备份的情况下长期手工修改生成文件，后续再次 Generate 可能覆盖手工改动。

---

## 5. Generate 后必须复核的 BL 文件

### 5.1 复核 `uart_bootloader.yml`

文件：`BL/config.mcc/mcc/config_default/components/uart_bootloader.yml`

确认 `BTL_DUAL_BANK` 不再是默认关闭状态，而是保存为 `true`。

### 5.2 复核 `bootloader_common.h`

文件：`BL/config.mcc/src/config/default/bootloader/bootloader_common.h`

目标值如下：

```c
#define FLASH_START                             (0UL)
#define FLASH_LENGTH                            (1048576UL)
#define PAGE_SIZE                               (512UL)
#define ERASE_BLOCK_SIZE                        (8192UL)

#define BOOTLOADER_SIZE                         8192
#define APP_START_ADDRESS                       (0x2000U)

#define BTL_TRIGGER_RAM_START                   0x20000000
#define BTL_TRIGGER_LEN                         16
```

允许 MCC 根据生成结果调整格式，但地址、大小、页大小和擦除块大小不能改错。

### 5.3 复核 `bootloader_uart.c`

文件：`BL/config.mcc/src/config/default/bootloader/bootloader_uart.c`

改造完成后，除当前已有命令外，应存在 Bank Swap Reset 命令：

- 命令码：`0xA4`
- 命令名：`BL_CMD_BKSWAP_RESET`
- 处理动作：
  1. 先回复 `0x50 OK`。
  2. 等待串口发送完成。
  3. 确认 NVMCTRL 空闲。
  4. 调用 `NVMCTRL_BankSwap()`。
  5. 等待硬件复位，不允许再回到命令循环。

该分支**不能**调用普通 `bootloader_TriggerReset()`；普通复位对应 `0xA3`，不会交换 Bank。

### 5.4 复核 `btl.ld`

文件：`BL/config.mcc/src/config/default/btl.ld`

BL 自己只占用当前活动 Bank 的前 8 KB，目标值：

```c
#define ROM_START 0x0
#define ROM_SIZE  8192
#define RAM_START (0x20000000 + 16)
#define RAM_SIZE  (0x40000 - 16)
```

### 5.5 复核 `NVMCTRL_BankSwap()`

文件：`BL/config.mcc/src/config/default/peripheral/nvmctrl/plib_nvmctrl.c`

当前 PLIB 已有：

```c
void NVMCTRL_BankSwap(void)
{
    NVMCTRL_REGS->NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMD_BKSWRST | NVMCTRL_CTRLB_CMDEX_KEY;
}
```

该封装不需要改为普通系统复位。

### 5.6 保留 `custom_trigger.c`

文件：`BL/config.mcc/src/custom_trigger.c`

该文件仍然需要保留，用来覆盖 MCC 弱函数：

- 检测 RAM 前 16 字节是否为 `0x5048434D`。
- 检测成功后清除触发区。
- 返回 `true`，让 BL 进入升级模式。

不要删除这个文件，否则 MCC 弱函数可能仍然返回 `false`，导致 APP 无法触发回 BL。

---

## 6. MCC Generate 后仍缺少 `0xA4` 命令分支时的补丁

### 6.1 先判断是否需要补丁

Generate 后打开 `bootloader_uart.c`，先搜索：

```c
BL_CMD_BKSWAP_RESET
```

如果已经存在对应命令分支和 `NVMCTRL_BankSwap()` 调用，不需要手工补丁。

如果不存在，再使用下面的最小补丁。补丁完成后要记录：这是 MCC 未自动生成时的手工补充，下次 Generate 后必须重新复核。

### 6.2 增加命令码定义

在现有命令码附近增加：

```c
#define    BL_CMD_UNLOCK            0xa0U
#define    BL_CMD_DATA              0xa1U
#define    BL_CMD_VERIFY            0xa2U
#define    BL_CMD_RESET             0xa3U
#define    BL_CMD_BKSWAP_RESET      0xa4U
#define    BL_CMD_READ_VERSION      0xa6U
```

### 6.3 增加 Bank Swap 命令分支

在 `BL_CMD_RESET` 分支之后、默认 `0x52 INVALID` 分支之前加入：

```c
else if (BL_CMD_BKSWAP_RESET == input_command)
{
    SERCOM4_USART_WriteByte(BL_RESP_OK);

    while (SERCOM4_USART_TransmitComplete() == false)
    {
        /* 等待上位机收到 OK 应答 */
    }

    while (NVMCTRL_IsBusy() == true)
    {
        /* 等待 Flash 操作结束 */
    }

    NVMCTRL_BankSwap();

    while (true)
    {
        /* BKSWRST 会触发硬件复位；这里不能返回命令循环。 */
    }
}
```

### 6.4 补丁后检查点

- `0xA3` 分支仍然调用 `bootloader_TriggerReset()`，只做普通复位。
- `0xA4` 分支必须调用 `NVMCTRL_BankSwap()`。
- `0xA4` 分支不能擦写 BL 区域。
- `0xA4` 分支不能直接跳转到 `0x2000`。
- 回复 `0x50 OK` 必须放在 Bank Swap 之前，保证上位机能够收到确认。

---

## 7. APP 工程必须同步修改的内容

### 7.1 修改 APP 链接长度

文件：`APP/.vscode/APP.mplab.json`

当前值：

```json
"linker.preprocessor-macros": [
    "ROM_LENGTH=0xfe000",
    "ROM_ORIGIN=0x2000"
]
```

目标值：

```json
"linker.preprocessor-macros": [
    "ROM_ORIGIN=0x2000",
    "ROM_LENGTH=0x7e000"
]
```

原因：单个 Bank 的 APP 可用范围是 `0x2000 ~ 0x7FFFF`，长度为 `0x7E000`。`0xfe000` 会让 APP 链接范围跨过 Bank 边界，双 Bank 方案中不正确。

### 7.2 APP 不新增 Flash 后台接收逻辑

本方案中 APP 只负责：

1. 正常执行业务。
2. 用户命令或业务条件需要升级时，写 RAM 触发字。
3. 调用软复位回到 BL。

APP 不需要：

- 解析 UART 固件帧。
- 擦除 Flash。
- 写不活动 Bank。
- 记录版本号或槽位状态。
- 在复位后等待 BL 拷贝固件。

### 7.3 APP 回 BL 触发代码

需要升级时，APP 写入 RAM 前 16 字节后复位：

```c
#include <stdint.h>
#include "definitions.h"

static void App_EnterBootloader(void)
{
    volatile uint32_t *ram = (volatile uint32_t *)0x20000000UL;

    ram[0] = 0x5048434DU;
    ram[1] = 0x5048434DU;
    ram[2] = 0x5048434DU;
    ram[3] = 0x5048434DU;

    NVIC_SystemReset();
}
```

调用位置由 APP 业务决定，例如按键、串口命令或升级请求处理函数中。

### 7.4 APP 输出 raw binary

上位机 `btl_host.py` 只接收 raw binary，不解析 Intel HEX。

APP 构建完成后，需要从 ELF 生成 `.bin`：

```bash
xc32-objcopy -Obinary out/APP/default.elf app.bin
```

如果命令行工具路径由插件管理，优先使用当前工程已配置的 XC32 工具链，不要混用其他编译器版本。

### 7.5 APP 大小检查

构建完成后检查 APP 的 `mem.map`：

- `.vectors` 起点必须是 `0x2000`。
- ROM 结束地址不能超过 `0x7FFFF`。
- APP 镜像长度不能超过 `0x7E000`。

---

## 8. 上位机烧录命令

### 8.1 命令参数约定

- 串口：替换命令中的 `COMx`。
- 设备名：`PIC32CX_SG41`。
- 波特率：保持当前 BL/APP 一致的 115200、8N1、无校验。

### 8.2 普通覆盖活动 Bank APP（不交换 Bank）

不使用 `-s`，上位机写完后发送 `0xA3` 普通复位：

```bash
python Tools/btl_host.py -v -i COMx -d PIC32CX_SG41 -a 0x2000 -f app.bin
```

该流程是直接覆盖当前活动 Bank 的 APP，不是 Fail-Safe 双 Bank 交换流程。

### 8.3 Fail-Safe：写入不活动 Bank 的 APP 镜像

前提：不活动 Bank 的 `0x80000` 处已经存在有效 BL。

```bash
python Tools/btl_host.py -v -s -i COMx -d PIC32CX_SG41 -a 0x82000 -f app.bin
```

流程：

1. BL 将新 APP 写入不活动 Bank 的 `0x82000`。
2. 上位机发起 CRC 校验。
3. 校验通过后，上位机发送 `0xA4 BKSWAP_RESET`。
4. BL 调用 Bank Swap 并复位。
5. 原不活动 Bank 成为新的活动 Bank。

### 8.4 Fail-Safe：写入不活动 Bank 的合并镜像（BL + APP）

合并镜像包含不活动 Bank 的 BL 和 APP，目标地址为 `0x80000`：

```bash
python Tools/btl_host.py -v -s -i COMx -d PIC32CX_SG41 -a 0x80000 -f merged_bl_app.bin
```

合并镜像布局必须满足：

- `0x0000 ~ 0x1FFF`：BL raw binary。
- `0x2000` 偏移位置：APP raw binary。
- 未使用区域填充 `0xFF`。

该方式适合第一次给空白不活动 Bank 补齐 BL + APP；后续日常升级可以使用 APP-only 命令写入 `0x82000`。

---

## 9. 首次量产/首次双 Bank 烧录顺序

首次使用双 Bank 时，不能直接发送 Bank Swap 命令，必须先保证两个 Bank 都有有效启动内容。

推荐顺序：

1. 通过调试器把 BL 写入 Bank A 的 `0x0000`。
2. 通过调试器把 APP 写入 Bank A 的 `0x2000`。
3. 复位后确认 Bank A 的 APP 能够正常运行。
4. 触发回到 BL。
5. 使用合并镜像把 Bank B 的 BL + APP 写入 `0x80000`，并使用 `-s` 完成第一次 Bank 交换。
6. 复位后确认新活动 Bank 的 APP 能够运行。
7. 后续日常升级只写当前不活动 Bank 的 APP 区域 `0x82000`，再发送 Bank Swap Reset。

如果不活动 Bank 中没有有效 BL，禁止发送 `0xA4`；否则交换复位后可能进入无效 Bank，导致无法正常启动。

---

## 10. 构建后静态验证清单

### 10.1 BL 构建检查

- BL 工程能够 clean build。
- `mem.map` 中 `.vectors` 位于 `0x0000`。
- BL ROM 总大小不超过 `8192` 字节。
- `mem.map` 中可以找到 `NVMCTRL_BankSwap` 的符号引用。
- `custom_trigger.o` 已参与链接。
- 串口 PLIB、NVMCTRL、DSU、SYSTICK 均已参与链接。

### 10.2 APP 构建检查

- APP 工程能够 clean build。
- `mem.map` 中 `.vectors` 位于 `0x2000`。
- APP ROM 结束地址不超过 `0x7FFFF`。
- APP 链接长度为 `0x7E000`，不再是 `0xFE000`。
- 已生成用于上位机烧录的 `app.bin`。

### 10.3 代码与配置检查

- `BTL_DUAL_BANK = true`。
- `BTL_SIZE = 8192`。
- `APP_START_ADDRESS = 0x2000`。
- Firmware Trigger 长度为 16。
- `0xA4` 调用 `NVMCTRL_BankSwap()`，不是普通 `NVIC_SystemReset()`。
- `0xA3` 仍然是普通复位。
- 当前工程中不存在自定义 `common/bl_core.c`、`bsp_flash.c`、`crc32.c`、`ota_uart.c` 等废弃 OTA 层。

---

## 11. 上板测试步骤

### 11.1 BL 基础协议测试

在 Bank A BL 已经烧录、APP 尚未进入升级模式时，先验证基础命令：

1. 上电复位。
2. 串口发送 `READ_VERSION`，BL 回复版本号。
3. 发送错误帧头，BL 回复 `0x51` 或 `0x52`。
4. 发送未实现命令，BL 回复 `0x52`。

在不活动 Bank 尚未写入有效镜像之前，不要直接测试 `0xA4`。

### 11.2 验证 Bank A 基线启动

1. 烧录 Bank A BL 和 Bank A APP。
2. 复位。
3. BL 未检测到触发字，跳转 `0x2000`。
4. APP 按预期点亮 LED 或运行业务。

### 11.3 写入 Bank B 合并镜像

1. 从 APP 触发回到 BL。
2. 使用合并镜像命令写入 `0x80000`。
3. 等待 UNLOCK、DATA、VERIFY 全部通过。
4. 收到 `CRC_OK` 后，上位机发送 `0xA4`。
5. BL 回复 `OK` 后执行 Bank Swap 和复位。
6. 检查 Bank B 的新 APP 是否运行。

### 11.4 验证不活动 Bank APP-only 升级

1. 再次从当前活动 APP 触发回到 BL。
2. 写入当前不活动 Bank 的 APP-only 镜像，目标地址 `0x82000`。
3. CRC 校验通过后执行 Bank Swap Reset。
4. 复位后确认新版本 APP 运行。
5. 掉电重新上电，确认 Bank 映射状态保持，APP 仍能启动。
6. 重复一轮反向切换，确认两个 Bank 可以交替启动。

### 11.5 CRC 失败保护测试

1. 人为使用错误文件或错误地址准备一次写入。
2. 观察 BL 返回 `0x54 CRC_FAIL`。
3. 确认 CRC 失败后不执行 Bank Swap，不复位到无效 Bank。

---

## 12. 常见错误与禁止事项

### 12.1 禁止把 APP 链接长度保留为 `0xFE000`

`0xFE000` 表示从 `0x2000` 一直覆盖到整个 Flash 末尾，会跨越 Bank 边界。双 Bank 方案中 APP 长度必须是 `0x7E000`。

### 12.2 禁止让 APP 后台写 Flash

本方案的 Fail-Safe 靠 BL 和硬件 Bank Swap 完成。APP 不负责接收固件，也不负责写不活动 Bank。

### 12.3 禁止用普通复位代替 Bank Swap

- `0xA3`：普通复位，跳回当前活动 Bank。
- `0xA4`：Bank Swap + 复位，切换活动 Bank。

两者不能混用。

### 12.4 禁止在不活动 Bank 无效时发送 `0xA4`

Bank B 没有有效 BL + APP 时执行交换，会导致复位后从无效 Bank 启动。

### 12.5 禁止手工拼接第二套 Flash 分区常量

地址来源应保持一致：

- MCC 组件配置。
- MCC 生成的 `bootloader_common.h`。
- MCC 生成的 `btl.ld`。
- APP 链接宏 `ROM_ORIGIN / ROM_LENGTH`。

修改分区时必须同步检查这四类文件，不能只改其中一个。

### 12.6 不要恢复旧自定义 OTA 文件

当前方案继续使用 MCC 原生接口和 `Tools/btl_host.py`。旧工程或早期迁移过程中出现的自定义 OTA 层、自定义帧格式、自定义上位机脚本不再纳入本方案。

---

## 13. 关键文件对照

| 作用 | 文件 |
|---|---|
| BL MCC 组件配置 | `BL/config.mcc/mcc/config_default/components/uart_bootloader.yml` |
| BL 公共地址、大小、触发定义 | `BL/config.mcc/src/config/default/bootloader/bootloader_common.h` |
| BL UART 命令分发与 Flash 写入 | `BL/config.mcc/src/config/default/bootloader/bootloader_uart.c` |
| BL 链接脚本 | `BL/config.mcc/src/config/default/btl.ld` |
| Bank Swap PLIB 封装 | `BL/config.mcc/src/config/default/peripheral/nvmctrl/plib_nvmctrl.c` |
| Firmware Trigger 强覆盖 | `BL/config.mcc/src/custom_trigger.c` |
| APP 链接宏 | `APP/.vscode/APP.mplab.json` |
| APP 主程序 | `APP/config.mcc/src/main.c` |
| 上位机脚本 | `Tools/btl_host.py` |

---

## 14. 改造完成判定标准

同时满足以下条件，才认为双 Bank Fail-Safe 改造完成：

1. BL MCC 中 `BTL_DUAL_BANK = true`。
2. BL 构建产物大小不超过 8 KB。
3. BL 命令分发支持 `0xA4 BKSWAP_RESET`，并调用 `NVMCTRL_BankSwap()`。
4. APP 链接范围限制在单个 Bank 内，ROM 起点 `0x2000`，长度 `0x7E000`。
5. APP 能够通过 RAM 触发字回到 BL。
6. 不活动 Bank 为空白时不会误触发 Bank Swap。
7. 合并镜像能够写入 `0x80000`，交换后新 APP 启动。
8. APP-only 镜像能够写入 `0x82000`，CRC 通过后交换并启动。
9. 掉电重启后活动 Bank 保持正确。
10. CRC 失败时不执行 Bank Swap。
