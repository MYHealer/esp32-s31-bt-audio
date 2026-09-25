# ESP32-S31 Bluetooth Audio

ESP32-S31 蓝牙音频项目，支持 A2DP Sink 多 codec（SBC/AAC/LDAC/LHDC/aptX/aptX-HD/aptX-LL/Opus/LC3plus）。

## 硬件

- **芯片**: ESP32-S31（RISC-V 双核，320MHz，Wi-Fi 6 / BT 5.4）
- **Codec**: ES8389（I2S，48kHz/16bit/stereo）
- **屏幕**: ST7262 800×480 RGB + GT911 触摸
- **存储**: SDMMC
- **PA**: GPIO47（有源高）

## 构建环境

### 1. 安装 fork IDF

本项目需要 **esp-idf-v6.1-codecs**（含 LDAC/aptX/LHDC 解码器的 fork）：

```bash
git clone https://github.com/pschatzmann/ESP32-S31-A2DP-Codecs.git
cd ESP32-S31-A2DP-Codecs/esp-idf-v6.1-codecs
./install.sh
```

### 2. 应用 fork 补丁

> ⚠️ **被 fork 的 IDF 在工程内有两份副本，改一份等于没改。**
>
> | 路径 | 角色 |
> |---|---|
> | `fork_patches/` | 存档，**被本仓库 git 跟踪** |
> | `ref_prj/ESP32-S31-A2DP-Codecs-main/.../esp-idf-v6.1-codecs/` | **实际编译的那份**，不受本仓库 git 跟踪 |
>
> 所以：
> - **改文件** → 改 `ref_prj/` 那份（生效），再 `cp` 回 `fork_patches/`（存档）。
> - **回退** → 从 `fork_patches/` 取，然后**必须 `cp` 到 `ref_prj/`**。
>   `git checkout HEAD -- fork_patches/...` **只回退存档**，编译仍读旧内容。
> - **`git status` 不可信**：它只反映 `fork_patches/`，`ref_prj/` 改了也不显示。
>
> 每次改完或回退完，跑一次校验（**必须忽略行尾**，两份 CRLF/LF 不同，否则会看到满屏假差异）：
>
> ```bash
> FORK=ref_prj/ESP32-S31-A2DP-Codecs-main/ESP32-S31-A2DP-Codecs-main/esp-idf-v6.1-codecs
> diff --strip-trailing-cr fork_patches/a2dp_vendor_mihc.c \
>      $FORK/components/bt/host/bluedroid/stack/a2dp/a2dp_vendor_mihc.c
> # 差异 0 行才算同步
> ```

将 `fork_patches/` 中的文件覆盖到 fork IDF 的对应位置：

```bash
FORK=ref_prj/ESP32-S31-A2DP-Codecs-main/ESP32-S31-A2DP-Codecs-main/esp-idf-v6.1-codecs
cp fork_patches/btc_av.c $FORK/components/bt/host/bluedroid/btc/profile/std/a2dp/
cp fork_patches/btc_av.h $FORK/components/bt/host/bluedroid/btc/profile/std/include/
cp fork_patches/bta_av_api.h $FORK/components/bt/host/bluedroid/bta/include/bta/
cp fork_patches/esp_a2dp_api.h $FORK/components/bt/host/bluedroid/api/include/api/
cp fork_patches/a2dp_vendor_ldacbt_decoder.c $FORK/components/bt/host/bluedroid/stack/a2dp/
cp fork_patches/a2dp_vendor_lhdcv5_decoder.c $FORK/components/bt/host/bluedroid/stack/a2dp/
cp fork_patches/a2dp_vendor_mihc.c $FORK/components/bt/host/bluedroid/stack/a2dp/
cp fork_patches/a2dp_vendor_mihc_decoder.c $FORK/components/bt/host/bluedroid/stack/a2dp/
cp fork_patches/a2dp_vendor_mihc.h $FORK/components/bt/host/bluedroid/stack/include/stack/
cp fork_patches/a2dp_vendor_mihc_constants.h $FORK/components/bt/host/bluedroid/stack/include/stack/
```

### 3. 设置环境

```powershell
# Windows PowerShell
. <fork-idf-path>/export.ps1
```

### 4. 编译烧录

```powershell
# 必须用 PowerShell（不要 Git Bash）
cd bt_audio
idf.py build
idf.py -p COM4 flash        # 只烧 app 分区，保留 NVS
```

## MIHC 不可见时的排查顺序

**MIHC 在手机开发者选项里不出现**，按以下顺序查。**每一步都有硬判据，不要靠猜。**

### 第 1 步：capability 字节（最常见）

MIHC 的 capability 必须**恰好 15 字节**，且 `cap[11]` 的 **bit0 必须为 1**。

```c
// stack/a2dp/a2dp_vendor_mihc.c  A2DP_BuildInfoMihc()
*p_result++ = A2DP_MIHC_LATENCY_SUPPORTED |      // 0x01 <- cap[11] bit0
              (A2DP_MIHC_SRCLASS_48000 & A2DP_MIHC_CAP11_SRCLASS_MASK) |
              (A2DP_MIHC_DEPTHCLASS_16 & A2DP_MIHC_CAP11_DEPTH_MASK);
```

`cap[11] bit0` 是 APK 读的 `codecSpecific3`。**它为 0 时 APK 判「设备不支持低延迟」，
把 MIHC 从开发者选项整个排除**——与"长度必须 15"是**两道独立的门**，改对长度但漏了
这一位，MIHC 依然不出现。

**权威字节串**（2026-09-22 真机实测可选中）：

```
0F 01 FF 8F 03 00 00 31 4D 04 04 45 11 02 01
```

| cap | 值 | 含义 |
|---|---|---|
| [0] | `0F` | 长度 15（硬门：手机 `cmp #0xf; b.ne` 拒绝其他值）|
| [1] | `01` | media_type<<4 \| version |
| [2] | `FF` | NON_A2DP |
| [3..6] | `8F 03 00 00` | vendor LE = 0x038F |
| [7..8] | `31 4D` | codec LE = 0x4D31 |
| [9] | `04` | 采样率位图（48 kHz）|
| [10] | `04` | 位深位图（16 bit）|
| **[11]** | **`45`** | **bit0=1（codecSpecific3）** \| bits2-3=SRclass \| bits5-7=depthclass |
| [12] | `11` | 帧长档（low \| high）|
| [13] | `02` | 声道位图（stereo）|
| [14] | `01` | 尾档 |

**诊断**：手机端 `logcat` 搜 `mihc` / `can not support mihc` / `MIHC is disabled`。

### 第 2 步：SEP 数量上限

```c
// components/esp_bt_audio/src/classic/profiles/bt_audio_a2dp_sink.c
uint8_t seid = 0;
a2dp_sink_register_sbc_sep(seid++);      // seid 0
a2dp_sink_register_mihc_sep(seid++);     // seid 1  <- MIHC 必须靠前
...                                      // 其余顺延
```

`bta_av_api_reg_sep()` 拒绝 `seid >= BTA_AV_MAX_SEPS`，而 `BTA_AV_MAX_SEPS`
= `CONFIG_BT_A2DP_SEP_NUM_MAX`（Kconfig `range 1 12`，默认 2）。

- **`=2` 时只有前 2 个注册成功** → 手机只看到 **SBC + AAC**。
- MIHC 若排在最后（seid 9），任何截断都会先丢它 → **把它放在 SBC 之后（seid 1）**。
- `AVDT_NUM_SEPS` 的公式值 = 12（3 base + aptX3 + LDAC/Opus/LC3plus/LHDCV5/MIHC/AAC 各1），
  与本工程注册的 9 个一致。

**诊断**：**不要信 `A2DP sink: initialized with N SEPs`**——那个 `N` 是 `seid` 变量，
无论注册成败都自增，失败也照打。可信的判据只有**有没有 `Failed to register ... SEP` 行**。

### 第 3 步：`CONFIG_BT_A2DP_USE_EXTERNAL_CODEC`

**必须 `=y`**。它为 `n` 时栈把 `BTC_AV_EXT_CODEC` 定为 FALSE，`bta_av_api_reg_sep()`
被 `#if` 编译成一条空 `ret` —— **`esp_a2d_sink_register_stream_endpoint()` 的调用
全部被静默丢弃**，SEP 改由栈的内建表提供，而那张表里没有 MIHC。

**诊断**（无需上机，一条命令）：

```bash
riscv32-esp-elf-objdump -d build/bt_audio.elf | grep -A3 "<bta_av_api_reg_sep>:"
# 空壳:  8082  ret
# 正常:  7155  addi sp,sp,-208   (有函数体)
```

### 第 4 步：手机端属性

```bash
adb shell getprop persist.bluetooth.disable.mihc   # 必须 false
adb shell setprop persist.bluetooth.disable.mihc false
```

`persist.*` 刷机不清，但换机/恢复出厂会回退成 `true`。

## 关键配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `CONFIG_BT_A2DP_SEP_NUM_MAX` | 12 | 最大 SEP 数。Kconfig `range 1 12`；**2 时只有 SBC+AAC 可选中** |
| `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC` | **y** | **必须 y**。为 n 时 `bta_av_api_reg_sep()` 被编译成空函数，所有注册静默失效 |
| `CONFIG_ESP_BT_AUDIO_GMF_IO_SUPPORT` | y | GMF I/O 支持 |
| `CODEC_DAC_SAMPLE_RATE` | 48000 | I2S 输出采样率 |
| `CODEC_DAC_BITS_PER_SAMPLE` | 16 | I2S 位深 |

## 架构

```
A2DP Source (手机)
    ↓ BT
Bluedroid Stack (SBC/AAC/LDAC/LHDC/aptX/Opus/LC3plus/MIHC 解码)
    ↓ PCM
esp_bt_audio (raw_data_cb → data_q)
    ↓
GMF Pipeline: io_bt → aud_dec(PCM) → aud_asrc(48kHz) → io_codec_dev
    ↓
ES8389 DAC → Speaker (PA GPIO47)
```

## 已知限制

- 24bit I2S 输出有噪声（ES8389 MCLK 公式不匹配），当前使用 16bit
- LDAC 采样率动态切换时 ASRC 需要重配（当前通过 stream 重建处理）
- **MIHC 目前只能协商，不能出声**：协商已打通（手机可选中并开始推流），
  但 S31 侧的解码器是 LHDC V5 引擎的克隆，无法还原 MIHC 的容器封装，
  输出为噪声。待办是补上 MIHC 适配层：sync/container header 解析、CRC 层、
  运行时 `blocksize`、自适应声道模��。
  注意：MLAC 私有实现在 BES 固件里已被链接器 GC（移植无用），
  真正的路径是「项目自带的 LHDC 引擎 + 待补的 MIHC 适配层」。
  `MIHC_FRAMEDUMP` 已默认开启，用于抓取真实 over-the-air 帧。
- 手机端需 `adb shell setprop persist.bluetooth.disable.mihc false`
  并**删除旧配对记录**（蓝牙 SDP 缓存会复用旧的 capability），
  MIHC 才会出现在开发者选项的编解码器列表中。

## License

Apache-2.0
