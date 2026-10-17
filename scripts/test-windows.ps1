param(
    [string]$BuildDirectory = 'build_x64',
    [string]$ObsRoot = 'C:/Program Files/obs-studio',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$srRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$srBuild = Join-Path $srRoot $BuildDirectory
$srRuntime = (Resolve-Path -LiteralPath $ObsRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $srRuntime 'bin/64bit/obs.dll'))) {
    throw 'ObsRoot must point to the root of an extracted or installed OBS runtime'
}
cmake -S $srRoot -B $srBuild -DSR_BUILD_TESTS=ON "-DSR_OBS_RUNTIME_DIR=$srRuntime"
if ($LASTEXITCODE) { throw 'Test configuration failed' }
cmake --build $srBuild --config $Configuration --parallel 4
if ($LASTEXITCODE) { throw 'Test build failed' }
ctest --test-dir $srBuild -C $Configuration --output-on-failure --no-tests=error --parallel 2
if ($LASTEXITCODE) { throw 'Regression tests failed' }
