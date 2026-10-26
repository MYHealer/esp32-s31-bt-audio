# 一键把 esp_codec_dev STD 候选 slot_bit 固定 >= 32 的补丁应用到 managed_components。
# 方案A (I2S slot=32bit 白噪修复)。幂等: 已应用则跳过; 未应用且干净则应用。
# 用法: 在工程根 dot-source 或直接执行。构建前调用, 确保组件管理器 clean 后仍生效。
$ErrorActionPreference = 'Stop'

$ROOT = 'E:/ESP/43_s31_code/43_s31_code/bt_audio'
$PATCH = "$ROOT/fork_patches/esp_codec_dev__slot32.patch"
$TARGET = "$ROOT/managed_components/espressif__esp_codec_dev/src/codec_dev_i2s_bus.c"
$PATCH_DIR = "$ROOT/managed_components"

if (-not (Test-Path $TARGET)) {
    Write-Host "[slot32] TARGET missing: $TARGET (managed_components not fetched?)" -ForegroundColor Yellow
    exit 0
}

$content = Get-Content $TARGET -Raw
if ($content -match 'slot_start') {
    Write-Host "[slot32] already applied, skip." -ForegroundColor Green
    exit 0
}

Push-Location $PATCH_DIR
try {
    git apply --check --verbose $PATCH
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[slot32] check failed, target may be dirty. Manual resolve needed." -ForegroundColor Red
        exit 1
    }
    git apply --verbose $PATCH
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[slot32] apply failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "[slot32] applied OK." -ForegroundColor Green
} finally {
    Pop-Location
}