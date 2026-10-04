<#
.SYNOPSIS
  Build / flash / monitor the CR-10 print server from a Windows terminal.

.DESCRIPTION
  Building happens inside Docker (espressif/idf image, see Dockerfile).
  Docker Desktop cannot see COM ports, so flashing uses the standalone
  esptool.exe on the host (downloaded automatically, no Python needed) and the
  monitor runs in Docker, connected to the board through esp_rfc2217_server.exe.

.EXAMPLE
  .\idf.ps1 build
  .\idf.ps1 ports
  .\idf.ps1 flash COM5
  .\idf.ps1 monitor COM5
  .\idf.ps1 flash-monitor COM5
  .\idf.ps1 build-dry ; .\idf.ps1 flash COM5 -Dry
#>
param(
    [Parameter(Position = 0)][string]$Command = "help",
    [Parameter(Position = 1)][string]$Port = "",
    [switch]$Dry,
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$Image = "cr10-idf"
$Cache = "cr10-idf-cache"
$EsptoolVersion = "v5.4.0"
$ToolDir = Join-Path $Root "tools\esptool"
$RfcPort = 4000
$BuildDir = if ($Dry) { "build_dry" } else { "build" }

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

function Ensure-Docker {
    docker info --format "{{.OSType}}" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Docker is not running. Start Docker Desktop and try again." }
    docker image inspect $Image *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Building Docker image '$Image' (first time only, large download)..." -ForegroundColor Cyan
        docker build -t $Image $Root
        if ($LASTEXITCODE -ne 0) { Fail "docker build failed" }
    }
}

function Invoke-Idf([string[]]$IdfArgs, [switch]$Interactive) {
    Ensure-Docker
    $dockerArgs = @("run", "--rm", "-v", "${Root}:/project", "-v", "${Cache}:/opt/idf-cache", "-w", "/project")
    if ($Interactive) { $dockerArgs += "-it" }
    $dockerArgs += @($Image, "idf.py")
    if ($Dry) {
        $dockerArgs += @("-B", "build_dry", "-D", "SDKCONFIG=build_dry/sdkconfig", "-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.dryrun")
    }
    & docker @($dockerArgs + $IdfArgs)
    if ($LASTEXITCODE -ne 0) { Fail "idf.py $($IdfArgs -join ' ') failed" }
}

function Get-Tool([string]$name) {
    $exe = Get-ChildItem -Path $ToolDir -Recurse -Filter $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($exe) { return $exe.FullName }
    Write-Host "Downloading esptool $EsptoolVersion (standalone Windows build)..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force $ToolDir | Out-Null
    $zip = Join-Path $ToolDir "esptool.zip"
    $url = "https://github.com/espressif/esptool/releases/download/$EsptoolVersion/esptool-$EsptoolVersion-windows-amd64.zip"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $ToolDir -Force
    Remove-Item $zip
    $exe = Get-ChildItem -Path $ToolDir -Recurse -Filter $name | Select-Object -First 1
    if (-not $exe) { Fail "$name not found in the esptool release archive" }
    return $exe.FullName
}

function Require-Port {
    if (-not $Port) {
        Show-Ports
        Fail "Give the COM port of the board's UART USB-C port, e.g.: .\idf.ps1 $Command COM5"
    }
}

function Show-Ports {
    Write-Host "Serial ports:" -ForegroundColor Cyan
    $ports = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match "\(COM\d+\)" }
    if (-not $ports) { Write-Host "  (none found - is the board plugged into its UART port?)"; return }
    $ports | ForEach-Object { Write-Host "  $($_.Name)" }
}

function Invoke-Flash {
    Require-Port
    $argsFile = Join-Path $Root "$BuildDir\flash_args"
    if (-not (Test-Path $argsFile)) { Fail "$BuildDir\flash_args not found - run '.\idf.ps1 build$(if($Dry){'-dry'})' first" }
    $esptool = Get-Tool "esptool.exe"
    # flash_args holds the flash options and "<offset> <file>" pairs, paths relative to the build dir
    $flashArgs = (Get-Content $argsFile -Raw) -split "\s+" | Where-Object { $_ }
    Push-Location (Join-Path $Root $BuildDir)
    try {
        & $esptool --chip esp32s3 -p $Port -b $Baud --before default-reset --after hard-reset write-flash @flashArgs
        if ($LASTEXITCODE -ne 0) { Fail "flashing failed (hold BOOT and tap RESET to force download mode, then retry)" }
    } finally { Pop-Location }
    Write-Host "Flashed $BuildDir to $Port" -ForegroundColor Green
}

function Start-RfcServer {
    $srv = Get-Tool "esp_rfc2217_server.exe"
    Write-Host "Starting RFC2217 bridge $Port -> tcp:$RfcPort (allow it in Windows Firewall if asked)" -ForegroundColor Cyan
    $p = Start-Process -FilePath $srv -ArgumentList @("-p", "$RfcPort", $Port) -PassThru -WindowStyle Minimized
    Start-Sleep -Milliseconds 1500
    if ($p.HasExited) { Fail "esp_rfc2217_server exited - is $Port in use (close other serial monitors)?" }
    return $p
}

function Invoke-Monitor {
    Require-Port
    $p = Start-RfcServer
    try {
        Invoke-Idf @("-p", "rfc2217://host.docker.internal:${RfcPort}?ign_set_control", "monitor") -Interactive
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
}

switch ($Command.ToLower()) {
    "build"         { Invoke-Idf @("build") }
    "build-dry"     { $Dry = $true; $BuildDir = "build_dry"; Invoke-Idf @("build") }
    "menuconfig"    { Invoke-Idf @("menuconfig") -Interactive }
    "clean"         { Invoke-Idf @("fullclean") }
    "size"          { Invoke-Idf @("size") }
    "shell"         { Ensure-Docker; docker run --rm -it -v "${Root}:/project" -v "${Cache}:/opt/idf-cache" -w /project $Image bash }
    "ports"         { Show-Ports }
    "flash"         { Invoke-Flash }
    "monitor"       { Invoke-Monitor }
    "flash-monitor" { Invoke-Flash; Invoke-Monitor }
    "erase"         {
        Require-Port
        $esptool = Get-Tool "esptool.exe"
        & $esptool --chip esp32s3 -p $Port erase-flash
    }
    "serve"         {
        # Foreground RFC2217 bridge, e.g. for the VS Code dev container tasks
        Require-Port
        $srv = Get-Tool "esp_rfc2217_server.exe"
        & $srv -p $RfcPort $Port
    }
    default {
        Write-Host @"
CR-10 print server - build & flash helper

  .\idf.ps1 build                 build firmware (Docker)
  .\idf.ps1 build-dry             build the dry-run (simulated printer) variant
  .\idf.ps1 menuconfig            configure pins / options
  .\idf.ps1 ports                 list COM ports
  .\idf.ps1 flash COM5 [-Dry]     flash via the board's UART USB-C port
  .\idf.ps1 monitor COM5          serial monitor (Ctrl+] to quit)
  .\idf.ps1 flash-monitor COM5    both
  .\idf.ps1 erase COM5            erase the whole flash (clears Wi-Fi settings)
  .\idf.ps1 serve COM5            RFC2217 bridge for VS Code dev container tasks
  .\idf.ps1 clean | size | shell
"@
    }
}
