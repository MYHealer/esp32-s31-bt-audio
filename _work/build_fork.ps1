# bt_audio (fork esp-idf-v6.1-codecs) 编译脚本
# 依据记忆 esp32-s31-toolchain.md：fork IDF 无现成 env 脚本，须手工还原 6 变量。
# 坑 6：Git Bash 调 PowerShell 时 MSYSTEM 会泄漏 -> idf.py 秒退，必须先清。

Remove-Item Env:\MSYSTEM    -ErrorAction SilentlyContinue
Remove-Item Env:\MSYSCON    -ErrorAction SilentlyContinue
Remove-Item Env:\MINGW_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:\MINGW_CHOST  -ErrorAction SilentlyContinue

$env:IDF_PATH            = 'E:\ESP\43_s31_code\43_s31_code\ESP32-S31-A2DP-Codecs-main\ESP32-S31-A2DP-Codecs-main\esp-idf-v6.1-codecs'
$env:IDF_TOOLS_PATH      = 'E:\ESP\.espressif'
$env:IDF_PYTHON_ENV_PATH = 'E:\ESP\.espressif\python_env\idf6.1_py3.14_env'
$env:ESP_ROM_ELF_DIR     = 'E:\ESP\.espressif\tools\esp-rom-elfs'
$env:ESP_IDF_VERSION     = '6.1.0'          # ★ 最容易漏；漏掉报 component_manager TypeError
$env:PYTHONUTF8          = '1'

# 坑：PowerShell 里 @(...) -join ';' + ';' + $x 的优先级会粘连路径，必须显式括号
$env:PATH = ((
  'E:\ESP\.espressif\tools\cmake\3.30.2\bin',
  'E:\ESP\.espressif\tools\ninja\1.12.1',
  'E:\ESP\.espressif\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin',
  'E:\ESP\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin'
) -join ';') + ';' + $env:PATH

$PY = 'E:\ESP\.espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe'
$IDFPY = Join-Path $env:IDF_PATH 'tools\idf.py'

Set-Location 'E:\ESP\43_s31_code\43_s31_code\bt_audio'

Write-Host "=== IDF_PATH = $env:IDF_PATH"
Write-Host "=== ESP_IDF_VERSION = $env:ESP_IDF_VERSION"
Write-Host "=== MSYSTEM = '$env:MSYSTEM'"
Write-Host ""

# 坑：idf.py 用管道会让 $LASTEXITCODE 被覆盖成 0（假成功）-> 用重定向
& $PY $IDFPY build > 'E:\ESP\43_s31_code\43_s31_code\bt_audio\_work\build.log' 2>&1
Write-Host "IDF_EXIT: $LASTEXITCODE"
