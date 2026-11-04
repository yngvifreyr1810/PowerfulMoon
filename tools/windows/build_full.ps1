param(
    [Parameter(Mandatory=$true)][string]$RuntimeRoot,
    [Parameter(Mandatory=$true)][string]$BridgeZip,
    [Parameter(Mandatory=$true)][string]$Libcxx,
    [Parameter(Mandatory=$true)][string]$DependencySources,
    [string]$OutputDirectory = ''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root 'build/dist' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$release=Get-Content -LiteralPath (Join-Path $root 'release.json') -Raw | ConvertFrom-Json
$version=$release.version
python (Join-Path $PSScriptRoot 'version_info.py')
if ($LASTEXITCODE -ne 0) { throw 'Invalid release metadata' }
python -m PyInstaller --noconfirm --clean --onefile --windowed --name PowerfulMoon-Update --icon (Join-Path $root 'docs/icon/icon.ico') --distpath (Join-Path $root 'build') --workpath (Join-Path $root 'build/updater-work') --specpath (Join-Path $root 'build') --add-data ((Join-Path $root 'docs/icon/icon.ico')+';.') (Join-Path $PSScriptRoot 'updater_app.py')
if ($LASTEXITCODE -ne 0) { throw 'Updater EXE build failed' }
$zip=Join-Path $OutputDirectory ('PowerfulMoon-'+$version+'-windows-x64.zip')
python (Join-Path $PSScriptRoot 'package_full.py') --runtime-root $RuntimeRoot --bridge-zip $BridgeZip --libcxx $Libcxx --dependency-sources $DependencySources --output $zip
if ($LASTEXITCODE -ne 0) { throw 'Full package build failed' }
$taskBuild=Join-Path $env:TEMP 'PowerfulMoon-Setup-build'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
Copy-Item -LiteralPath $zip -Destination (Join-Path $taskBuild 'PowerfulMoon-windows-x64.zip') -Force
python -m PyInstaller --noconfirm --clean --onefile --windowed --name ('PowerfulMoon-Setup-'+$version) --icon (Join-Path $root 'docs/icon/icon.ico') --distpath $OutputDirectory --workpath (Join-Path $taskBuild 'build') --specpath $taskBuild --add-data ((Join-Path $taskBuild 'PowerfulMoon-windows-x64.zip')+';.') --add-data ((Join-Path $root 'docs/icon/icon.ico')+';.') (Join-Path $PSScriptRoot 'setup_app.py')
if ($LASTEXITCODE -ne 0) { throw 'Setup EXE build failed' }
$exe=Join-Path $OutputDirectory ('PowerfulMoon-Setup-'+$version+'.exe')
$hash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($exe+'.sha256'),($hash+'  '+[IO.Path]::GetFileName($exe)+[Environment]::NewLine),[Text.Encoding]::ASCII)
