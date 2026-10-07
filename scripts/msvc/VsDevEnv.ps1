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

# Helpers for the MSVC build scripts. Dot-source this file:
#
#   . scripts/msvc/VsDevEnv.ps1
#   Enter-VsDevEnv -VcVarsVer 14.44
#
# Works with Windows PowerShell 5.1 and PowerShell 7.

Set-StrictMode -Version 3.0

function Get-VsInstallPath {
  # Returns the newest Visual Studio installation with the x64 C++ tools.
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'; is Visual Studio installed?"
  }
  $path = Invoke-Native $vswhere @('-latest', '-products', '*', '-prerelease',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath')
  if (-not $path) {
    throw 'No Visual Studio installation with the x64 C++ tools was found.'
  }
  return ($path | Select-Object -First 1)
}

function Enter-VsDevEnv {
  # Imports the environment of 'vcvarsall.bat x64' into this PowerShell
  # session. Does nothing if cl.exe is already on PATH and no toolset version
  # is requested. -VcVarsVer pins the MSVC toolset, e.g. 14.44.
  param([string]$VcVarsVer = '')

  if (-not $VcVarsVer -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    return
  }
  $vcvarsall = Join-Path (Get-VsInstallPath) 'VC\Auxiliary\Build\vcvarsall.bat'
  $arguments = 'x64'
  if ($VcVarsVer) { $arguments += " -vcvars_ver=$VcVarsVer" }
  $output = Invoke-Native 'cmd.exe' @('/s', '/c', "`"$vcvarsall`" $arguments >nul && set") -NoThrow
  if ($LASTEXITCODE -ne 0) {
    throw "vcvarsall.bat $arguments failed (exit code $LASTEXITCODE)."
  }
  foreach ($line in $output) {
    $index = $line.IndexOf('=')
    if ($index -gt 0) {
      Set-Item -Path ("env:" + $line.Substring(0, $index)) -Value $line.Substring($index + 1)
    }
  }
  if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe is not on PATH after running vcvarsall.bat.'
  }
  Write-Host "MSVC environment: VCToolsVersion=$env:VCToolsVersion"
}

function Get-DiaSdkLib {
  # Returns the path of diaguids.lib for x64 in the current Visual Studio.
  $roots = @()
  if ($env:VSINSTALLDIR) { $roots += $env:VSINSTALLDIR }
  $roots += Get-VsInstallPath
  foreach ($root in $roots) {
    $lib = Join-Path $root 'DIA SDK\lib\amd64\diaguids.lib'
    if (Test-Path $lib) { return (Resolve-Path $lib).Path }
  }
  throw 'diaguids.lib was not found under the Visual Studio installation.'
}

function Invoke-Native {
  # Runs a native command and throws if it fails, unless -NoThrow is given;
  # the exit code is then left in $LASTEXITCODE. Use it for native commands
  # in scripts that run with 'Stop': when the caller redirects the output,
  # e.g. to a log file, Windows PowerShell 5.1 turns the first stderr line of
  # a native command into a terminating error.
  param([Parameter(Mandatory = $true)][string]$Command,
        [string[]]$Arguments = @(),
        [switch]$NoThrow)
  # With 'Continue', a missing command would only print an error, and the
  # check below would read the exit code of an earlier command.
  if (-not (Get-Command $Command -CommandType Application -ErrorAction SilentlyContinue)) {
    throw "'$Command' was not found."
  }
  # Local to this function; the caller keeps its own preference.
  $ErrorActionPreference = 'Continue'
  Write-Host "> $Command $($Arguments -join ' ')"
  & $Command @Arguments
  if (-not $NoThrow -and $LASTEXITCODE -ne 0) {
    throw "'$Command' failed with exit code $LASTEXITCODE."
  }
}
