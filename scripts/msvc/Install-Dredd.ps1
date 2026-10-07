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
  Installs a packaged Windows build of Dredd (see scripts/msvc/package.ps1).

.DESCRIPTION
  Copy this script into the repository that uses Dredd, next to a pin file:

    { "version": "0.1.0", "sha256": "<sha-256 of the zip>",
      "url": "https://.../dredd-0.1.0-windows-x64.zip" }

  The script is idempotent and needs no administrator rights:
    - if <InstallRoot>\<version> already holds that package, it does nothing;
    - otherwise it downloads (or copies) the zip, checks its SHA-256, unpacks
      it next to the final folder and renames it into place.
  It prints the path of dredd.exe, adds bin\ to PATH for the current process,
  and, on Azure Pipelines or GitHub Actions, for the following steps too.

.EXAMPLE
  $dredd = scripts\Install-Dredd.ps1 -PinFile tools\dredd.pin.json

.EXAMPLE
  # Azure Artifacts or another source that needs a token.
  scripts\Install-Dredd.ps1 -PinFile tools\dredd.pin.json `
    -Headers @{ Authorization = "Bearer $env:SYSTEM_ACCESSTOKEN" }
#>
[CmdletBinding(DefaultParameterSetName = 'Pin')]
param(
  [Parameter(ParameterSetName = 'Pin', Mandatory = $true)][string]$PinFile,
  [Parameter(ParameterSetName = 'Explicit', Mandatory = $true)][string]$Version,
  [Parameter(ParameterSetName = 'Explicit', Mandatory = $true)][string]$Sha256,
  # A URL, or the path of a local zip file.
  [Parameter(ParameterSetName = 'Explicit', Mandatory = $true)][string]$Source,
  [string]$InstallRoot = '',
  [hashtable]$Headers = @{}
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($PSCmdlet.ParameterSetName -eq 'Pin') {
  $pin = Get-Content -Raw $PinFile | ConvertFrom-Json
  $Version = $pin.version
  $Sha256 = $pin.sha256
  $Source = $pin.url
}
$Sha256 = $Sha256.ToLowerInvariant()
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z.+_-]*$') { throw "Invalid version '$Version'." }
if ($Sha256 -notmatch '^[0-9a-f]{64}$') { throw "Invalid SHA-256 '$Sha256'." }

if (-not $InstallRoot) {
  # Agents keep tools in their tool cache; developers in their profile.
  if ($env:AGENT_TOOLSDIRECTORY) { $InstallRoot = Join-Path $env:AGENT_TOOLSDIRECTORY 'dredd' }
  elseif ($env:RUNNER_TOOL_CACHE) { $InstallRoot = Join-Path $env:RUNNER_TOOL_CACHE 'dredd' }
  else { $InstallRoot = Join-Path $env:LOCALAPPDATA 'Programs\Dredd' }
}
$target = Join-Path $InstallRoot $Version
$marker = Join-Path $target '.package-sha256'
$exe = Join-Path $target 'bin\dredd.exe'

function Add-ToPath([string]$dir) {
  if (($env:PATH -split ';') -notcontains $dir) { $env:PATH = "$dir;$env:PATH" }
  if ($env:TF_BUILD) { Write-Host "##vso[task.prependpath]$dir" }
  if ($env:GITHUB_PATH) { Add-Content -Encoding ASCII $env:GITHUB_PATH $dir }
}

if ((Test-Path $exe) -and (Test-Path $marker) -and
    ((Get-Content -Raw $marker).Trim() -eq $Sha256)) {
  Write-Host "Dredd $Version is already installed in $target"
  Add-ToPath (Split-Path $exe)
  return $exe
}

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
$work = Join-Path $InstallRoot ".tmp-$Version-$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null
try {
  $zip = Join-Path $work 'dredd.zip'
  if ($Source -match '^https?://') {
    Write-Host "Downloading $Source"
    Invoke-WebRequest -Uri $Source -OutFile $zip -Headers $Headers -UseBasicParsing
  } else {
    Copy-Item $Source $zip
  }
  $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
  if ($actual -ne $Sha256) {
    throw "SHA-256 mismatch for $Source`n  expected $Sha256`n  actual   $actual"
  }
  $unpacked = Join-Path $work 'unpacked'
  Expand-Archive -Path $zip -DestinationPath $unpacked
  # The zip holds a single folder, dredd-<version>-windows-x64.
  $top = @(Get-ChildItem -Directory $unpacked)
  if ($top.Count -ne 1 -or -not (Test-Path (Join-Path $top[0].FullName 'bin\dredd.exe'))) {
    throw 'Unexpected package layout: expected one folder containing bin\dredd.exe.'
  }
  Set-Content -Encoding ASCII (Join-Path $top[0].FullName '.package-sha256') $Sha256
  if (Test-Path $target) { Remove-Item -Recurse -Force $target }
  Move-Item $top[0].FullName $target
} finally {
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }
}

Write-Host "Installed Dredd $Version in $target"
Add-ToPath (Split-Path $exe)
return $exe
