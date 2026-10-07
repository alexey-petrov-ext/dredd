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
  Makes an official LLVM Windows release use this machine's DIA SDK.

.DESCRIPTION
  The CMake package in clang+llvm-*-x86_64-pc-windows-msvc refers to
  diaguids.lib by the absolute path on the machine that built the release,
  e.g. 'C:/Program Files/Microsoft Visual Studio/2022/Enterprise/DIA SDK/...'.
  Linking fails wherever Visual Studio is installed elsewhere (another
  edition, Build Tools, VS 2026). This script replaces that path with the
  diaguids.lib of the current Visual Studio. It is idempotent.
#>
param(
  [Parameter(Mandatory = $true)][string]$LlvmDir,
  # Defaults to the DIA SDK of the newest Visual Studio installation.
  [string]$DiaGuidsLib = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VsDevEnv.ps1')

if (-not $DiaGuidsLib) { $DiaGuidsLib = Get-DiaSdkLib }
$replacement = $DiaGuidsLib -replace '\\', '/'

$exports = Get-ChildItem -Path (Join-Path $LlvmDir 'lib\cmake') -Recurse -Filter '*.cmake' |
  Where-Object { Select-String -Path $_.FullName -Pattern 'diaguids\.lib' -Quiet }
foreach ($file in $exports) {
  $text = [System.IO.File]::ReadAllText($file.FullName)
  # A path inside a quoted, semicolon-separated list that ends in diaguids.lib.
  $fixed = [regex]::Replace($text, '[^";]*[\\/]diaguids\.lib', { param($m) $replacement })
  if ($fixed -ne $text) {
    [System.IO.File]::WriteAllText($file.FullName, $fixed)
    Write-Host "DIA SDK path set to '$replacement' in $($file.FullName)"
  }
}
if (-not $exports) {
  Write-Host 'No reference to diaguids.lib found; nothing to do.'
}
