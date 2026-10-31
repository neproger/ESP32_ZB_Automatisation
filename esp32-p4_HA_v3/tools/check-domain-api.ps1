$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$allowedWithMut = @(
    'crates/ha-app/src/main.rs',
    'crates/ha-app/src/zigbee_service.rs',
    'crates/ha-domain/tests/domain_handle.rs'
)

$violations = @()
Get-ChildItem -LiteralPath (Join-Path $root 'crates') -Recurse -Filter '*.rs' | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    $matches = Select-String -LiteralPath $_.FullName -Pattern '\bdomain\.(lock|with)\('
    foreach ($match in $matches) {
        $violations += "${relative}:$($match.LineNumber): legacy DomainHandle access"
    }
    if ($allowedWithMut -notcontains $relative) {
        $matches = Select-String -LiteralPath $_.FullName -Pattern '\.with_mut\('
        foreach ($match in $matches) {
            $violations += "${relative}:$($match.LineNumber): with_mut outside allowlist"
        }
    }
}

if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Output 'domain API allowlist ok'
