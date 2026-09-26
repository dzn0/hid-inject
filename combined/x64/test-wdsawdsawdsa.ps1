$ErrorActionPreference = 'Stop'
$sender = Join-Path $PSScriptRoot 'hid-inject-shared-send.exe'
if (-not (Test-Path -LiteralPath $sender -PathType Leaf)) {
    throw "Emissor nao encontrado: $sender"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Abra o PowerShell como administrador para acessar a fila do driver.'
}

$startInfo = [Diagnostics.ProcessStartInfo]::new($sender, '--stdin')
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardInput = $true
$process = [Diagnostics.Process]::Start($startInfo)
$process.StandardInput.AutoFlush = $true
$sent = 0

try {
    Write-Output 'Aguardando 3 segundos. Selecione a janela que recebera as teclas.'
    Start-Sleep -Seconds 3
    if ($process.HasExited) {
        throw "O emissor encerrou antes do teste: exit=$($process.ExitCode)"
    }

    # W D S A W D S A W D S A (scan codes Set 1), repeated for five seconds.
    $scanCodes = @(17, 32, 31, 30, 17, 32, 31, 30, 17, 32, 31, 30)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 5) {
        if ($process.HasExited) {
            throw "O emissor encerrou durante o teste: exit=$($process.ExitCode)"
        }
        $scanCode = $scanCodes[$sent % $scanCodes.Count]
        $process.StandardInput.WriteLine("20 $scanCode 0")
        $sent++
        Start-Sleep -Milliseconds 50
    }
} finally {
    if (-not $process.HasExited) {
        $process.StandardInput.Close()
    }
    $process.WaitForExit()
}

if ($process.ExitCode -ne 0) {
    throw "O emissor falhou: exit=$($process.ExitCode)"
}
Write-Output "Comandos de teclado enviados: $sent; exit=0"
