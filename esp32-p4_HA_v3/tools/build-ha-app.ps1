# Reproducible ESP32-P4 build with the pinned ESP-IDF v6.1 toolchain.
# The normal path reuses the CMake cache; if it is stale, the script removes
# only the esp-idf-sys CMake build and retries once. It never deletes flash or
# the application sources.
$ErrorActionPreference = 'Continue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$app = Join-Path $root 'crates\ha-app'
$target = 'C:/t/hav3'
$idf = 'C:\Users\ASUS\.espressif\esp-idf\v6.1'
$tools = 'C:\Users\ASUS\.espressif'
$python = Join-Path $tools 'python_env\idf6.1_py3.10_env'
$cmake = Join-Path $tools 'tools\cmake\4.0.3\bin\cmake.exe'

$env:IDF_PATH = $idf
$env:IDF_TOOLS_PATH = $tools
$env:IDF_PYTHON_ENV_PATH = $python
# esp-idf-sys stores components_<target>.lock under PROJECT_DIR (its CMake pass
# needs the same value; missing it yields the unwritable path \components_*.lock).
$env:PROJECT_DIR = $app.Replace('\', '/')
# Same as [package.metadata.esp-idf-sys.extra_components]; without it the manual
# CMake pass silently drops native_net/native_zigbee from the build.
$env:EXTRA_COMPONENT_DIRS = @(
    (Join-Path $app 'native_zigbee'),
    (Join-Path $app 'native_net')
) -join ';'
$env:EXTRA_COMPONENT_DIRS = $env:EXTRA_COMPONENT_DIRS.Replace('\', '/')
# CMake re-run in the cached esp-idf-sys dir needs the target to match.
$env:IDF_TARGET = 'esp32p4'
Remove-Item Env:ESP_IDF_TOOLS_INSTALL_DIR -ErrorAction SilentlyContinue
$env:LIBCLANG_PATH = 'C:\Users\ASUS\AppData\Local\Programs\Python\Python310\lib\site-packages\clang\native'

# Wi-Fi credentials come from the caller's environment, never from the repo.
if (-not $env:HA_WIFI_SSID -or -not $env:HA_WIFI_PASSWORD) {
    Write-Warning 'HA_WIFI_SSID/HA_WIFI_PASSWORD are not set; building without Wi-Fi.'
}

# The UI bundle is generated (web/ui is gitignored) but included by Rust.
$ui = Join-Path $root 'web-ui'
Push-Location $ui
try {
    if (-not (Test-Path (Join-Path $ui 'node_modules'))) {
        & npm ci
        if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' }
    }
    & npm run build
    if ($LASTEXITCODE -ne 0) { throw 'web UI build failed' }
} finally {
    Pop-Location
}

function Build-Sys([string]$buildRoot) {
    $out = Join-Path $buildRoot 'out\build'
    if ((Test-Path $out) -and (Test-Path $cmake)) {
        & $cmake --build $out | Out-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "C bridge CMake pass failed; cargo will regenerate: $out"
        }
    }
}

function Run-Cargo {
    Push-Location $app
    try {
        & cargo build | Out-Host
        return $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

$sysBuilds = @(Get-ChildItem (Join-Path $target 'riscv32imafc-esp-espidf\debug\build') -Directory -Filter 'esp-idf-sys-*' -ErrorAction SilentlyContinue)
foreach ($build in $sysBuilds) { Build-Sys $build.FullName }

if ((Run-Cargo) -eq 0) { exit 0 }

Write-Warning 'Build failed; dropping stale esp-idf-sys CMake cache and retrying.'
foreach ($build in $sysBuilds) { Remove-Item -Recurse -Force $build.FullName -ErrorAction SilentlyContinue }
if ((Run-Cargo) -eq 0) { exit 0 }

Write-Warning 'Retry failed; dropping the whole target cache once more.'
Remove-Item -Recurse -Force $target -ErrorAction SilentlyContinue
if ((Run-Cargo) -eq 0) { exit 0 }
exit 1
