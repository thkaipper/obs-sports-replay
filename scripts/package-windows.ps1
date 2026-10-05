param(
    [string]$BuildDirectory = 'build_x64',
    [string]$InnoCompiler = ''
)
$ErrorActionPreference = 'Stop'
$srRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$srSpec = Get-Content -LiteralPath (Join-Path $srRoot 'buildspec.json') -Raw | ConvertFrom-Json
$srStage = Join-Path $srRoot 'release/staging'
New-Item -ItemType Directory -Path $srStage -Force | Out-Null
cmake --install (Join-Path $srRoot $BuildDirectory) --config RelWithDebInfo --prefix $srStage
if ($LASTEXITCODE) { throw 'Package staging failed' }
Copy-Item -LiteralPath (Join-Path $srRoot 'LICENSE') -Destination (Join-Path $srStage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $srRoot 'README.md') -Destination $srStage
Copy-Item -LiteralPath (Join-Path $srRoot 'docs') -Destination $srStage -Recurse -Force
$srZip = Join-Path $srRoot "release/sports-replay-$($srSpec.version)-windows-x64.zip"
Compress-Archive -Path (Join-Path $srStage '*') -DestinationPath $srZip -Force
if ($InnoCompiler) {
    & $InnoCompiler "/DMyVersion=$($srSpec.version)" "/DPackageDir=$srStage" "/O$(Join-Path $srRoot 'release')" (Join-Path $srRoot 'installer/sports-replay.iss')
    if ($LASTEXITCODE) { throw 'Installer compilation failed' }
}
Get-Item -LiteralPath $srZip
