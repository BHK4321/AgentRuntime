# Build every C++ test binary in AgentOS/build-grpc and run them with one ctest.
# From the workspace root (E:\OS):
#   powershell -File AgentOS\scripts\run-all-tests.ps1

$ErrorActionPreference = "Stop"

$WorkspaceRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $WorkspaceRoot "AgentOS\CMakeLists.txt"))) {
    $WorkspaceRoot = (Get-Location).Path
}

Set-Location $WorkspaceRoot

$BuildDir = Join-Path $WorkspaceRoot "AgentOS\build-grpc"
$VcpkgRoot = "E:\vcpkg"
$PgBin = "C:\Program Files\PostgreSQL\17\bin"

$env:AGENTOS_DATABASE_URL = "postgresql://agentos:agentos@localhost:5432/agentos"
$env:PATH = "$(Join-Path $VcpkgRoot 'installed\x64-windows\bin');$PgBin;$env:PATH"

if (-not (Test-Path $BuildDir)) {
    cmake -S (Join-Path $WorkspaceRoot "AgentOS") -B $BuildDir `
        -DAGENTOS_ENABLE_POSTGRES=ON `
        -DAGENTOS_ENABLE_GRPC=ON `
        -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
        -DCMAKE_PREFIX_PATH="$VcpkgRoot\installed\x64-windows"
}

cmake --build $BuildDir --config Release
ctest --test-dir $BuildDir -C Release --output-on-failure
exit $LASTEXITCODE
