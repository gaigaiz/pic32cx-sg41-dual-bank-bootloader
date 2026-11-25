# 编译后自动导出 raw bin 方案 · 移植指南

面向其他 Cortex-M / XC32（或同类 GCC 工具链）项目：在 CMake 构建完成后，自动从工具链生成的 Intel HEX 提取主 Flash 段，产出可直接喂给串口 Bootloader 上位机的 raw binary（`.bin`）。

## 1. 这套方案解决什么问题

- 目标：编译完成后自动得到一份可烧录的 raw bin，不用每次手动跑打包脚本。
- 做法：**从 Intel HEX 按绝对地址区间提取主 Flash 段**，而不是从 ELF 用 objcopy 按节名过滤。
- 为什么不走 objcopy：XC32/GCC 会给碎片化节名附加 `%nn` 后缀（如 `.text%28`、`.dinit%32`、`.rodata%90`），objcopy 的 `--only-section` 白名单匹配不到这些节，漏下的节在 bin 里被填成 `0x00`。向量头看着正常，中间代码却是空的，芯片烧录后不运行，属于静默错误。
- HEX 每条记录自带绝对地址，按地址区间筛选不依赖节名，天然保留全部主 Flash 字节，并跳过配置位区和 RAM 段。

## 2. 适用前提

- 工具链能产出 Intel HEX（bin2hex 或 `objcopy -O ihex`）。
- 目标 Flash 存在独立配置位区（如 0x804000 一带）和 RAM 段；裸 `objcopy -O binary` 直接转 ELF 会按地址跨度铺开，导致 bin 膨胀到数 MB。
- 构建机器装有 Python，命令行可执行 `python`（或 `python3` / `py`）。
- MCC/CMake 工程的生成 `CMakeLists.txt` 里预留了 `user.cmake` 挂载点（见第 5 节）。

## 3. 需要搬运的文件

| 文件 | 是否必需 | 说明 |
|---|---|---|
| 提取脚本 `Tools/make_merged_bin.py` | 必需 | 从 HEX 提取主 Flash 段生成 bin；分区常量和输入输出路径按新项目调整 |
| 自定义接线 `user.cmake` | 必需 | 放在工程生成 CMakeLists 同级目录，构建后调用提取脚本 |
| 校验脚本 `Tools/verify_merged_bin.py` | 可选 | 产出后独立回读 bin 与 HEX 逐字节比对；不上板前建议保留 |

> 只贴 user.cmake 片段、不拷提取脚本，构建后步骤会找不到脚本而无法导出。

## 4. 目录结构与 user.cmake 放置位置

参考 MCC Melody 工程结构：

```
<工程>/cmake/<组>/default/
├── CMakeLists.txt              ← MCC 生成，不要直接改
├── user.cmake                  ← 自定义文件，放这一层（新建）
└── .generated/                 ← MCC 生成的内部文件，勿放自定义内容
    ├── toolchain.cmake
    ├── main.cmake
    ├── file.cmake
    └── rule.cmake
```

- `user.cmake` 与生成的 `CMakeLists.txt` 同级，**不要**放进 `.generated/` 目录。
- 生成的 `CMakeLists.txt` 里应预留如下钩子；先确认新项目有这段，存在才 include、不存在不报错：

```cmake
if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/user.cmake)
    include(user.cmake)
endif()
```

- 若新项目 `CMakeLists.txt` 没有这段钩子，先核对 MCC 版本是否支持；不要把自定义文件塞进 `.generated/`（会被重新生成覆盖）。

## 5. 步骤一：放置并修改提取脚本

把 `make_merged_bin.py` 拷到新项目 `Tools/` 下，按新项目改以下内容。

### 5.1 分区常量（按芯片数据手册 / 链接脚本核对）

| 常量 | 含义 | 本项目示例值 |
|---|---|---|
| `APP_OFFSET` | APP 在 Flash 的起始地址（Bank 内偏移） | `0x2000` |
| `BANK_SIZE` | 单个 Bank 大小 | `0x80000` |
| `ERASE_BLOCK` | NVMCTRL 擦除块大小 | `0x2000`（8192） |
| `CONFIG_BASE` | 配置位区基准，提取时排除 | `0x800000` |
| `FLASH_END` | 整片 Flash 结束地址 | `0x100000` |
| `BL_SIZE` | BL 槽大小（仅双 Bank/含 BL 场景需要） | `0x2000` |

### 5.2 输入输出路径（按新项目目录调整）

脚本里写死的路径：

- `HEX_BL`：BL 的 `.hex` 路径（单 APP 项目可去掉）。
- `HEX_APP`：APP 的 `.hex` 路径。
- `OUT_DIR`：bin 输出目录。
- `OUT_BL_BIN` / `OUT_APP_BIN` / `OUT_MERGED`：各产物文件名。

### 5.3 单 APP 项目简化

若新项目没有 BL、没有双 Bank，把脚本精简为：只读一份 APP HEX，提取 `[APP_OFFSET, FLASH_END)` 区间，缺地址填 `0xFF`，输出单个 `app.bin`；删除 BL 槽位和 merged 合并逻辑。

## 6. 步骤二：创建 user.cmake 并贴入骨架

在 `CMakeLists.txt` 同级目录新建 `user.cmake`，最小骨架如下：

```cmake
# 仓库根：按本文件在新项目中的目录深度调整 ../ 数量
set(APP_default_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../../")
set(APP_default_merge_script "${APP_default_repo_root}/Tools/make_merged_bin.py")

find_program(PYTHON_EXECUTABLE NAMES python python3 py)

add_custom_target(APP_default_ExportAppBin ALL
    COMMAND ${PYTHON_EXECUTABLE} "${APP_default_merge_script}"
    COMMENT "Extract APP raw bin from HEX (make_merged_bin.py)"
    DEPENDS APP_default_Bin2Hex)
add_dependencies(APP_default_ExportAppBin APP_default_Bin2Hex)
```

### 骨架各段作用

1. 路径变量：定位仓库根和提取脚本；写在使用它们的 target 之前。
2. `find_program`：查找可用 Python 解释器。
3. 导出目标：`ALL` 自定义目标，调用脚本；`DEPENDS` + `add_dependencies` 挂到现有 hex 目标上，保证先生成 `.hex` 再提取。

### 可选容错分支

需要时可加上：找不到 Python 或脚本不存在时 `message(WARNING)` 告警并跳过导出，不让 CMake 配置中断：

```cmake
if(NOT PYTHON_EXECUTABLE)
    message(WARNING "Python interpreter not found; run the bin export script manually.")
elseif(NOT EXISTS "${APP_default_merge_script}")
    message(WARNING "Extraction script not found: ${APP_default_merge_script}")
else()
    add_custom_target(APP_default_ExportAppBin ALL
        COMMAND ${PYTHON_EXECUTABLE} "${APP_default_merge_script}"
        COMMENT "Extract APP raw bin from HEX"
        DEPENDS APP_default_Bin2Hex)
    add_dependencies(APP_default_ExportAppBin APP_default_Bin2Hex)
endif()
```

## 7. 必须按新项目修改的参数清单

| 位置 | 核对项 | 怎么找新项目的实际值 | 本项目示例 |
|---|---|---|---|
| `user.cmake` | 仓库根相对层级 | 数 `user.cmake` 到仓库根有几层目录 | `../../../../` |
| `user.cmake` | hex 目标名 | 打开新项目 `.generated/main.cmake`，找 bin2hex 的 `add_custom_target` 名 | `APP_default_Bin2Hex` |
| `user.cmake` | 脚本路径 | 对齐脚本在新项目的实际位置 | `Tools/make_merged_bin.py` |
| 提取脚本 | HEX 输入路径 | 对照新项目 out 目录结构 | `BL/out/BL/default.hex`、`APP/out/APP/default.hex` |
| 提取脚本 | 分区常量 | 对照芯片手册与链接脚本 `ROM_ORIGIN` / `ROM_LENGTH` | 见 5.1 |

> 若需要在 CMake 里定位 ELF 目标名，可按前缀匹配，但要注意前缀随工程名变化；本方案的导出目标并不强制依赖 ELF 目标名。

## 8. 接线时序（不是编译器默认行为）

- 工具链默认只保证源码链接出 ELF；生成 `.hex` 是 MCC 额外挂的 bin2hex 目标。
- "先生成 hex、再跑提取脚本"的顺序**必须**靠 `DEPENDS` + `add_dependencies` 显式声明。
- 不写依赖时，并行构建（ninja / `make -j`）可能在 hex 尚未刷新时就执行提取脚本，读到旧 hex 或找不到文件。
- 提取脚本读固定路径 HEX、不比较时间戳；双 Bank 场景需保证 BL HEX 先编译、再编译 APP HEX。

## 9. 移植后验证清单

1. 配置/编译工程，提取脚本自动执行且无报错。
2. 产物 bin 大小合理（不是数 MB），落在预期 Flash 区间。
3. bin 向量头：MSP 为 RAM 顶地址，复位向量指向有效 Flash 地址（Thumb 地址最低位为 1）。
4. 跑校验脚本：bin 与 HEX 主 Flash 段逐字节一致；未使用尾部为 `0xFF`；不含配置位区内容。
5. 上板确认能正常跳转运行。

## 10. 常见坑

- **bin 膨胀到数 MB**：说明把配置位段或 RAM 段按地址铺开了。检查提取脚本是否只保留主 Flash 区间、是否跳过配置位区。
- **烧录 CRC 通过但板子无现象**：典型为镜像代码段被零填充。先用 HEX 提取版对比；不要用 elf 直出 objcopy 白名单。
- **脚本报找不到 HEX**：输入路径与新项目 out 目录不一致，或另一路 HEX 尚未编译生成。
- **改动被 MCC 覆盖**：自定义内容只能放 `user.cmake`，不要改 `.generated/` 和生成的 `CMakeLists.txt`。
- **配置位误入 OTA 镜像**：提取区间必须排除配置位区；串口 OTA 通常不更新配置位。

## 11. 本项目可参照的现成文件

- 提取脚本：`Tools/make_merged_bin.py`
- 校验脚本：`Tools/verify_merged_bin.py`
- CMake 接线：`APP/cmake/APP/default/user.cmake`
- 本项目改动说明：`docs/APP编译后自动导出raw-bin_修改说明.md`

## 12. 本项目参考值（仅示例，移植时以新项目为准）

| 项 | 值 |
|---|---|
| 芯片 | PIC32CX1025SG41128（Cortex-M4F，1MB Flash，256KB RAM） |
| 工具链 | XC32 v5.10 |
| APP 链接起点（Bank 内偏移） | `0x2000` |
| BL 链接起点（Bank 内偏移） | `0x0` |
| 单 Bank 大小 | `0x80000` |
| 擦除块 | 8192 字节（`0x2000`） |
| APP 产物落点 | `BL/out/BL/app.bin` |
| 上位机 | `Tools/btl_host.py`，只接收 raw binary，不解析 HEX |
