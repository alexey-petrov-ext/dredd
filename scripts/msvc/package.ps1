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
  Packages a Windows build of Dredd as a portable, versioned zip.

.DESCRIPTION
  Layout of dredd-<version>-windows-x64.zip:

    dredd-<version>-windows-x64/
      bin/dredd.exe                  Clang/LLVM and protobuf linked statically
      lib/clang/<major>/include/     Clang's built-in headers; Dredd finds
                                     them relative to bin/dredd.exe
      scripts/query_mutant_info.py   needs only the Python standard library
      LICENSE                        Dredd (Apache-2.0)
      licenses/                      licenses of the code linked into dredd.exe
      BUILDINFO.json                 version, commit, LLVM and MSVC versions

  Also writes <zip>.sha256 next to the zip. Users do not need LLVM, CMake or
  Visual Studio to run Dredd from this package; they only need a compiler
  environment for the code that they mutate.
#>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$LlvmDir,
  [string]$BuildDir = 'build-msvc',
  [string]$OutDir = 'dist'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repoRoot $BuildDir }
if (-not [System.IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repoRoot $OutDir }
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z.+_-]*$') {
  throw "Version '$Version' must consist of letters, digits and . + _ -"
}

$dredd = Join-Path $BuildDir 'src\dredd\dredd.exe'
if (-not (Test-Path $dredd)) { throw "$dredd not found; build Dredd first." }

# Clang's resource directory is lib/clang/<major> from LLVM 16 onwards.
$resourceDirs = @(Get-ChildItem -Directory (Join-Path $LlvmDir 'lib\clang'))
if ($resourceDirs.Count -ne 1) {
  throw "Expected exactly one directory under $LlvmDir\lib\clang, found $($resourceDirs.Count)."
}
$clangMajor = $resourceDirs[0].Name

$name = "dredd-$Version-windows-x64"
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin'), (Join-Path $stage 'scripts'),
  (Join-Path $stage 'licenses'), (Join-Path $stage "lib\clang\$clangMajor") | Out-Null

Copy-Item $dredd (Join-Path $stage 'bin')
Copy-Item -Recurse (Join-Path $resourceDirs[0].FullName 'include') (Join-Path $stage "lib\clang\$clangMajor")
Copy-Item (Join-Path $repoRoot 'scripts\query_mutant_info.py') (Join-Path $stage 'scripts')
Copy-Item (Join-Path $repoRoot 'LICENSE') $stage
Copy-Item (Join-Path $PSScriptRoot 'licenses\LLVM-LICENSE.TXT') (Join-Path $stage 'licenses')
Copy-Item (Join-Path $repoRoot 'third_party\protobuf\protobuf\LICENSE') (Join-Path $stage 'licenses\protobuf-LICENSE')
Copy-Item (Join-Path $PSScriptRoot 'licenses\README.txt') (Join-Path $stage 'licenses')

$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
$llvmVersion = (Select-String -Path (Join-Path $LlvmDir 'lib\cmake\llvm\LLVMConfig.cmake') `
    -Pattern 'set\(LLVM_PACKAGE_VERSION ([^)]+)\)').Matches[0].Groups[1].Value
$buildInfo = [ordered]@{
  version      = $Version
  commit       = $commit
  llvm         = $llvmVersion
  msvcToolset  = $env:VCToolsVersion
  builtAt      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}
$buildInfo | ConvertTo-Json | Set-Content -Encoding ASCII (Join-Path $stage 'BUILDINFO.json')

# Smoke test: the packaged executable starts and finds its built-in headers.
$probeDir = Join-Path $OutDir 'probe'
if (Test-Path $probeDir) { Remove-Item -Recurse -Force $probeDir }
New-Item -ItemType Directory -Path $probeDir | Out-Null
$probe = Join-Path $probeDir 'probe.cc'
Set-Content -Encoding ASCII $probe "#include <stddef.h>`nint f(int x) { return x + (int)sizeof(size_t); }"
# Dredd reports progress on stderr; with 'Stop', Windows PowerShell 5.1 would
# turn the first stderr line into a terminating error.
$ErrorActionPreference = 'Continue'
& (Join-Path $stage 'bin\dredd.exe') $probe -- 2>&1 | ForEach-Object { Write-Host $_ }
$probeExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($probeExitCode -ne 0) { throw 'The packaged dredd.exe failed the smoke test.' }
if (-not (Select-String -Path $probe -Pattern '__dredd_replace' -Quiet)) {
  throw 'The packaged dredd.exe did not mutate the smoke test file.'
}
Remove-Item -Recurse -Force $probeDir

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# bsdtar writes zip entries with forward slashes, unlike Compress-Archive in
# Windows PowerShell 5.1.
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $OutDir $name
if ($LASTEXITCODE -ne 0) { throw "Creating $zip failed." }

$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
Set-Content -Encoding ASCII "$zip.sha256" "$hash  $name.zip"
Write-Host "Package: $zip"
Write-Host "SHA-256: $hash"
if ($env:GITHUB_OUTPUT) {
  Add-Content -Encoding ASCII $env:GITHUB_OUTPUT "zip=$zip"
  Add-Content -Encoding ASCII $env:GITHUB_OUTPUT "sha256=$hash"
  Add-Content -Encoding ASCII $env:GITHUB_OUTPUT "dir=$stage"
}
