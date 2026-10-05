param(
    [string]$SdkRoot = '',
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
$srRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $srRoot
try {
    if ($SdkRoot) {
        $srSdk = (Resolve-Path -LiteralPath $SdkRoot).Path
        $srPrefixes = @($srSdk, (Join-Path $srSdk 'obs-deps-2026-07-15-x64'), (Join-Path $srSdk 'obs-deps-qt6-2026-07-15-x64')) -join ';'
        cmake -S . -B build_sdk -G 'Visual Studio 17 2022' -A x64 -DSR_USE_EXISTING_SDK=ON "-DCMAKE_PREFIX_PATH=$srPrefixes" -DCMAKE_COMPILE_WARNING_AS_ERROR=ON "-DSR_BUILD_TESTS=$($Tests.IsPresent.ToString().ToUpper())"
        if ($LASTEXITCODE) { throw 'CMake configure failed' }
        cmake --build build_sdk --config RelWithDebInfo --parallel 4
    } else {
        cmake --preset windows-ci-x64 "-DSR_BUILD_TESTS=$($Tests.IsPresent.ToString().ToUpper())"
        if ($LASTEXITCODE) { throw 'CMake configure failed' }
        cmake --build --preset windows-ci-x64 --parallel 4
    }
    if ($LASTEXITCODE) { throw 'Build failed' }
} finally { Pop-Location }
