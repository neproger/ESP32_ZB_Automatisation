# Validates V2 struct layout with the P4 toolchain and regenerates byte fixtures.
param(
    [string]$CCompiler
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$v3 = Split-Path -Parent (Split-Path -Parent $here)
$workspace = Split-Path -Parent $v3
$v2Include = Join-Path $workspace 'esp32-p4_HA_v2\ha_model\include'
$fixtureDir = Join-Path $v3 'crates\ha-model\tests\fixtures'

if (-not (Test-Path -LiteralPath $v2Include)) {
    throw "V2 include directory not found: $v2Include"
}

if (-not $CCompiler) {
    $toolRoot = Join-Path $env:USERPROFILE '.espressif\tools\riscv32-esp-elf'
    $CCompiler = Get-ChildItem -LiteralPath $toolRoot -Recurse -Filter riscv32-esp-elf-gcc.exe |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $CCompiler -or -not (Test-Path -LiteralPath $CCompiler)) {
    throw "riscv32-esp-elf-gcc not found; pass -CCompiler <path>"
}

$objcopy = Join-Path (Split-Path -Parent $CCompiler) 'riscv32-esp-elf-objcopy.exe'
if (-not (Test-Path -LiteralPath $objcopy)) {
    throw "riscv32-esp-elf-objcopy not found next to $CCompiler"
}

$temp = Join-Path $env:TEMP ('ha-v2-abi-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    & $CCompiler -std=c11 -Wall -Wextra -Werror -march=rv32imafc -mabi=ilp32f `
        -I $v2Include -c (Join-Path $here 'v2_abi_probe.c') -o (Join-Path $temp 'probe.o')
    if ($LASTEXITCODE -ne 0) { throw 'V2 ABI probe failed' }

    & $CCompiler -std=c11 -march=rv32imafc -mabi=ilp32f `
        -I $v2Include -c (Join-Path $here 'v2_abi_fixtures.c') -o (Join-Path $temp 'fixtures.o')
    if ($LASTEXITCODE -ne 0) { throw 'V2 fixture build failed' }

    New-Item -ItemType Directory -Force -Path $fixtureDir | Out-Null
    $sections = [ordered]@{
        rule = 456
        group_key = 8
        group_record = 32
        group_item_key = 16
        group_item_record = 36
        group_automation_key = 16
        automation_key = 8
    }
    foreach ($entry in $sections.GetEnumerator()) {
        $output = Join-Path $fixtureDir ($entry.Key + '.bin')
        $section = '.abi_' + $entry.Key
        & $objcopy -O binary "--only-section=$section" `
            (Join-Path $temp 'fixtures.o') $output
        if ($LASTEXITCODE -ne 0) { throw "objcopy failed for $($entry.Key)" }
        $length = (Get-Item -LiteralPath $output).Length
        if ($length -ne $entry.Value) {
            throw "fixture $($entry.Key): expected $($entry.Value) bytes, got $length"
        }
    }
    Write-Host "V2 ABI probe passed; fixtures written to $fixtureDir"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
