param(
  [string]$RepoDir = "C:\dev\Sunshine",
  [string]$Branch = "master",
  [string]$ServiceName = "SunshineService",
  [int]$HealthPort = 47990,
  [int]$StartupTimeoutSeconds = 60,
  [string]$RollbackInstaller = "C:\ops\sunshine-prev-installer.exe",
  [string]$MsysRoot = "C:\msys64",
  [int]$InstallerTimeoutSeconds = 180,
  [string]$LogRoot = "C:\ops\logs"
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
  $principal = [Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
  $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if (-not $isAdmin) {
    throw "This script must run in an elevated PowerShell session."
  }
}

function Assert-Command {
  param([string]$Name)

  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command not found on PATH: $Name"
  }
}

function Write-DeployLog {
  param([string]$Message)

  $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
  $line = "[$timestamp] $Message"
  Write-Host $line
  Add-Content -Path $script:DeployLogPath -Value $line
}

function Get-SunshineInstallVersion {
  $paths = @(
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine",
    "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine"
  )

  foreach ($path in $paths) {
    $item = Get-ItemProperty -Path $path -ErrorAction SilentlyContinue
    if ($item -and $item.DisplayVersion) {
      return [string]$item.DisplayVersion
    }
  }

  return $null
}

function Get-SunshineBinaryStamp {
  $binaryPath = "C:\Program Files\Sunshine\sunshine.exe"
  if (-not (Test-Path $binaryPath)) {
    return $null
  }

  $item = Get-Item $binaryPath
  return [string]::Format("{0}|{1}", $item.LastWriteTimeUtc.Ticks, $item.Length)
}

function Invoke-MsysBash {
  param(
    [string]$MsysPath,
    [string]$Script
  )

  $bashPath = Join-Path $MsysPath "usr\bin\bash.exe"
  if (-not (Test-Path $bashPath)) {
    throw "MSYS2 bash not found: $bashPath"
  }

  & $bashPath -lc $Script
  if ($LASTEXITCODE -ne 0) {
    throw "MSYS2 command failed with exit code $LASTEXITCODE."
  }
}

function Stop-SunshineService {
  param([string]$Name)

  $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if ($svc -and $svc.Status -ne "Stopped") {
    Stop-Service -Name $Name -Force -ErrorAction Stop
    $svc.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(30))
  }
}

function Start-SunshineService {
  param([string]$Name)

  $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if (-not $svc) {
    throw "Service not found: $Name"
  }

  if ($svc.Status -ne "Running") {
    Start-Service -Name $Name -ErrorAction Stop
  }
}

function Install-Sunshine {
  param(
    [string]$InstallerPath,
    [int]$TimeoutSeconds
  )

  if ([IO.Path]::GetExtension($InstallerPath).ToLowerInvariant() -ne ".exe") {
    throw "Unsupported installer type (expected .exe): $InstallerPath"
  }

  $beforeVersion = Get-SunshineInstallVersion
  $beforeBinaryStamp = Get-SunshineBinaryStamp
  $process = Start-Process -FilePath $InstallerPath -ArgumentList "/S" -PassThru -NoNewWindow
  if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    try {
      Stop-Process -Id $process.Id -Force -ErrorAction Stop
    }
    catch {
      Write-DeployLog "Failed to stop timed out installer process $($process.Id): $($_.Exception.Message)"
    }
    throw "Installer timed out after ${TimeoutSeconds}s."
  }

  $afterVersion = Get-SunshineInstallVersion
  $afterBinaryStamp = Get-SunshineBinaryStamp
  $changed = $beforeVersion -ne $afterVersion -or $beforeBinaryStamp -ne $afterBinaryStamp

  if ($null -ne $process.ExitCode) {
    if ($process.ExitCode -ne 0 -and -not $changed) {
      throw "Installer failed with exit code $($process.ExitCode)."
    }
    if ($process.ExitCode -ne 0 -and $changed) {
      Write-DeployLog "Installer returned exit code $($process.ExitCode), but Sunshine install files changed. Continuing."
    }
    return
  }

  if (-not $changed) {
    throw "Installer exited without an exit code and no Sunshine install changes were detected."
  }
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

function Find-Installer {
  param([string]$SearchDir)

  $cpackInstaller = Get-ChildItem -Path (Join-Path $SearchDir "cpack_artifacts") -Recurse -File -Filter "Sunshine.exe" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($cpackInstaller) {
    return $cpackInstaller.FullName
  }

  $preferred = Get-ChildItem -Path $SearchDir -Recurse -File -Filter "*installer.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "vigembus_installer.exe" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($preferred) {
    return $preferred.FullName
  }

  $anyExe = Get-ChildItem -Path $SearchDir -Recurse -File -Filter "*.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "*Sunshine*" -and $_.Name -ne "vigembus_installer.exe" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($anyExe) {
    return $anyExe.FullName
  }

  throw "Could not find generated installer under $SearchDir"
}

Assert-Admin

if (-not (Test-Path $RepoDir)) {
  throw "Repo directory not found: $RepoDir"
}

if (-not (Test-Path $RollbackInstaller)) {
  throw "Rollback installer not found: $RollbackInstaller"
}

if ([IO.Path]::GetExtension($RollbackInstaller).ToLowerInvariant() -ne ".exe") {
  throw "Rollback installer must be an EXE installer: $RollbackInstaller"
}

Assert-Command -Name "git"

$buildRoot = Join-Path $RepoDir "build"
$deploySucceeded = $false
$repoDirMsys = $RepoDir -replace '\\', '/'
$repoDirMsys = $repoDirMsys -replace '^([A-Za-z]):', '/$1'
$msysNpmCmd = (Join-Path $MsysRoot "ucrt64\bin\npm.cmd") -replace '\\', '/'
New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
$script:DeployLogPath = Join-Path $LogRoot ("deploy-sunshine-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")

try {
  Write-DeployLog "Updating source in $RepoDir..."
  Set-Location $RepoDir
  & git fetch origin
  & git checkout $Branch
  & git pull --ff-only origin $Branch
  & git submodule update --init --recursive

  if (-not (Test-Path $buildRoot)) {
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
  }

  Write-DeployLog "Building and packaging in MSYS2 UCRT64..."
  Invoke-MsysBash -MsysPath $MsysRoot -Script "export MSYSTEM=UCRT64; export CHERE_INVOKING=1; export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd '$repoDirMsys' && cmake -B build -G Ninja -S . -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF -DNPM=$msysNpmCmd && ninja -C build && cpack --config build/CPackConfig.cmake -G NSIS"

  $newInstaller = Find-Installer -SearchDir $buildRoot
  Write-DeployLog "Using installer: $newInstaller"

  Write-DeployLog "Stopping $ServiceName..."
  Stop-SunshineService -Name $ServiceName

  Write-DeployLog "Installing new build with timeout ${InstallerTimeoutSeconds}s..."
  Install-Sunshine -InstallerPath $newInstaller -TimeoutSeconds $InstallerTimeoutSeconds

  Write-DeployLog "Starting $ServiceName..."
  Start-SunshineService -Name $ServiceName

  Write-DeployLog "Waiting for readiness..."
  if (-not (Wait-SunshineReady -Name $ServiceName -Port $HealthPort -TimeoutSec $StartupTimeoutSeconds)) {
    throw "Health check failed after install."
  }

  $deploySucceeded = $true
  Write-DeployLog "Deploy OK"
}
catch {
  Write-DeployLog "Deploy failed: $($_.Exception.Message)"

  if (Test-Path $RollbackInstaller) {
    Write-DeployLog "Rolling back with: $RollbackInstaller"
    try {
      Stop-SunshineService -Name $ServiceName
    }
    catch {
      Write-DeployLog "Failed stopping service during rollback: $($_.Exception.Message)"
    }

    Write-DeployLog "Installing rollback build with timeout ${InstallerTimeoutSeconds}s..."
    Install-Sunshine -InstallerPath $RollbackInstaller -TimeoutSeconds $InstallerTimeoutSeconds
    Write-DeployLog "Starting $ServiceName after rollback..."
    Start-SunshineService -Name $ServiceName

    if (-not (Wait-SunshineReady -Name $ServiceName -Port $HealthPort -TimeoutSec $StartupTimeoutSeconds)) {
      throw "Rollback completed, but health check still failed."
    }
    Write-DeployLog "Rollback succeeded."
  }

  throw
}
finally {
  Write-Host "Deploy log: $script:DeployLogPath"
  if ($deploySucceeded) {
    Write-Host "Deployment successful."
  }
}
