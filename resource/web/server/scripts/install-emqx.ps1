param([Parameter(Mandatory=$true)][string]$Image)
$ErrorActionPreference = 'Stop'
$dockerPath = Join-Path $env:ProgramFiles 'Docker/Docker/resources/bin/docker.exe'
if (!(Test-Path $dockerPath)) {
    $command = Get-Command docker -ErrorAction SilentlyContinue
    if ($command) { $dockerPath = $command.Source }
}
if (!(Test-Path $dockerPath)) {
    Write-Output 'Installing Docker Desktop with winget. Approve the Windows administrator prompt if shown.'
    if (!(Get-Command winget -ErrorAction SilentlyContinue)) {
        throw 'winget is unavailable. Install Microsoft App Installer, then retry.'
    }
    & winget install --exact --id Docker.DockerDesktop --source winget --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0) {
        throw "Docker installation failed (exit $LASTEXITCODE). Check installer output and any restart requirement, then retry."
    }
}
if (!(Test-Path $dockerPath)) { throw 'Docker installation did not produce docker.exe. Restart Windows if requested and retry.' }
$desktop = Join-Path $env:ProgramFiles 'Docker/Docker/Docker Desktop.exe'
$ErrorActionPreference = 'Continue'
& $dockerPath info --format '{{.OSType}}' 2>$null
$infoExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($infoExit -ne 0 -and (Test-Path $desktop)) {
    Start-Process -FilePath $desktop -WindowStyle Hidden
}
Write-Output 'Waiting for Docker Linux engine (up to 5 minutes)...'
$ready = $false
for ($attempt = 0; $attempt -lt 60; $attempt++) {
    $ErrorActionPreference = 'Continue'
    $info = & $dockerPath info --format '{{.OSType}}' 2>$null
    $infoExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($infoExit -eq 0 -and $info -eq 'linux') { $ready = $true; break }
    Start-Sleep -Seconds 5
}
if (!$ready) {
    throw 'Docker is installed but Linux engine is not ready. Complete Docker Desktop first-run setup, check WSL 2/virtualization or restart Windows if required, then retry.'
}
& $dockerPath pull $Image
exit $LASTEXITCODE
