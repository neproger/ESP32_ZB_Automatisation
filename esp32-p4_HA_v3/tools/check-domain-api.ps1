$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$expected = @{
    'crates/ha-app/src/main.rs::main' = 2
    'crates/ha-app/src/main.rs::handle_intake' = 6
    'crates/ha-app/src/zigbee_service.rs::run' = 2
    'crates/ha-app/src/zigbee_service.rs::register_domain' = 3
    'crates/ha-app/src/zigbee_service.rs::remove_device' = 1
    'crates/ha-domain/tests/domain_handle.rs::post_publishes_one_command_sent_on_ok' = 1
    'crates/ha-domain/tests/domain_handle.rs::post_error_publishes_no_command_sent' = 1
    'crates/ha-domain/tests/domain_handle.rs::command_sent_follows_executor_order' = 1
    'crates/ha-domain/tests/domain_handle.rs::post_from_with_mut_returns_invalid_state' = 1
    'crates/ha-domain/tests/domain_handle.rs::poisoned_domain_returns_invalid_state' = 1
}
$seen = @{}
$violations = @()

$scanRoots = @((Join-Path $root 'crates'))
Get-ChildItem -LiteralPath $scanRoots -Recurse -Filter '*.rs' |
    Where-Object { $_.FullName -notmatch '[\\/]generated[\\/]' } |
    ForEach-Object {
    $relative = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    $lines = @(Get-Content -LiteralPath $_.FullName)
    $text = $lines -join "`n"
    $offsets = @()
    $offset = 0
    foreach ($line in $lines) {
        $offsets += $offset
        $offset += $line.Length + 1
    }

    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -notmatch '\.with_mut\(') {
            continue
        }

        $function = $null
        for ($back = $index; $back -ge 0; $back--) {
            if ($lines[$back] -match '^\s*(?:pub\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)') {
                $function = $Matches[1]
                break
            }
        }
        if ($null -eq $function) {
            $violations += "${relative}:$($index + 1): with_mut outside a function"
            continue
        }

        $key = "${relative}::${function}"
        if (-not $expected.ContainsKey($key)) {
            $violations += "${relative}:$($index + 1): with_mut not allowlisted in $function"
        } else {
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = 0
            }
            $seen[$key]++
        }

        $start = $offsets[$index] + $lines[$index].IndexOf('.with_mut(')
        $open = $text.IndexOf('(', $start)
        $depth = 0
        $end = -1
        for ($cursor = $open; $cursor -lt $text.Length; $cursor++) {
            if ($text[$cursor] -eq '(') {
                $depth++
            } elseif ($text[$cursor] -eq ')') {
                $depth--
                if ($depth -eq 0) {
                    $end = $cursor
                    break
                }
            }
        }
        if ($end -lt 0) {
            $violations += "${relative}:$($index + 1): unterminated with_mut call"
            continue
        }
        $call = $text.Substring($start, $end - $start + 1)
        if ($call -match '\bpost\s*\(' -and
            $key -ne 'crates/ha-domain/tests/domain_handle.rs::post_from_with_mut_returns_invalid_state') {
            $violations += "${relative}:$($index + 1): post() inside with_mut"
        }
    }
}

foreach ($key in $expected.Keys) {
    $count = if ($seen.ContainsKey($key)) { $seen[$key] } else { 0 }
    if ($count -ne $expected[$key]) {
        $violations += "${key}: expected $($expected[$key]) with_mut call(s), found $count"
    }
}

$domainSource = Get-Content -LiteralPath (Join-Path $root 'crates/ha-domain/src/lib.rs') -Raw
if ($domainSource -match '(?s)impl DomainHandle\s*\{.*?pub fn (?:lock|with)\s*[<(]') {
    $violations += 'DomainHandle still exposes lock() or with()'
}

if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Output 'domain API allowlist ok'

