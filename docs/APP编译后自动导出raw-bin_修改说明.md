# APP 编译后自动导出 raw bin 修改说明

## 1. 概述

本次修改在 APP 工程的 CMake 构建流程里加入"编译后自动生成可烧录 raw binary"的步骤。
最终方案：**由构建后步骤调用已验证的 HEX 提取脚本 `Tools/make_merged_bin.py`，从工具链生成的 Intel HEX 按地址区间提取主 Flash 段**，不再对 ELF 做 objcopy 节名过滤。

- 芯片：PIC32CX1025SG41128（Cortex-M4F，1MB Flash，256KB RAM）
- 工具链：XC32 v5.10
- 基线提交：`64ebae5`（APP 记录列表落盘外部 QSPI Flash、上电重载）
- 最终定稿提交：`9131adf`、`4964992`

## 2. 背景与现象

### 2.1 诉求

APP 编译完成后，需要一份可直接喂给 `btl_host.py` 的 raw binary（`.bin`），避免每次手动跑镜像生成脚本。

### 2.2 早期方案与现象

最初在 `user.cmake` 里用 `xc32-objcopy -O binary --only-section=...` 按节名白名单从 ELF 导出 bin。实测出现静默错误：

- 烧录流程本身正常：`Unlocking` → `Programming 100%` → `Verification ... success` → `Reboot Done`，CRC 校验通过。
- 但板子烧录后无任何现象，APP 不运行。
- 改用 `Tools/make_merged_bin.py` 从 HEX 提取出的镜像烧录则正常。

体积和向量头看起来正常，是因为 `.vectors` 节被白名单命中、向量头正确；但中间代码段已被零填充，属于不可从表面判断的静默失败。

## 3. 根因定位

### 3.1 objcopy 白名单漏掉带 `%编号` 后缀的节

XC32/链接器给碎片化节名附加了 `%nn` 后缀，例如：

- `.text%28`、`.text%60`、`.text%71`、`.text%103`
- `.dinit%32`
- `.rodata%90`、`.rodata%91`

objcopy 的 `--only-section` 模式 `.text.*`、`.rodata.*`、`.dinit` 要求 `.text.` 后接点号；而 `.text%28` 第 6 个字符是 `%`，匹配不上。这些节未被纳入导出镜像，对应位置被填成 `0x00`。

### 3.2 被漏掉的 Flash 节（实测）

| Flash 地址 | 节名 | 大小 | bin 偏移 | 说明 |
|---|---|---|---|---|
| 0x2D3C | `.text%28` | 0x2A0 | 0x0D3C | 代码块 |
| 0x3440 | `.dinit%32` | 0x0C0 | 0x1440 | `.data` 全局变量 Flash 初值镜像 |
| 0x4094 | `.text%60` | 0x040 | 0x2094 | 代码块 |
| 0x4310 | `.text%71` | 0x030 | 0x2310 | 代码块 |
| 0x456E | `.rodata%90` | 0x016 | 0x256E | 只读数据 |
| 0x4584 | `.rodata%91` | 0x014 | 0x2584 | 只读数据 |
| 0x4620 | `.text%103` | 0x004 | 0x2620 | 代码块 |

其中 `.dinit%32` 被漏尤其致命：它是 RAM 初值镜像，漏掉后启动拷贝到 RAM 的全局变量初值错误；叠加多个函数代码块被清零，CPU 跳进空函数或拿到错误初值后 HardFault / 卡死，串口无输出。

### 3.3 对照证据

同一偏移上，objcopy 版全为 `00`，HEX 提取版为真实指令/数据：

```
偏移 0x0D3C (Flash 0x2D3C)
  objcopy 版 : 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  HEX 提取版: 2D E9 F0 47 86 46 8C 46 08 46 08 9E 15 46 74 46

偏移 0x1440 (Flash 0x3440, .dinit)
  objcopy 版 : 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  HEX 提取版: B0 28 00 20 1C 00 00 00 00 00 00 00 00 29 00 20
```

### 3.4 为什么走 HEX 提取可行

Intel HEX 每条记录自带绝对地址，提取脚本按地址区间筛选主 Flash 段，不依赖节名，因此不会漏节；同时天然跳过 0x804000 配置位区和 RAM 段，避免裸 objcopy 按地址跨度铺开导致的数 MB 膨胀。

## 4. 修改内容

### 4.1 文件清单（相对基线 `64ebae5`，排除 `main.c`）

| 文件 | 状态 | 性质 |
|---|---|---|
| `APP/cmake/APP/default/user.cmake` | 新增 | 功能性改动：构建后自动导出 bin 的 CMake 接线 |
| `APP/config.mcc/mcc/mcc-manifest-autosave.yml` | 修改 | 非功能：仅 MCC 自动保存时间戳更新 |

> `APP/config.mcc/src/main.c` 的两处 LED 调试改动（`LED1_Clear()` 取消注释、`LED2_Clear()` 注释掉）不属于本次构建方案改动，已排除在本说明之外。

### 4.2 user.cmake 说明

`user.cmake` 位于生成的 `CMakeLists.txt` 同目录；该生成文件已在 include `main.cmake` 之后自动 `include(user.cmake)`，**不修改 MCC 生成文件**。

逐段作用：

1. **定位 ELF 目标**：用 `get_property(... BUILDSYSTEM_TARGETS)` 取目标列表，按前缀 `^APP_default_image_` 匹配 elf 目标，不硬编码 MCC 生成的哈希后缀。
2. **路径定位**：本文件在 `<repo>/APP/cmake/APP/default/`，向上四级到仓库根，拼接脚本路径 `Tools/make_merged_bin.py`。
3. **查找 Python**：`find_program(PYTHON_EXECUTABLE NAMES python python3 py)`。
4. **容错分支**：找不到 Python 或脚本不存在时，`message(WARNING)` 告警并跳过自动导出，不中断 CMake 配置。
5. **导出目标**：定义 ALL 自定义目标 `APP_default_ExportAppBin`，命令行调用提取脚本；`DEPENDS` + `add_dependencies` 挂到现有 `APP_default_Bin2Hex` 目标，保证先生成 `.hex` 再跑提取。

### 4.3 复用的既有脚本

`Tools/make_merged_bin.py`（本次未改逻辑，仅被 CMake 接线调用）：

- 输入：`BL/out/BL/default.hex`、`APP/out/APP/default.hex`
- 输出：`BL/out/BL/bl.bin`、`BL/out/BL/app.bin`、`BL/out/BL/merged_bl_app.bin`
- 提取口径：BL 主 Flash `[0, 0x2000)`；APP 主 Flash `[0x2000, 0x80000)`；跳过 0x804000 配置位区；缺地址填 `0xFF`。

校验脚本 `Tools/verify_merged_bin.py` 独立回读三份 bin 与 HEX 主 Flash 逐字节比对。

## 5. 验证记录

> 说明：CMake 重新配置/编译由开发者自行完成；以下为打包脚本与产物的静态/产物级核对结果。

### 5.1 提取脚本运行结果

```
== APP image ==
  program range : 0x00002000 ~ 0x0000462A (9770 bytes)
  app.bin       : 9770 bytes
  APP vector head: F8 FF 03 20 21 38 00 00
  boundary check: OK
```

- APP 镜像长度 9770 字节，未膨胀。
- 向量头：MSP=`0x2003FFF8`，复位向量=`0x00003821`（Thumb 入口 `0x00003820`）。

### 5.2 独立校验

`verify_merged_bin.py` 输出：

```
bl.bin size   : 8192 (expect 8192)
app.bin size  : 9770
merged size   : 24576 (0x6000)
BL vector: MSP=0x2003FFF8 reset=0x00000009
APP vector: MSP=0x2003FFF8 reset=0x00003821 (Thumb entry 0x00003820)
merged BL slot identical to bl.bin : OK
merged APP slot identical to app.bin: OK
HEX main-Flash byte compare         : OK

ALL CHECKS PASSED
```

### 5.3 原零填充偏移复查

此前被清零的偏移（0xD3C、0x1440、0x2094、0x2310、0x256E）在新镜像中均为真实指令/数据，与 HEX 基准一致。

## 6. 产物与烧录口径

- 自动导出的 APP 镜像落点：`BL/out/BL/app.bin`（与使用手册口径一致）。
- 烧录 APP（活动 Bank A，地址 0x2000）：

```
python Tools\btl_host.py -v -i COM3 -d PIC32CX_SG41 -a 0x2000 -f BL\out\BL\app.bin
```

- 旧的 `APP/out/APP/app.bin`（objcopy 清零版）已删除，不要再使用。

## 7. 迁移到其他项目的要点

### 7.1 必须一起搬运的文件

只放 `user.cmake` 不够，提取脚本本体必须一并拷贝：

1. `Tools/make_merged_bin.py`（提取逻辑）。
2. 目标工程生成 `CMakeLists.txt` 同目录下的 `user.cmake`（接线）。
3. 可选：`Tools/verify_merged_bin.py`（产出后独立校验，非构建必需）。

### 7.2 环境不变、分区不变时可照搬的部分

- Flash 分区地址与擦除块大小：`APP_OFFSET=0x2000`、`BANK_SIZE=0x80000`、`ERASE_BLOCK=0x2000`、配置位区 `0x804000`、`FLASH_END=0x100000`。
- Python 查找方式、`DEPENDS`/`add_dependencies` 接线写法、HEX 按地址提取的逻辑。

### 7.3 仍需按新项目核对的项

| 位置 | 核对项 | 本项目取值 |
|---|---|---|
| `user.cmake` | 仓库根相对层级 | `../../../../`（按目录深度调整） |
| `user.cmake` | ELF 目标前缀正则 | `^APP_default_image_` |
| `user.cmake` | 现有 hex 目标名 | `APP_default_Bin2Hex`（取自新项目 `.generated/main.cmake`） |
| `make_merged_bin.py` | HEX 输入 / 输出目录路径 | `BL/out/BL`、`APP/out/APP` |

### 7.4 接线时序不是编译器默认行为

- 工具链默认只保证源码链接出 ELF；生成 `.hex` 是 MCC 额外挂的 bin2hex 目标。
- "先生成 hex、再跑提取脚本"的顺序必须靠 `DEPENDS` + `add_dependencies` 显式声明；不写依赖时，并行构建（ninja / `make -j`）可能在 hex 未刷新时就执行提取脚本。

### 7.5 单 APP 项目简化

若新项目没有 BL / 双 Bank，可把脚本简化为只读一份 APP HEX、提取 `[ROM_ORIGIN, FLASH_END)` 区间输出 `app.bin`，去掉 `bl.bin`、`merged` 相关逻辑。

## 8. 提交记录（基线 `64ebae5` 之后）

| 提交 | 说明 |
|---|---|
| `22502c3` | chore: MCC autosave 时间戳基线 |
| `512306c` | APP: 初版 objcopy 节过滤导出 bin（已被后续方案取代） |
| `6338569` | APP: 板上 LED1 调试改动（main.c，不计入构建方案） |
| `9131adf` | build: 改为经 make_merged_bin.py 从 HEX 导出 bin，去掉 objcopy 节过滤 |
| `4964992` | APP: 板上 LED2 调试改动（main.c，不计入构建方案） |

## 9. 边界与备注

- 本方案只覆盖 raw bin 的自动导出；编译、调试、上板由开发者完成。
- 提取脚本读固定路径 HEX，不比较时间戳；双 Bank 场景需保证 BL HEX 先编译、再编译 APP HEX。
- 脚本同时刷新 `bl.bin` 与 `merged_bl_app.bin`；若只编译 APP 而 BL HEX 缺失，脚本会因读不到 BL HEX 报错，属既有前提约束。
- 配置位（0x804000）不进入 OTA 镜像，与 `BTL_FUSE_PROGRAM_ENABLE` 未勾选的现状一致。
