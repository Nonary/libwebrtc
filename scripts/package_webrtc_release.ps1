param(
  [Parameter(Mandatory = $true)]
  [string]$ReleaseVersion,
  [string]$RootDir = "",
  [string]$OutDir = "",
  [string]$ArtifactDir = "",
  [string]$ReleaseRepository = "",
  [string]$RunnerOs = "Windows",
  [string]$Toolchain = "ucrt-x86_64",
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [string]$WebrtcRepoUrl = "https://github.com/webrtc-sdk/webrtc.git",
  [string]$WebrtcRef = "m125_release"
)

$ErrorActionPreference = "Stop"

function Write-Step {
  param([string]$Message)
  Write-Host "[webrtc-package] $Message"
}

function Add-GitHubOutput {
  param(
    [string]$Name,
    [string]$Value
  )

  Write-Host "$Name=$Value"
  if ($env:GITHUB_OUTPUT) {
    "$Name=$Value" >> $env:GITHUB_OUTPUT
  }
}

function Require-Path {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "Required WebRTC artifact path was not found: $Path"
  }
}

$scriptRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $RootDir) {
  $RootDir = $scriptRoot
}
$RootDir = (Resolve-Path $RootDir).Path

if (-not $OutDir) {
  $OutDir = if ($env:WEBRTC_ROOT) { $env:WEBRTC_ROOT } else { Join-Path $RootDir "build\libwebrtc" }
}
$OutDir = (Resolve-Path $OutDir).Path

if (-not $ArtifactDir) {
  $ArtifactDir = Join-Path $RootDir ".tmp\webrtc-release"
}
if (-not $ReleaseRepository) {
  $ReleaseRepository = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { "Nonary/libwebrtc" }
}

$semverPattern = '^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$'
if ($ReleaseVersion -notmatch $semverPattern) {
  throw "ReleaseVersion must be a v-prefixed semantic version tag (for example v1.0.0): $ReleaseVersion"
}

Require-Path (Join-Path $OutDir "include\libwebrtc.h")
Require-Path (Join-Path $OutDir "lib\libwebrtc.dll")
Require-Path (Join-Path $OutDir "lib\libwebrtc.dll.a")

$keys = @{}
$outerGitHubOutput = $env:GITHUB_OUTPUT
$keyOutputPath = Join-Path ([System.IO.Path]::GetTempPath()) "webrtc-keys-$([System.Guid]::NewGuid().ToString('N')).txt"
try {
  $env:GITHUB_OUTPUT = $keyOutputPath
  & (Join-Path $PSScriptRoot "get_webrtc_cache_keys.ps1") `
    -RootDir $RootDir `
    -RunnerOs $RunnerOs `
    -Toolchain $Toolchain `
    -Configuration $Configuration `
    -WebrtcRepoUrl $WebrtcRepoUrl `
    -WebrtcRef $WebrtcRef
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to compute WebRTC release identity."
  }

  foreach ($line in @(Get-Content -LiteralPath $keyOutputPath)) {
    if ($line -match '^([A-Za-z0-9._-]+)=(.*)$') {
      $keys[$matches[1]] = $matches[2]
    }
  }
} finally {
  $env:GITHUB_OUTPUT = $outerGitHubOutput
  if (Test-Path -LiteralPath $keyOutputPath) {
    Remove-Item -LiteralPath $keyOutputPath -Force -ErrorAction SilentlyContinue
  }
}

foreach ($requiredKey in @("output-key", "webrtc-source-revision", "libwebrtc-wrapper-commit", "libwebrtc-output-input-hash", "binary-recipe-hash")) {
  if (-not $keys.ContainsKey($requiredKey) -or [string]::IsNullOrWhiteSpace($keys[$requiredKey])) {
    throw "WebRTC key generator did not emit '$requiredKey'."
  }
}

# Release identity follows the libvirtualdisplay convention: a semver tag on
# this repository with a versioned asset name. The content-hash provenance is
# preserved inside the manifest for verification.
$releaseTag = $ReleaseVersion
$packageVersion = $ReleaseVersion.TrimStart('v')
$assetName = "libwebrtc-$packageVersion-windows-x64.zip"

$ArtifactDir = [System.IO.Path]::GetFullPath($ArtifactDir)
if (Test-Path -LiteralPath $ArtifactDir) {
  Remove-Item -LiteralPath $ArtifactDir -Recurse -Force
}
New-Item -ItemType Directory -Path $ArtifactDir | Out-Null

$stageDir = Join-Path $ArtifactDir "stage"
New-Item -ItemType Directory -Path $stageDir | Out-Null
Copy-Item -LiteralPath (Join-Path $OutDir "include") -Destination (Join-Path $stageDir "include") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $OutDir "lib") -Destination (Join-Path $stageDir "lib") -Recurse -Force

$embeddedManifest = [ordered]@{
  schema = 1
  repository = $ReleaseRepository
  tag = $releaseTag
  version = $packageVersion
  asset = $assetName
  platform = "windows-x64"
  configuration = $Configuration
  runner_os = $RunnerOs
  toolchain = $Toolchain
  webrtc_repo_url = $WebrtcRepoUrl
  webrtc_ref = $WebrtcRef
  webrtc_revision = $keys["webrtc-source-revision"]
  libwebrtc_wrapper_commit = $keys["libwebrtc-wrapper-commit"]
  libwebrtc_output_input_hash = $keys["libwebrtc-output-input-hash"]
  binary_recipe_hash = $keys["binary-recipe-hash"]
  output_key = $keys["output-key"]
}
$embeddedManifestPath = Join-Path $stageDir "webrtc-artifact.json"
$embeddedManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $embeddedManifestPath -Encoding UTF8

$assetPath = Join-Path $ArtifactDir $assetName
Write-Step "Creating $assetPath"
Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $assetPath -Force
$assetSha256 = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()

$pinnedManifest = [ordered]@{}
foreach ($key in $embeddedManifest.Keys) {
  $pinnedManifest[$key] = $embeddedManifest[$key]
}
$pinnedManifest["sha256"] = $assetSha256
$pinnedManifest["created_utc"] = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

$manifestPath = Join-Path $ArtifactDir "windows-x64.json"
$pinnedManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

$notesPath = Join-Path $ArtifactDir "release-notes.md"
@"
Prebuilt Windows x64 libwebrtc artifacts ($ReleaseVersion).

- WebRTC ref: ``$WebrtcRef``
- WebRTC revision: ``$($keys["webrtc-source-revision"])``
- libwebrtc wrapper commit: ``$($keys["libwebrtc-wrapper-commit"])``
- output key: ``$($keys["output-key"])``
- asset SHA256: ``$assetSha256``

Consumers pin this release through a committed ``windows-x64.json`` manifest
(also attached to this release) and must verify the SHA256 before extracting
the archive.
"@ | Set-Content -LiteralPath $notesPath -Encoding UTF8

Add-GitHubOutput -Name "asset-name" -Value $assetName
Add-GitHubOutput -Name "asset-path" -Value $assetPath
Add-GitHubOutput -Name "manifest-path" -Value $manifestPath
Add-GitHubOutput -Name "notes-path" -Value $notesPath
Add-GitHubOutput -Name "release-tag" -Value $releaseTag
Add-GitHubOutput -Name "sha256" -Value $assetSha256
Add-GitHubOutput -Name "output-key" -Value $keys["output-key"]
