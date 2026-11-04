# MIT. Uses tiny synthetic files only: no game, actual driver or game installation.
$ErrorActionPreference='Stop'
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+[IO.Path]::PathSeparator+$env:PSModulePath
$scratch=Join-Path $env:TEMP ('PowerMoon-installer-test-'+[guid]::NewGuid().ToString('N'))
$package=Join-Path $scratch 'package';$game=Join-Path $scratch 'game'
New-Item -ItemType Directory -Path $package,$game,(Join-Path $package 'payload') -Force | Out-Null
$installer=Join-Path $package 'Controller-Setup.ps1'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../tools/windows/Controller-Setup.ps1') -Destination $installer
$exe=Join-Path $game 'PowerMoonRecomp.exe'
Set-Content -LiteralPath $exe -Value 'synthetic original host'
$original=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$files=@()
foreach ($name in @('PowerMoonRecomp.exe','libc++.dll')) {
    $path=Join-Path (Join-Path $package 'payload') $name
    Set-Content -LiteralPath $path -Value ('synthetic update '+$name)
    $files+=@{name=$name;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}
@{files=$files;supportedOriginalHashes=@($original)} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'runtime-manifest.json')
foreach ($name in @('SDL3.dll','avcodec-63.dll','avformat-63.dll','avutil-61.dll','swscale-10.dll','save-sentinel.txt')) { Set-Content -LiteralPath (Join-Path $game $name) -Value 'preserve me' }
function Run-Setup([string]$mode,[int]$expected=0) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer -Mode $mode -ExecutablePath $exe
    if ($LASTEXITCODE -ne $expected) { throw "Unexpected $mode result: $LASTEXITCODE" }
}
Run-Setup Install
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $files[0].sha256) { throw 'Installed host mismatch' }
$count=(Get-ChildItem -LiteralPath (Join-Path $game 'controller-backups') -Directory).Count
Run-Setup Install
if ((Get-ChildItem -LiteralPath (Join-Path $game 'controller-backups') -Directory).Count -ne $count) { throw 'Repeated install made a new backup' }
Run-Setup Restore
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $original) { throw 'Original host not restored' }
if (Test-Path -LiteralPath (Join-Path $game 'libc++.dll')) { throw 'New DLL was not removed on restore' }
if ((Get-Content -LiteralPath (Join-Path $game 'save-sentinel.txt') -Raw).Trim() -ne 'preserve me') { throw 'Save sentinel changed' }
Set-Content -LiteralPath $exe -Value 'unsupported host'
$unsupported=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
Run-Setup Install 1
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $unsupported) { throw 'Unsupported host was modified' }
Write-Output 'Installer, idempotence, restoration, dependency cleanup, save preservation, version refusal: PASS'
