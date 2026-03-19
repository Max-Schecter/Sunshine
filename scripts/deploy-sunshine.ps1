param(
  [Parameter(Mandatory = $true)]
  [string]$Repo,

  [Parameter(Mandatory = $true)]
  [long]$RunId,

  [Parameter(Mandatory = $true)]
  [string]$ArtifactName,

  [Parameter(Mandatory = $true)]
  [string]$RollbackInstallerPath,

  [string]$Token = $env:GITHUB_TOKEN,
  [string]$ServiceName = "SunshineService",
  [int]$HealthPort = 47990,
  [int]$StartupTimeoutSeconds = 60,
  [string]$WorkRoot = "C:\ops\sunshine-deploy",
  [string]$ConfigPath = "C:\Program Files\Sunshine\config"
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
  $isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
  )
  if (-not $isAdmin) {
    throw "This script must run in an elevated PowerShell session."
  }
}

function New-WorkDir {
  param([string]$Root)
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $path = Join-Path $Root $stamp
  New-Item -ItemType Directory -Path $path -Force | Out-Null
  return $path
}

function Get-GitHubHeaders {
  param([string]$AuthToken)
  if ([string]::IsNullOrWhiteSpace($AuthToken)) {
    throw "GitHub token missing. Set -Token or GITHUB_TOKEN."
  }
  return @{
    Authorization          = "Bearer $AuthToken"
    "X-GitHub-Api-Version" = "2022-11-28"
    Accept                 = "application/vnd.github+json"
  }
}

function Get-ArtifactDownloadUrl {
  param(
    [string]$RepoName,
    [long]$Run,
    [string]$Name,
    [hashtable]$Headers
  )

  $url = "https://api.github.com/repos/$RepoName/actions/runs/$Run/artifacts?per_page=100"
  $resp = Invoke-RestMethod -Method GET -Uri $url -Headers $Headers
  if (-not $resp.artifacts) {
    throw "No artifacts found for run id $Run in $RepoName."
  }

  $artifact = $resp.artifacts |
    Where-Object { $_.name -eq $Name -and $_.expired -eq $false } |
    Select-Object -First 1

  if (-not $artifact) {
    throw "Artifact '$Name' not found (or expired) for run id $Run."
  }

  return $artifact.archive_download_url
}

function Assert-RunSucceeded {
  param(
    [string]$RepoName,
    [long]$Run,
    [hashtable]$Headers
  )

  $url = "https://api.github.com/repos/$RepoName/actions/runs/$Run"
  $run = Invoke-RestMethod -Method GET -Uri $url -Headers $Headers
  if ($run.status -ne "completed" -or $run.conclusion -ne "success") {
    throw "Run $Run is not successful (status=$($run.status), conclusion=$($run.conclusion))."
  }
}

function Download-ArtifactZip {
  param(
    [string]$DownloadUrl,
    [string]$OutZip,
    [hashtable]$Headers
  )
  Invoke-WebRequest -Method GET -Uri $DownloadUrl -Headers $Headers -OutFile $OutZip
}

function Find-Installer {
  param([string]$SearchDir)

  $preferredExe = Get-ChildItem -Path $SearchDir -Recurse -File -Filter "*installer.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($preferredExe) { return $preferredExe.FullName }

  $anyExe = Get-ChildItem -Path $SearchDir -Recurse -File -Filter "*.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($anyExe) { return $anyExe.FullName }

  throw "No EXE installer found in artifact contents."
}

function Stop-SunshineService {
  param([string]$Name)
  $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if ($svc) {
    if ($svc.Status -ne "Stopped") {
      Stop-Service -Name $Name -Force -ErrorAction Stop
      $svc.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(30))
    }
  }
}

function Start-SunshineService {
  param([string]$Name)
  Start-Service -Name $Name -ErrorAction Stop
}

function Install-Sunshine {
  param(
    [string]$InstallerPath,
    [string]$LogPath
  )

  $ext = [IO.Path]::GetExtension($InstallerPath).ToLowerInvariant()
  if ($ext -eq ".exe") {
    $args = "/S"
    $p = Start-Process -FilePath $InstallerPath -ArgumentList $args -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) {
      throw "EXE install failed with exit code $($p.ExitCode)."
    }
    return
  }

  throw "Unsupported installer type (expected .exe): $InstallerPath"
}

function Test-SunshineReady {
  param(
    [string]$Name,
    [int]$Port
  )

  $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if (-not $svc -or $svc.Status -ne "Running") {
    return $false
  }

  $listening = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
  if (-not $listening) {
    return $false
  }

  return $true
}

function Wait-SunshineReady {
  param(
    [string]$Name,
    [int]$Port,
    [int]$TimeoutSec
  )
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Test-SunshineReady -Name $Name -Port $Port) {
      return $true
    }
    Start-Sleep -Seconds 2
  }
  return $false
}

function Backup-Config {
  param(
    [string]$Path,
    [string]$DestinationRoot
  )
  if (-not (Test-Path $Path)) {
    return $null
  }
  $backupPath = Join-Path $DestinationRoot "config-backup"
  New-Item -ItemType Directory -Path $backupPath -Force | Out-Null
  Copy-Item -Path (Join-Path $Path "*") -Destination $backupPath -Recurse -Force
  return $backupPath
}

function Restore-Config {
  param(
    [string]$BackupPath,
    [string]$Path
  )
  if ([string]::IsNullOrWhiteSpace($BackupPath) -or -not (Test-Path $BackupPath)) {
    return
  }

  if (Test-Path $Path) {
    Remove-Item -Path $Path -Recurse -Force -ErrorAction SilentlyContinue
  }
  New-Item -ItemType Directory -Path $Path -Force | Out-Null
  Copy-Item -Path (Join-Path $BackupPath "*") -Destination $Path -Recurse -Force
}

Assert-Admin

if (-not (Test-Path $RollbackInstallerPath)) {
  throw "Rollback installer not found: $RollbackInstallerPath"
}
if ([IO.Path]::GetExtension($RollbackInstallerPath).ToLowerInvariant() -ne ".exe") {
  throw "Rollback installer must be an EXE installer: $RollbackInstallerPath"
}

$headers = Get-GitHubHeaders -AuthToken $Token
$workDir = New-WorkDir -Root $WorkRoot
$artifactZip = Join-Path $workDir "artifact.zip"
$extractDir = Join-Path $workDir "artifact"
$installLog = Join-Path $workDir "install.log"
$rollbackLog = Join-Path $workDir "rollback.log"
$configBackup = $null

New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
$configBackup = Backup-Config -Path $ConfigPath -DestinationRoot $workDir

$installSucceeded = $false

try {
  Write-Host "Validating run id $RunId..."
  Assert-RunSucceeded -RepoName $Repo -Run $RunId -Headers $headers

  Write-Host "Fetching artifact metadata for run $RunId..."
  $downloadUrl = Get-ArtifactDownloadUrl -RepoName $Repo -Run $RunId -Name $ArtifactName -Headers $headers

  Write-Host "Downloading artifact..."
  Download-ArtifactZip -DownloadUrl $downloadUrl -OutZip $artifactZip -Headers $headers

  Write-Host "Extracting artifact..."
  Expand-Archive -Path $artifactZip -DestinationPath $extractDir -Force

  $newInstaller = Find-Installer -SearchDir $extractDir
  Write-Host "Using installer: $newInstaller"

  Write-Host "Stopping $ServiceName..."
  Stop-SunshineService -Name $ServiceName

  Write-Host "Installing new build..."
  Install-Sunshine -InstallerPath $newInstaller -LogPath $installLog

  Write-Host "Starting $ServiceName..."
  Start-SunshineService -Name $ServiceName

  Write-Host "Waiting for readiness..."
  if (-not (Wait-SunshineReady -Name $ServiceName -Port $HealthPort -TimeoutSec $StartupTimeoutSeconds)) {
    throw "Service failed readiness checks within ${StartupTimeoutSeconds}s."
  }

  $installSucceeded = $true
  Write-Host "Deployment successful."
}
catch {
  Write-Warning "Deployment failed: $($_.Exception.Message)"
  Write-Warning "Rolling back with: $RollbackInstallerPath"

  try {
    Stop-SunshineService -Name $ServiceName
  } catch {
    Write-Warning "Failed stopping service during rollback: $($_.Exception.Message)"
  }

  try {
    Install-Sunshine -InstallerPath $RollbackInstallerPath -LogPath $rollbackLog
    Restore-Config -BackupPath $configBackup -Path $ConfigPath
    Start-SunshineService -Name $ServiceName

    if (-not (Wait-SunshineReady -Name $ServiceName -Port $HealthPort -TimeoutSec $StartupTimeoutSeconds)) {
      throw "Rollback install completed, but readiness checks still failed."
    }
    Write-Warning "Rollback succeeded."
  } catch {
    Write-Error "Rollback failed: $($_.Exception.Message)"
    throw
  }

  throw
}
finally {
  if ($installSucceeded) {
    Write-Host "Install log: $installLog"
  } else {
    Write-Host "Install log: $installLog"
    Write-Host "Rollback log: $rollbackLog"
  }
}
