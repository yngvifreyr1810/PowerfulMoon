param(
    [ValidateSet('Install','Play','Restore')][string]$Mode='Install',
    [string]$ExecutablePath='',
    [string]$BridgeDirectory=''
)
$ErrorActionPreference='Stop'
$env:PSModulePath = (Join-Path $PSHOME 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
$taskRoot=$PSScriptRoot
$settingsPath=Join-Path $taskRoot '.local-settings.json'
function Select-Program([string]$Title,[string]$FileName) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog=[Windows.Forms.OpenFileDialog]::new()
    $dialog.Title=$Title;$dialog.Filter="$FileName|$FileName";$dialog.CheckFileExists=$true
    try { if ($dialog.ShowDialog() -eq 'OK') { return $dialog.FileName } } finally { $dialog.Dispose() }
    throw 'No program selected. Nothing changed.'
}
function Read-LocalSettings {
    if (Test-Path -LiteralPath $settingsPath) { return Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json }
    return [pscustomobject]@{exe='';bridge=''}
}
function Test-BridgeRunning {
    $client=[Net.Sockets.TcpClient]::new()
    try { return $client.ConnectAsync('127.0.0.1',26759).Wait(400) -and $client.Connected }
    catch { return $false } finally { $client.Dispose() }
}
try {
    $local=Read-LocalSettings
    if (-not $ExecutablePath) { $ExecutablePath=$local.exe }
    if (-not $ExecutablePath -or -not (Test-Path -LiteralPath $ExecutablePath)) {
        $ExecutablePath=Select-Program 'Choose your existing Power Moon Recomp 0.1.6 program' 'PowerMoonRecomp.exe'
    }
    $ExecutablePath=(Resolve-Path -LiteralPath $ExecutablePath).Path
    if ([IO.Path]::GetFileName($ExecutablePath) -ne 'PowerMoonRecomp.exe') { throw 'Choose PowerMoonRecomp.exe.' }
    $gameRoot=[IO.Path]::GetDirectoryName($ExecutablePath)
    $local.exe=$ExecutablePath
    if ($Mode -eq 'Play') {
        if (-not (Test-BridgeRunning)) {
            if (-not $BridgeDirectory) { $BridgeDirectory=$local.bridge }
            if (-not $BridgeDirectory -or -not (Test-Path -LiteralPath (Join-Path $BridgeDirectory 'START.cmd'))) {
                $bridgeExe=Select-Program 'Choose ProCon2Bridge.exe from the extracted bridge download' 'ProCon2Bridge.exe'
                $BridgeDirectory=[IO.Path]::GetDirectoryName($bridgeExe)
            }
            $local.bridge=$BridgeDirectory
            # START.cmd handles the first signed-driver setup and clear status messages.
            Start-Process -FilePath (Join-Path $BridgeDirectory 'START.cmd') -WorkingDirectory $BridgeDirectory -WindowStyle Hidden
            for ($attempt=0;$attempt -lt 30 -and -not (Test-BridgeRunning);$attempt++) { Start-Sleep -Milliseconds 500 }
            if (-not (Test-BridgeRunning)) { throw 'Bridge not ready. Run its START.cmd once and finish driver setup, then use PLAY.cmd again.' }
        }
        $local | ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding UTF8
        Start-Process -FilePath $ExecutablePath -WorkingDirectory $gameRoot -WindowStyle Normal
        Write-Host 'Started. Press a controller button to reconnect. F1 > Controls contains motion and vibration settings.'
        exit 0
    }
    if (Get-Process PowerMoonRecomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $ExecutablePath }) { throw 'Save and close Power Moon Recomp before installing or restoring.' }
    if ($Mode -eq 'Restore') {
        $backupRoot=Join-Path $gameRoot 'controller-backups'
        $backup=Get-ChildItem -LiteralPath $backupRoot -Directory | Sort-Object Name -Descending | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'backup.json') } | Select-Object -First 1
        if (-not $backup) { throw 'No complete controller update backup found.' }
        $records=Get-Content -LiteralPath (Join-Path $backup.FullName 'backup.json') -Raw | ConvertFrom-Json
        foreach ($record in $records) {
            if ($record.name -notin @('PowerMoonRecomp.exe','libc++.dll')) { throw 'Unrecognized backup entry.' }
            if ($record.existed -and (Get-FileHash -LiteralPath (Join-Path $backup.FullName $record.name) -Algorithm SHA256).Hash -ne $record.originalHash) { throw 'Backup integrity check failed.' }
        }
        foreach ($record in $records) {
            $target=Join-Path $gameRoot $record.name
            if ($record.existed) { Copy-Item -LiteralPath (Join-Path $backup.FullName $record.name) -Destination $target }
            elseif ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $record.installedHash) { Remove-Item -LiteralPath $target }
        }
        Write-Host 'Restored the previous program. Your own game data and save were not modified.'
        exit 0
    }
    $manifest=Get-Content -LiteralPath (Join-Path $taskRoot 'runtime-manifest.json') -Raw | ConvertFrom-Json
    foreach ($file in $manifest.files) {
        if ($file.name -notin @('PowerMoonRecomp.exe','libc++.dll')) { throw 'Unrecognized package entry.' }
        $source=Join-Path (Join-Path $taskRoot 'payload') $file.name
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) { throw 'Package integrity check failed. Download the release again.' }
    }
    $current=(Get-FileHash -LiteralPath $ExecutablePath -Algorithm SHA256).Hash
    if ($current -eq $manifest.files[0].sha256) {
        foreach ($file in $manifest.files) {
            $target=Join-Path $gameRoot $file.name
            if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.sha256) { throw 'The installed update is incomplete. Run RESTORE.cmd, then INSTALL.cmd again.' }
        }
        $local | ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding UTF8
        Write-Host 'This controller update is already installed. Use PLAY.cmd.'
        exit 0
    }
    if ($current -notin $manifest.supportedOriginalHashes) { throw 'Unsupported program version. This update supports the original Windows Power Moon Recomp 0.1.6 runtime only.' }
    foreach ($required in @('SDL3.dll','avcodec-63.dll','avformat-63.dll','avutil-61.dll','swscale-10.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $gameRoot $required))) { throw "Incomplete upstream installation: missing $required" }
    }
    $backup=Join-Path (Join-Path $gameRoot 'controller-backups') ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -Path $backup -ItemType Directory -Force | Out-Null
    $records=@()
    foreach ($file in $manifest.files) {
        $target=Join-Path $gameRoot $file.name;$existed=Test-Path -LiteralPath $target;$originalHash=''
        if ($existed) { $originalHash=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash;Copy-Item -LiteralPath $target -Destination (Join-Path $backup $file.name) }
        $records+=@{name=$file.name;existed=$existed;originalHash=$originalHash;installedHash=$file.sha256}
    }
    $records | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'backup.json') -Encoding UTF8
    try {
        foreach ($file in $manifest.files) { Copy-Item -LiteralPath (Join-Path (Join-Path $taskRoot 'payload') $file.name) -Destination (Join-Path $gameRoot $file.name) }
    } catch {
        foreach ($record in $records) {
            if ($record.existed) { Copy-Item -LiteralPath (Join-Path $backup $record.name) -Destination (Join-Path $gameRoot $record.name) }
            elseif (Test-Path -LiteralPath (Join-Path $gameRoot $record.name)) { Remove-Item -LiteralPath (Join-Path $gameRoot $record.name) }
        }
        throw
    }
    $local | ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding UTF8
    Write-Host 'Installed. Use PLAY.cmd to start. RESTORE.cmd returns to the backed-up program.'
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
