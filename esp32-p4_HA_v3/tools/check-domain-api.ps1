$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$expected = @{
    'crates/ha-app/src/main.rs::main' = 1
    'crates/ha-app/src/main.rs::handle_intake' = 2
    'crates/ha-app/src/zigbee_service.rs::run' = 0
    'crates/ha-app/src/zigbee_service.rs::register_domain' = 3
    'crates/ha-app/src/zigbee_service.rs::apply_capabilities' = 1
    'crates/ha-app/src/zigbee_service.rs::remove_device' = 0
    'crates/ha-domain/tests/domain_handle.rs::post_publishes_one_command_sent_on_ok' = 1
    'crates/ha-domain/tests/domain_handle.rs::post_error_publishes_no_command_sent' = 1
    'crates/ha-domain/tests/domain_handle.rs::command_sent_follows_executor_order' = 1
    'crates/ha-domain/tests/domain_handle.rs::post_from_with_mut_returns_invalid_state' = 1
    'crates/ha-domain/tests/domain_handle.rs::poisoned_domain_returns_invalid_state' = 1
    'crates/ha-domain/tests/domain_handle.rs::with_mut_inside_transaction_returns_invalid_state' = 1
    'crates/ha-domain/tests/domain_handle.rs::transaction_inside_with_mut_returns_invalid_state' = 1
}
$seen = @{}
$violations = @()

function Add-Violation {
    param([string]$Message)
    $script:violations += $Message
}

function Get-Source {
    param([string]$RelativePath)
    $path = Join-Path $script:root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Violation "${RelativePath}: file is missing"
        return $null
    }
    return [System.IO.File]::ReadAllText($path)
}

function Get-RustFunction {
    param([string]$Source, [string]$Name, [string]$SignatureContains = '')
    if ($null -eq $Source) {
        return $null
    }
    $pattern = '(?m)^[ \t]*(?:pub(?:\s*\([^)]*\))?\s+)?(?:const\s+)?(?:async\s+)?fn\s+' +
        [regex]::Escape($Name) +
        '\s*(?:<[^>]*>)?\s*\('
    foreach ($match in [regex]::Matches($Source, $pattern)) {
        $open = $Source.IndexOf('{', $match.Index)
        if ($open -lt 0) {
            continue
        }
        $depth = 0
        $end = -1
        for ($index = $open; $index -lt $Source.Length; $index++) {
            if ($Source[$index] -eq '{') {
                $depth++
            } elseif ($Source[$index] -eq '}') {
                $depth--
                if ($depth -eq 0) {
                    $end = $index
                    break
                }
            }
        }
        if ($end -lt 0) {
            continue
        }
        $signature = $Source.Substring($match.Index, $open - $match.Index)
        if ([string]::IsNullOrEmpty($SignatureContains) -or ($signature -match $SignatureContains)) {
            return [pscustomobject]@{
                Signature = $signature
                Body = $Source.Substring($match.Index, $end - $match.Index + 1)
            }
        }
    }
    return $null
}

function Test-PublicFunction {
    param([string]$Signature)
    return ($Signature -match '^\s*pub\s+fn\b')
}

function Check-EntitySnapshotBoundary {
    $relative = 'crates/ha-domain/src/entity.rs'
    $source = Get-Source $relative
    if ($null -ne $source) {
        foreach ($name in @('entity_iter', 'entity_snapshot')) {
            $function = Get-RustFunction $source $name
            if (($null -ne $function) -and (Test-PublicFunction $function.Signature)) {
                Add-Violation "${relative}: public Domain::$name is forbidden"
            }
        }
        $function = Get-RustFunction $source 'snapshot'
        if (($null -ne $function) -and (Test-PublicFunction $function.Signature)) {
            Add-Violation "${relative}: public Domain::snapshot is forbidden"
        }
    }

    foreach ($check in @(
        @{ Path = 'crates/ha-domain/src/handle.rs'; Owner = 'DomainHandle' },
        @{ Path = 'crates/ha-domain/src/transaction.rs'; Owner = 'DomainTransaction' }
    )) {
        $candidate = Get-Source $check.Path
        $function = Get-RustFunction $candidate 'entity_snapshot'
        if (($null -eq $function) -or (-not (Test-PublicFunction $function.Signature))) {
            Add-Violation "$($check.Path): $($check.Owner) must expose public entity_snapshot"
        }
    }
}

function Check-AdapterRemovalApis {
    $relative = 'crates/zigbee-devices/src/domain_adapter.rs'
    $source = Get-Source $relative
    if ($null -ne $source) {
        foreach ($name in @('remove_device_records', 'remove_group', 'remove_automation')) {
            $function = Get-RustFunction $source $name
            if (($null -ne $function) -and (Test-PublicFunction $function.Signature)) {
                Add-Violation "${relative}: DomainAdapter::$name is forbidden"
            }
        }
    }

    Get-ChildItem -LiteralPath (Join-Path $script:root 'crates') -Recurse -Filter '*.rs' |
        Where-Object { $_.FullName -notmatch '[\\/]generated[\\/]' } |
        ForEach-Object {
            $scanRelative = $_.FullName.Substring($script:root.Length + 1).Replace('\', '/')
            $text = [System.IO.File]::ReadAllText($_.FullName)
            if ($text -match 'DomainAdapter\s*::\s*remove_(device_records|group|automation)\s*\(') {
                Add-Violation "${scanRelative}: call to removed DomainAdapter removal API"
            }
            if ($text -match '(?i)\b(?:adapter|domain_adapter)\s*\.\s*remove_(device_records|group|automation)\s*\(') {
                Add-Violation "${scanRelative}: call to removed adapter removal API"
            }
            if ($text -match '(?i)\.\s*remove_(device_records|group|automation)\s*\(') {
                Add-Violation "${scanRelative}: call to removed removal API"
            }
        }
}

function Check-DeviceRemoval {
    $relative = 'crates/ha-device/src/lib.rs'
    $source = Get-Source $relative
    if ($null -eq $source) {
        return
    }

    $prepare = Get-RustFunction $source 'prepare_remove'
    if ($null -eq $prepare) {
        Add-Violation "${relative}: prepare_remove is missing"
    } else {
        if ($prepare.Signature -notmatch '&\s*DomainHandle\b') {
            Add-Violation "${relative}: prepare_remove must take &DomainHandle"
        }
        if ($prepare.Signature -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: prepare_remove must not take &Domain or &mut Domain"
        }
        if ($prepare.Body -notmatch 'domain\.transaction\s*::') {
            Add-Violation "${relative}: prepare_remove must use DomainHandle::transaction"
        }
        if (($prepare.Body -notmatch 'transaction\.entity_snapshot') -or
            ($prepare.Body -notmatch 'transaction\.entity_meta')) {
            Add-Violation "${relative}: prepare_remove must read through DomainTransaction"
        }
        if ($prepare.Body -match '\.with_mut\s*\(') {
            Add-Violation "${relative}: prepare_remove must not use with_mut"
        }
        if ($prepare.Body -match 'domain\.entity_') {
            Add-Violation "${relative}: prepare_remove must not call Domain entity methods directly"
        }
        if ($prepare.Body -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: prepare_remove must not use &Domain or &mut Domain"
        }
    }

    $validate = Get-RustFunction $source 'validate_plan'
    if ($null -eq $validate) {
        Add-Violation "${relative}: validate_plan is missing"
    } else {
        if ($validate.Signature -notmatch '&\s*DomainTransaction\b') {
            Add-Violation "${relative}: validate_plan must take &DomainTransaction"
        }
        if (($validate.Body -notmatch 'transaction\.entity_meta') -or
            ($validate.Body -notmatch 'transaction\.entity_snapshot')) {
            Add-Violation "${relative}: validate_plan must read through DomainTransaction"
        }
        if ($validate.Body -match '\.with_mut\s*\(') {
            Add-Violation "${relative}: validate_plan must not use with_mut"
        }
        if ($validate.Body -match 'domain\.entity_') {
            Add-Violation "${relative}: validate_plan must not call Domain entity methods directly"
        }
        if ($validate.Body -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: validate_plan must not use &Domain or &mut Domain"
        }
    }

    $remove = Get-RustFunction $source 'remove_canonical'
    if ($null -eq $remove) {
        Add-Violation "${relative}: remove_canonical is missing"
    } else {
        if ($remove.Signature -notmatch '&\s*DomainHandle\b') {
            Add-Violation "${relative}: remove_canonical must take &DomainHandle"
        }
        if ($remove.Signature -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: remove_canonical must not take &Domain or &mut Domain"
        }
        if ($remove.Body -match '\.with_mut\s*\(') {
            Add-Violation "${relative}: remove_canonical must not use with_mut"
        }
        if ($remove.Body -match 'domain\.entity_') {
            Add-Violation "${relative}: remove_canonical must not call Domain entity methods directly"
        }
        if ($remove.Body -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: remove_canonical must not use &Domain or &mut Domain"
        }
        $transactionAt = $remove.Body.IndexOf('.transaction')
        $validationAt = $remove.Body.IndexOf('validate_plan(transaction')
        $mutationAt = $remove.Body.IndexOf('transaction.entity_remove')
        $progressAt = $remove.Body.IndexOf('progress.mark')
        if (($transactionAt -lt 0) -or ($validationAt -lt $transactionAt) -or
            ($mutationAt -lt $validationAt) -or ($progressAt -lt $mutationAt)) {
            Add-Violation "${relative}: remove_canonical must validate and mutate inside one transaction before marking progress"
        }
    }
}

function Check-ConfigLayer {
    $relative = 'crates/ha-app/src/config.rs'
    $source = Get-Source $relative
    if ($null -eq $source) {
        return
    }

    foreach ($item in @(
        @{ Public = 'remove_group'; Plan = 'remove_group_plan' },
        @{ Public = 'remove_automation'; Plan = 'remove_automation_plan' }
    )) {
        $public = Get-RustFunction $source $item.Public
        if ($null -eq $public) {
            Add-Violation "${relative}: $($item.Public) is missing"
            continue
        }
        if ($public.Signature -cmatch '\b(?:Fn|FnMut|FnOnce)\b') {
            Add-Violation "${relative}: $($item.Public) must not accept persistence callbacks"
        }
        if ($public.Signature -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: $($item.Public) must not accept Domain"
        }
        if ($public.Body -notmatch ([regex]::Escape($item.Plan) + '\s*\(')) {
            Add-Violation "${relative}: $($item.Public) must use $($item.Plan)"
        }
        if ($public.Body -match 'store::(?:save|remove)_domain') {
            Add-Violation "${relative}: $($item.Public) must not use a second persistence path"
        }

        $plan = Get-RustFunction $source $item.Plan
        if ($null -eq $plan) {
            Add-Violation "${relative}: $($item.Plan) is missing"
            continue
        }
        if ($plan.Signature -match '&\s*(?:mut\s+)?Domain\b') {
            Add-Violation "${relative}: $($item.Plan) must not accept Domain"
        }
        if ($plan.Body -notmatch 'domain\.transaction\s*::') {
            Add-Violation "${relative}: $($item.Plan) must use DomainHandle::transaction"
        }
        if ($plan.Body -notmatch 'transaction\.entity_snapshot') {
            Add-Violation "${relative}: $($item.Plan) must use DomainTransaction snapshot"
        }
        if ($plan.Body -notmatch 'remove_record_cascade\s*\(\s*transaction') {
            Add-Violation "${relative}: $($item.Plan) must remove through DomainTransaction"
        }
        if ($plan.Body -match '\.with_mut\s*\(') {
            Add-Violation "${relative}: $($item.Plan) must not use with_mut"
        }
    }

    $removeRecord = Get-RustFunction $source 'remove_record_cascade'
    if (($null -eq $removeRecord) -or ($removeRecord.Body -notmatch 'transaction\.entity_remove')) {
        Add-Violation "${relative}: remove_record_cascade must use DomainTransaction.entity_remove"
    }

    if ($source -match '\.with_mut\s*\(') {
        Add-Violation "${relative}: config layer must not use with_mut"
    }

    foreach ($pattern in @('DomainAdapter', '\bmicro_db\b', '\bTable\b', '\bStorage\b', 'StoreSink', 'RecordSink')) {
        if ($source -match $pattern) {
            Add-Violation "${relative}: forbidden dependency/pattern '$pattern'"
        }
    }
}

function Check-ZigbeeRemovalSequence {
    $relative = 'crates/ha-app/src/zigbee_service.rs'
    $source = Get-Source $relative
    if ($null -eq $source) {
        return
    }
    $function = Get-RustFunction $source 'remove_device' '&\s*DomainHandle\b'
    if ($null -eq $function) {
        Add-Violation "${relative}: remove_device(&DomainHandle, ...) is missing"
        return
    }
    $prepareAt = $function.Body.IndexOf('ha_device::prepare_remove')
    $invokeAt = $function.Body.IndexOf('ha_device::invoke_remover')
    $removeAt = $function.Body.IndexOf('ha_device::remove_canonical')
    if (($prepareAt -lt 0) -or ($invokeAt -lt 0) -or ($removeAt -lt 0) -or
        ($prepareAt -ge $invokeAt) -or ($invokeAt -ge $removeAt)) {
        Add-Violation "${relative}: expected prepare_remove -> invoke_remover -> remove_canonical order"
    }
    if (($function.Body -match '\.transaction\s*::') -or ($function.Body -match '\.with_mut\s*\(')) {
        Add-Violation "${relative}: Zigbee remover must stay outside Domain transactions"
    }
}

function Check-MaintenanceBoundary {
    $handle = Get-Source 'crates/ha-domain/src/handle.rs'
    if (($null -ne $handle) -and ($handle -match 'maintenance')) {
        Add-Violation 'crates/ha-domain/src/handle.rs: DomainHandle must not expose maintenance'
    }

    foreach ($relative in @(
        'crates/ha-device/src/lib.rs',
        'crates/zigbee-devices/src',
        'crates/ha-app/src/config.rs'
    )) {
        $path = Join-Path $script:root $relative
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }
        $files = if ((Get-Item -LiteralPath $path).PSIsContainer) {
            Get-ChildItem -LiteralPath $path -Recurse -Filter '*.rs'
        } else {
            Get-Item -LiteralPath $path
        }
        foreach ($file in $files) {
            $text = [System.IO.File]::ReadAllText($file.FullName)
            if ($text -match 'take_maintenance|DomainMaintenance|SharedStorage') {
                $scanRelative = $file.FullName.Substring($script:root.Length + 1).Replace('\', '/')
                Add-Violation "${scanRelative}: service code must not access maintenance capability"
            }
        }
    }
}

function Check-Stage2Boundary {
    $adapterRelative = 'crates/zigbee-devices/src/domain_adapter.rs'
    $adapter = Get-Source $adapterRelative
    if ($null -ne $adapter) {
        foreach ($pattern in @(
            'RecordSink',
            'StoreSink',
            'save_domain',
            'remove_domain',
            'register_entity',
            'EntityDesc',
            'ensure_device',
            'ensure_entity',
            'ensure_standard_entity',
            'write_capabilities',
            'read_entity',
            'read_device',
            'read_capabilities',
            'put_record',
            'remove_record',
            'rename_entity',
            'rename_device'
        )) {
            if ($adapter -match [regex]::Escape($pattern)) {
                Add-Violation "${adapterRelative}: Zigbee projection still contains '$pattern'"
            }
        }
        foreach ($name in @('write_state', 'read_state')) {
            $function = Get-RustFunction $adapter $name
            if ($null -eq $function) {
                Add-Violation "${adapterRelative}: $name is missing"
            } elseif ($function.Signature -notmatch '&\s*DomainHandle\b') {
                Add-Violation "${adapterRelative}: $name must take &DomainHandle"
            }
        }
    }

    $deviceRelative = 'crates/ha-device/src/lib.rs'
    $device = Get-Source $deviceRelative
    if ($null -ne $device) {
        foreach ($pattern in @('RecordSink', 'StoreSink', 'save_domain', 'remove_domain')) {
            if ($device -match [regex]::Escape($pattern)) {
                Add-Violation "${deviceRelative}: Device service still contains '$pattern'"
            }
        }
    }

    Get-ChildItem -LiteralPath (Join-Path $script:root 'crates/zigbee-devices/src') -Recurse -Filter '*.rs' |
        Where-Object { $_.FullName -notmatch '[\\/]generated[\\/]' } |
        ForEach-Object {
            $relative = $_.FullName.Substring($script:root.Length + 1).Replace('\', '/')
            $text = [System.IO.File]::ReadAllText($_.FullName)
            foreach ($pattern in @('register_entity', 'EntityDesc', 'put_record', 'remove_record')) {
                if ($text -match [regex]::Escape($pattern)) {
                    Add-Violation "${relative}: Zigbee production code still contains '$pattern'"
                }
            }
        }
}

$script:root = $root
$script:violations = $violations

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
            Add-Violation "${relative}:$($index + 1): with_mut outside a function"
            continue
        }

        $key = "${relative}::${function}"
        if (-not $expected.ContainsKey($key)) {
            Add-Violation "${relative}:$($index + 1): with_mut not allowlisted in $function"
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
            Add-Violation "${relative}:$($index + 1): unterminated with_mut call"
            continue
        }
        $call = $text.Substring($start, $end - $start + 1)
        if ($call -match '\bpost\s*\(' -and
            $key -ne 'crates/ha-domain/tests/domain_handle.rs::post_from_with_mut_returns_invalid_state') {
            Add-Violation "${relative}:$($index + 1): post() inside with_mut"
        }
    }
}

foreach ($key in $expected.Keys) {
    $count = if ($seen.ContainsKey($key)) { $seen[$key] } else { 0 }
    if ($count -ne $expected[$key]) {
        Add-Violation "${key}: expected $($expected[$key]) with_mut call(s), found $count"
    }
}

$domainSource = Get-Content -LiteralPath (Join-Path $root 'crates/ha-domain/src/lib.rs') -Raw
if ($domainSource -match '(?s)impl DomainHandle\s*\{.*?pub fn (?:lock|with)\s*[<(]') {
    Add-Violation 'DomainHandle still exposes lock() or with()'
}

Check-EntitySnapshotBoundary
Check-AdapterRemovalApis
Check-DeviceRemoval
Check-ConfigLayer
Check-ZigbeeRemovalSequence
Check-Stage2Boundary

if ($script:violations.Count -ne 0) {
    $script:violations | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Output 'domain API checks ok'
