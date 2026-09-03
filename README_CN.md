# T5S3-GameBoy

**中文** | [English](README.md)

基于 [LilyGO T5S3-4.7-e-paper-PRO](https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO) 的竖屏触控 Game Boy 模拟器。项目使用 CrankBoy 模拟器核心，并结合开发板的 GT911 触摸控制器、BQ27220/BQ25896 电源硬件、SD 卡槽和实时 1bpp 墨水屏刷新方案。在保留本项目 T5S3 专用显示与电源实现的基础上，移植了 Paperboy 的 ROM 库、持久化和音频工作流。

| ![](./docs/1.jpg) | ![](./docs/2.jpg) |
| --- | --- |

## 功能

- 基于 CrankBoy 的 DMG 模拟，游戏画面为 480 x 432，并通过固定抖动转换成清晰的 1bpp 黑白输出。
- GT911 多点触控十字键、A/B、SELECT/START、SAVE/LOAD、电源和设置操作。
- SD ROM 库会按名称排序扫描卡根目录及一级子目录，识别大于 0 字节且不超过 4 MiB 的 `.gb` 和 `.gbc` 文件，最多列出 64 个 ROM。
- ROM 库支持上一个/下一个、启动游戏、恢复上次游戏、切换声音引擎和重新扫描。
- 使用同目录 `.sav` 文件持久化卡带 SRAM/RTC，使用带版本信息的 `.state` 文件保存完整模拟器快照。
- SD 卡根目录的 `/paperboy.cfg` 会记录上次启动的 ROM 和所选声音引擎。
- PCM、POLY 和 MUTE 三种运行时声音模式；PCM 与 POLY 通过可配置 GPIO 输出 LEDC PWM。
- 保留本项目原有的 T5S3 EPD 双缓冲和实时局部刷新实现。
- BQ27220 电量计与 BQ25896 充电管理页面、主页电池状态以及带 3500/3600 mV 滞回的醒目低电量警告。
- `BOOT` 键执行白-黑-白清屏，然后重绘当前页面。
- 在任意页面长按板载 `S3` 功能键两秒，会先保存待写入的持久化数据，再清理屏幕并安全关机；长按 `PWR` 可重新开机。

## SD ROM 库

请使用 FAT32 格式的 SD 卡，并在开机前插入。ROM 可以放在根目录或恰好一级子目录中：

```text
/
|-- paperboy.cfg          固件自动生成和维护
|-- Tetris.gb
|-- Tetris.gb.sav         Tetris.gb 的电池 SRAM/RTC 数据
|-- Tetris.gb.state       Tetris.gb 的完整运行快照
`-- games/
    |-- Zelda.gbc         仅在 ROM 兼容 DMG 模式时可运行
    |-- Zelda.gbc.sav
    `-- Zelda.gbc.state
```

进入 **Settings > SD Card** 使用 ROM 库：

- `PREV` / `NEXT` 切换当前条目，长按可连续翻页；`PLAY` 启动所选 ROM。
- `LOAD LAST` 打开 `paperboy.cfg` 中记录的 ROM；存在对应 `.state` 时会恢复快照。
- `SOUND` 在 PCM、POLY 和 MUTE 之间循环切换，并将选择保存到 `paperboy.cfg`。
- `RESCAN` 重新扫描当前已挂载 SD 卡上的 ROM。

扫描器最多保留 64 个条目，并且不会递归到一级子目录以下；扩展名匹配不区分大小写。ROM 必须大于 0 字节且不超过 4 MiB（4,194,304 字节），扫描时会忽略空文件和超限文件，它们不会占用 ROM 列表条目。`.gbc` 扩展名并不表示固件具备完整 Game Boy Color 支持：只有能以 DMG 兼容模式运行的 ROM 才受支持，不支持 GBC-only 游戏。

## 游戏存档与快照

对于从 SD 卡加载的 `game.gb` 或 `game.gbc`，游戏页面的 `SAVE` 操作会在 ROM 同一目录写入：

- `game.gb.sav` / `game.gbc.sav`：在卡带支持时保存电池供电 SRAM 和 RTC 数据。
- `game.gb.state` / `game.gbc.state`：保存带版本信息的 CrankBoy 完整模拟器状态，包括 MiniGB APU 音频状态，用于恢复当时的运行进度。

`LOAD` 会恢复当前 ROM 的快照。快照中的版本化 APU 段会保留音频寄存器、声道合成计数器和帧/采样小数相位；所选输出引擎与物理 GPIO 配置仍来自 `paperboy.cfg` 和固件构建配置。切换 ROM 或关机前，固件也会写回有改动的电池存档。`paperboy.cfg`、`.sav` 和 `.state` 均通过临时文件及备份/重命名流程进行替换；如果写入被意外中断，固件可以恢复此前的有效副本。

固件可读取原 Paperboy 写入的 SD 卡数据：会归一化旧的 `/sdcard/...` 配置路径；找不到 `game.gb.sav` 或 `game.gb.state` 时，会继续查找 `game.sav` 或 `game.state`；同时兼容裸 CrankBoy 快照和 PBSV v1 存档。新的写入使用无歧义的侧载文件名和 PBSV v2；仅当追加式名称因路径过长而无法使用时，才回退到较短的旧式文件名。Paperboy v1 保存的是本次开机后的相对时间，而不是现实世界时间，因此该旧时间戳不会用于关机期间的 RTC 追时，但存档中的 RTC 寄存器仍会正常恢复。

快照与对应 ROM 及 CrankBoy 状态格式相关。跨固件版本迁移数据时，应将 `.sav` 作为长期游戏进度记录保留。内置 ROM 或编译进固件的 ROM 没有对应的 SD 路径，因此其快速 SAVE/LOAD 仍只保存在内存中，复位或断电后会丢失。

## 外接音频

T5S3-4.7-e-paper-PRO 板上没有扬声器。PCM 和 POLY 可以为外接滤波器/功放产生单线 PWM，但物理引脚输出默认禁用（`PAPERBOY_AUDIO_GPIO=-1`）。APU 与三种可选声音模式仍可运行，但固件不会误认为板上存在扬声器。

选择经过确认且与板载外设隔离的输出后，将该 GPIO 接到合适的低通滤波器/功放输入，并将双方 GND 共地。不要使用 ESP32-S3 GPIO 直接驱动无源扬声器。虽然 Game Boy 各声道会参与 APU 混音，但物理输出为单声道。

`GPIO1` 是可选 LoRa 模块的 `LORA_RST` 信号，因此固件不会默认用它输出音频；驱动该引脚可能造成冲突或反向供电。若要启用物理输出，请先按实际板卡和接线确认一个可用引脚，再向 `platformio.ini` 现有的 `build_flags` 列表追加构建宏：

```ini
-DPAPERBOY_AUDIO_GPIO=YOUR_VERIFIED_GPIO
```

接线前请根据开发板原理图和[引脚表](docs/pinmap_cn.md)确认替代引脚没有冲突。SD 接口使用 MISO `GPIO21`、MOSI `GPIO13`、SCK `GPIO14` 和 CS `GPIO12`。

## 项目结构

```text
T5S3-GameBoy/
|-- src/                   应用、显示、触摸、存储和模拟器代码
|   |-- crankboy_core/     CrankBoy 核心及版本化状态兼容代码
|   |-- minigb_apu/        Game Boy 音频处理单元模拟
|   `-- rom/               编译期自定义 ROM 说明及生成的 test_rom.h
|-- lib/                   BQ25896、BQ27220 和 I2C 兼容库
|-- boards/                LilyGO T5S3 PlatformIO 板卡配置
|-- docs/                  硬件引脚表和项目图片
|-- tools/                 .gb ROM 转换工具
|-- firmware/              发布固件
`-- platformio.ini         项目构建配置
```

## 编译与烧录

安装 PlatformIO 后，在项目根目录执行：

```powershell
pio run
pio run -t upload --upload-port COM45
pio device monitor -p COM45 -b 115200
```

请将 `COM45` 替换为设备实际串口。项目仅有一个 PlatformIO 环境：`T5S3-GameBoy`。

## 将 ROM 编译进固件

通常应直接通过 SD ROM 库添加游戏，也可以将合法取得且兼容 DMG 的 ROM 编译进固件作为后备。以 `maxpirateeb.gb` 为例：

1. 从明确授权你使用的来源取得游戏 `.gb` 文件。
2. 将文件放入项目的 `ROMs/` 目录。
3. 生成头文件、清理旧构建并烧录：

```powershell
python tools/gb_rom_to_header.py .\ROMs\maxpirateeb.gb
pio run -t clean
pio run -t upload --upload-port COM45
```

工具会生成 `src/rom/test_rom.h`。该文件存在时会替换内置演示 ROM；删除后重新编译即可恢复演示 ROM。生成的头文件默认不纳入 Git，以降低误提交受版权保护 ROM 数据的风险。

以下站点提供明确授权的自制游戏：

- <https://hh.gbdev.io/search?typetag=game>
- <https://itch.io/games/tag-gameboy>
- <https://itch.io/jams/tag-gameboy>

请选择标明 DMG、Original Game Boy 或 Game Boy compatible 的 ROM，不要使用 GBC-only ROM。“老游戏”“绝版”或“abandonware”本身不代表可以合法下载或再分发；请只使用自己拥有或已获授权的 ROM，并且不要向本仓库提交商业 ROM 数据。

## 充电参数

充电方案与 T5S3-Reader 保持一致：

- 输入电流上限：1000 mA
- 快充电流：512 mA
- 预充/终止电流：64 / 64 mA
- 充电电压：4208 mV
- 最低系统电压：3300 mV
- 电池模型容量：1500 mAh

固件不会在 NTC、温度或安全计时故障期间强制恢复充电。
