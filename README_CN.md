# ESP32-S31 经典蓝牙音频（A2DP 音箱）

把这块板子当成 **蓝牙音箱**：手机搜到 **`S31-BT-AUDIO_xxxx`**（后面是 MAC 后两字节），连上后就能放歌。底层是乐鑫 `esp-gmf` 的 `bt_audio` 例程，板级改成了本板的 **ES8389 + I2S + SDMMC**。

默认角色：**A2DP Sink + HFP Handsfree + AVRCP**（经典蓝牙，BlueDroid）。默认 **打开** 800×480 ST7262 触屏 LVGL 界面（播放控制 / 拨号盘 / 音量条）。

## 硬件（已按本板改好）

| 功能 | 芯片 / 接口 | 引脚 |
|------|-------------|------|
| Codec | ES8389 | I2C `SDA=GPIO0` `SCL=GPIO1`，地址 `0x20` |
| I2S0 全双工 | STD Philips 16bit 立体声 | `BCK=GPIO49` `WS=GPIO50` `DOUT=GPIO51` `DIN=GPIO52`，`MCLK` 悬空 |
| 功放使能 | PA | `GPIO47`，高电平有效 |
| microSD | SDMMC 4-bit | `CLK=24` `CMD=25` `D0~D3=20~23`，电源使能 `GPIO60`（低电平有效） |
| 屏 | ST7262 800×480 RGB666（按 RGB888 24 线） | `PCLK=40` `DE=43` `HSYNC=44` `VSYNC=45`，背光 `GPIO39`（高有效） |
| 触摸 | GT911 | 与 Codec 共用 I2C，8 位地址 `0xBA` / `0x28` |

请插入一张 **FAT32** 的 SD 卡（板级初始化会挂载 `/sdcard`）。只测蓝牙音箱也可以插空卡/有卡即可；**不要不插卡**，否则 `esp_board_manager_init()` 可能失败。

自定义板名：`s31_es8389`（在 `components/s31_es8389/`）。

> 不要选官方 `esp32_s31_korvo_1`：那块板 I2S 在 GPIO3/4/5/6、PA 在 GPIO7、SD 电源在 GPIO39，和本板对不上。

## 编译前准备（PowerShell）

每次新开终端先加载 IDF：

```powershell
. C:\Espressif\tools\Microsoft.master.PowerShell_profile.ps1
pip install esp-bmgr-assist
```

进入工程：

```powershell
cd E:\idf-debuging\02_s31_examples\bt_audio
```

第一次（或换过板型 / 清过 `sdkconfig`）按顺序执行：

```powershell
$env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults;sdkconfig.defaults.esp32s31.classic"
idf.py --preview set-target esp32s31
idf.py bmgr -b s31_es8389
idf.py --preview build
```

烧录（把 `COM3` 换成你的口）：

```powershell
idf.py --preview -p COM3 flash monitor
```

退出 monitor：`Ctrl+]`。

成功启动后串口应出现类似：

```
I (...) DEV_AUDIO_CODEC: Successfully initialized codec: audio_dac
I (...) DEV_AUDIO_CODEC: Successfully initialized codec: audio_adc
I (...) DEV_FS_FAT: Filesystem mounted, base path: /sdcard
I (...) A2DP sink: initialized
Type 'help' to get the list of commands.
BTAudio >
```

## 怎么测（蓝牙音箱）

1. 手机打开蓝牙，扫描，找到 **`S31-BT-AUDIO_xxxx`**。
2. 配对连接。部分手机会问是否允许「媒体音频 / 通话音频」，都允许。
3. 用网易云 / QQ 音乐 / YouTube 放歌，喇叭应出声。
4. USB 串口输入 `help` 可看指令。常用：
   - `play` / `pause` / `stop` / `next` / `prev`
   - `vol_set 70`（0–100）
5. 通话（HFP）：用手机拨打/接听，板子 MIC 走 ES8389 ADC（`DIN=GPIO52`）。

iPhone 可以连 A2DP 放歌。这是经典蓝牙音频，不是 BLE。

## 可选：A2DP Source（当音源推到耳机）

1. SD 卡根目录放 `media0.mp3`、`media1.mp3`、`media2.mp3`。
2. `idf.py menuconfig` → `BT Audio Basic Example (GMF)` → `Classic Audio Roles Configuration` → A2DP 选 **A2DP Source**。
3. 重新编译烧录。串口：`start_discovery`，再 `connect <mac>`，然后 `start_media`。

## 故障排除

| 现象 | 处理 |
|------|------|
| 本机 IDF 6.2 编译报 `stringop-truncation` | 工程已加 `-Wno-error=stringop-truncation`，属于 IDF Bluedroid 告警 |
| `idf.py bmgr` 没有 `s31_es8389` | 确认 `components/s31_es8389/board_info.yaml` 存在，并已 `pip install esp-bmgr-assist` |
| 卡在 `esp_board_manager_init` / SD 失败 | 插入 FAT32 SD 卡；确认 GPIO60 电源 |
| Codec 初始化失败 | 查 I2C 0/1、地址 0x20、ES8389 供电 |
| 能连上但没声音 | 查 PA GPIO47、喇叭接线；串口 `vol_set 80` |
| 搜不到设备 | 确认日志已有 A2DP sink initialized；关掉再开手机蓝牙 |

LE Audio（NimBLE）需要换 `sdkconfig.defaults.esp32s31.nimble`，本 README 默认只讲经典蓝牙音箱。
