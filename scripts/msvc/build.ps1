# Copyright 2026 The Dredd Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

<#
.SYNOPSIS
  Builds Dredd with MSVC against an official LLVM release for Windows.

.DESCRIPTION
  1. Enters the Visual Studio x64 developer environment (optionally with a
     pinned MSVC toolset).
  2. Downloads and unpacks clang+llvm-<version>-x86_64-pc-windows-msvc.tar.xz
     from the LLVM GitHub release, unless it is already unpacked.
  3. Points the LLVM CMake package at this machine's DIA SDK: the archive
     refers to diaguids.lib by the absolute path of the machine it was built
     on.
  4. Configures and builds Dredd with Ninja, using the static MSVC runtime
     (/MT) that the LLVM libraries are built with.
  5. Runs the unit tests.

  The same script is used in CI and on developer machines.

.EXAMPLE
  scripts/msvc/build.ps1 -LlvmVersion 22.1.8 -VcVarsVer 14.44
#>
param(
  [string]$LlvmVersion = '22.1.8',
  # Where the LLVM release is (or will be) unpacked.
  [string]$LlvmDir = '',
  # Optional directory in which the downloaded archive is kept, e.g. a CI
  # cache directory.
  [string]$ArchiveCache = '',
  [string]$BuildDir = 'build-msvc',
  [ValidateSet('Release', 'RelWithDebInfo')]
  [string]$Config = 'Release',
  # MSVC toolset to use, e.g. 14.44; empty means the newest installed.
  [string]$VcVarsVer = '',
  [switch]$WarningsAsErrors,
  [switch]$SkipUnitTests
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is slow otherwise.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'VsDevEnv.ps1')

if (-not $LlvmDir) {
  $LlvmDir = Join-Path $repoRoot "third_party\clang+llvm-$LlvmVersion-msvc"
}
$LlvmDir = [System.IO.Path]::GetFullPath($LlvmDir)
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $repoRoot $BuildDir
}

Enter-VsDevEnv -VcVarsVer $VcVarsVer

# --- 2. LLVM release -------------------------------------------------------
$llvmConfig = Join-Path $LlvmDir 'lib\cmake\llvm\LLVMConfig.cmake'
if (-not (Test-Path $llvmConfig)) {
  $name = "clang+llvm-$LlvmVersion-x86_64-pc-windows-msvc"
  $url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/$name.tar.xz"
  if (-not $ArchiveCache) { $ArchiveCache = Join-Path $env:TEMP 'dredd-llvm-archives' }
  New-Item -ItemType Directory -Force -Path $ArchiveCache | Out-Null
  $archive = Join-Path $ArchiveCache "$name.tar.xz"
  if (-not (Test-Path $archive)) {
    Write-Host "Downloading $url"
    $partial = "$archive.partial"
    Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing
    Move-Item -Force $partial $archive
  }
  $staging = "$LlvmDir.staging"
  if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
  New-Item -ItemType Directory -Force -Path $staging | Out-Null
  Write-Host "Unpacking $archive"
  # The tar.exe that ships with Windows (bsdtar) reads .tar.xz directly.
  Invoke-Native "$env:SystemRoot\System32\tar.exe" @('-xf', $archive, '-C', $staging) -NoThrow
  if ($LASTEXITCODE -ne 0) {
    # Fall back to 7-Zip if this tar.exe was built without xz support.
    $sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if (-not $sevenZip) { throw "Could not unpack $archive (tar.exe failed, 7z.exe not found)." }
    Remove-Item -Recurse -Force $staging
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    Invoke-Native $sevenZip.Source @('x', '-y', "-o$staging", $archive)
    $tarFile = Join-Path $staging "$name.tar"
    Invoke-Native "$env:SystemRoot\System32\tar.exe" @('-xf', $tarFile, '-C', $staging)
    Remove-Item -Force $tarFile
  }
  if (Test-Path $LlvmDir) { Remove-Item -Recurse -Force $LlvmDir }
  Move-Item (Join-Path $staging $name) $LlvmDir
  Remove-Item -Recurse -Force $staging
}
Write-Host "LLVM: $LlvmDir"

# --- 3. DIA SDK path ---------------------------------------------------------
& (Join-Path $PSScriptRoot 'Repair-LlvmDiaSdkPath.ps1') -LlvmDir $LlvmDir

# --- 4. Configure and build ------------------------------------------------
$werror = if ($WarningsAsErrors) { 'ON' } else { 'OFF' }
$cmakeArgs = @(
  '-S', $repoRoot,
  '-B', $BuildDir,
  '-G', 'Ninja',
  "-DCMAKE_BUILD_TYPE=$Config",
  '-DCMAKE_C_COMPILER=cl.exe',
  '-DCMAKE_CXX_COMPILER=cl.exe',
  "-DDREDD_CLANG_LLVM_DIR=$($LlvmDir -replace '\\', '/')",
  # The official LLVM libraries use the static runtime (/MT). Dredd's
  # cmake_minimum_required predates policy CMP0091, so enable it explicitly.
  '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
  '-Dprotobuf_MSVC_STATIC_RUNTIME=ON',
  "-DDREDD_WARNINGS_AS_ERRORS=$werror"
)
Invoke-Native 'cmake' $cmakeArgs
Invoke-Native 'cmake' @('--build', $BuildDir, '--config', $Config)

# --- 5. Unit tests -----------------------------------------------------------
if (-not $SkipUnitTests) {
  Invoke-Native (Join-Path $BuildDir 'src\libdreddtest\libdreddtest.exe')
}
Write-Host "Built $(Join-Path $BuildDir 'src\dredd\dredd.exe')"
