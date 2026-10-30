# Select the torch build that matches this machine, then sync the uv environment.
#   .\setup_env.ps1                     # probe the GPU and install accordingly
#   .\setup_env.ps1 -Torch cpu          # force the CPU build
#   .\setup_env.ps1 -Torch cu130        # force Blackwell (CUDA 13.0) build
#   .\setup_env.ps1 -Tts                # also install the GPT-SoVITS inference stack
#   .\setup_env.ps1 -Print              # only show the uv command it would run
param(
    [ValidateSet("auto", "cpu", "cu126", "cu130")]
    [string]$Torch = "auto",
    [switch]$Tts,
    [switch]$Print
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    throw "uv is not on PATH. See https://docs.astral.sh/uv/getting-started/installation/"
}

# uv cannot resolve hardware, so the choice happens here: compute capability decides
# which dependency group is synced. cu126 wheels stop at sm_90, Blackwell needs cu130.
function Get-ComputeCap {
    if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
        $caps = & nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>$null |
            ForEach-Object { if ($_ -match "[0-9]+(\.[0-9]+)?") { [double]$Matches[0] } }
        # Hybrid-optimus laptops list several GPUs; the fastest one sets the build.
        if ($caps) { return ($caps | Measure-Object -Maximum).Maximum }
    }
    # Old drivers omit compute_cap; infer from the adapter name when one is visible.
    $nvidia = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "NVIDIA" }
    if (-not $nvidia) { return $null }
    if (($nvidia.Name -join " ") -match "RTX 50|B\d00|Blackwell") { return 10.0 }
    return 7.5
}

if ($Torch -eq "auto") {
    $cap = Get-ComputeCap
    if ($null -eq $cap) {
        $Torch = "cpu"
        Write-Host "No NVIDIA GPU detected -> CPU build"
    } elseif ($cap -ge 10.0) {
        $Torch = "cu130"
        Write-Host "GPU compute capability $cap -> CUDA 13.0 build (Blackwell)"
    } else {
        $Torch = "cu126"
        Write-Host "GPU compute capability $cap -> CUDA 12.6 build"
    }
}

$group = if ($Torch -eq "cpu") { "cpu" } else { "gpu-$Torch" }
# cpu is the default group; the GPU groups conflict with it, so the default has to be
# turned off explicitly before one of them can be enabled.
if ($group -eq "cpu") { $argv = @("sync") } else { $argv = @("sync", "--no-default-groups", "--group", $group) }
# tts is orthogonal to the backend groups and simply rides along.
if ($Tts) {
    $argv += @("--group", "tts")
    $group = "$group+tts"
}

Write-Host "uv $($argv -join ' ')"
if ($Print) { return }
& uv @argv
if ($LASTEXITCODE -ne 0) { throw "uv sync failed" }

& "$PSScriptRoot\.venv\Scripts\python.exe" -c @"
import torch
print('torch', torch.__version__, '| cuda available:', torch.cuda.is_available())
if '$Torch' != 'cpu' and not torch.cuda.is_available():
    print('WARNING: a GPU build was installed but CUDA is unavailable (driver missing or too old)')
"@
