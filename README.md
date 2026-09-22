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

将 `fork_patches/` 中的文件覆盖到 fork IDF 的对应位置：

```bash
FORK=ESP32-S31-A2DP-Codecs/esp-idf-v6.1-codecs
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

```bash
cd bt_audio
idf.py build
idf.py -p COM4 flash
```

## 关键配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `CONFIG_BT_A2DP_SEP_NUM_MAX` | 10 | 最大 SEP 数（SBC/AAC/aptX×3/LDAC/Opus/LC3plus/LHDC V5/MIHC）|
| `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC` | n | 栈内解码（vendor decoder 在 stack/a2dp/ 内）|
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
- **MIHC 只能协商，无法出声**：S31 侧无 MLAC 解码器（BES 闭源），
  协商成功后音频路径回落到 LHDC 引擎，MIHC 帧会输出噪声。
  当前价值在于 bring-up（可抓取 MIHC 原始帧），非播放。
- 手机端需 `adb shell setprop persist.bluetooth.disable.mihc false`
  并**删除旧配对记录**（蓝牙 SDP 缓存会复用旧的 capability），
  MIHC 才会出现在开发者选项的编解码器列表中。

## License

Apache-2.0
