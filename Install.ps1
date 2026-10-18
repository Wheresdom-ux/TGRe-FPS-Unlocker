param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$gameFolder = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$gameExe = Join-Path $gameFolder 'CALL to EXIST.exe'
if (!(Test-Path -LiteralPath $gameExe)) { throw 'Put FPS Patch inside the CALL to EXIST game folder.' }
$active = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq 'CALL to EXIST' -and $_.Path -eq $gameExe }
if ($active) { throw 'Close CALL to EXIST before installing or removing the patch.' }
$destination = Join-Path $gameFolder 'winmm.dll'
$source = Join-Path $PSScriptRoot 'build\winmm.dll'
if (!$Uninstall -and !(Test-Path -LiteralPath $source)) { throw 'Build winmm.dll with build.cmd first.' }
$ledger = Join-Path $PSScriptRoot 'install-state.json'
$previous = if (Test-Path -LiteralPath $ledger) { Get-Content -LiteralPath $ledger -Raw | ConvertFrom-Json } else { $null }
if (Test-Path -LiteralPath $destination) {
    $existingHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if (!$previous -or $previous.installedDllSHA256 -ne $existingHash) { throw 'A different winmm.dll already exists. It has been left untouched.' }
    $backupFolder = Join-Path $PSScriptRoot 'backups'
    New-Item -ItemType Directory -Path $backupFolder -Force | Out-Null
    $backup = Join-Path $backupFolder ('winmm-' + [guid]::NewGuid().ToString('N') + '.dll')
    # Only this exact, hash-verified DLL is moved; no recursive file operations.
    Move-Item -LiteralPath $destination -Destination $backup
}
if ($Uninstall) { Write-Host 'FPS proxy removed. The INI and logs have been kept.'; exit }
Copy-Item -LiteralPath $source -Destination $destination
$ini = Join-Path $gameFolder 'CTE_FPSPatch.ini'
if (!(Test-Path -LiteralPath $ini)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CTE_FPSPatch.ini') -Destination $ini }
@{
    installedDllSHA256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    gameExeSHA256 = (Get-FileHash -LiteralPath $gameExe -Algorithm SHA256).Hash
    installedAt = [DateTimeOffset]::Now.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath $ledger -Encoding UTF8
Write-Host 'FPS proxy installed. Launch through Steam; press F10 for live FPS settings.'
