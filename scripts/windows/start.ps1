$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path "$here\key.txt")) {
  $b = New-Object byte[] 32
  [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
  (($b | ForEach-Object { $_.ToString('x2') }) -join '') | Set-Content "$here\key.txt"
}
$env:LUMEN_API_KEY = (Get-Content "$here\key.txt").Trim()
$env:LLM_MODEL_PATH = "$here\model.gguf"
$env:LUMEN_CONTEXT_SIZE = "4096"
$env:LUMEN_GPU_LAYERS = "0"
$env:LUMEN_BIND = "127.0.0.1"
$env:LUMEN_PORT = "8080"
$env:LUMEN_TRUST_PROXY = "1"
$env:RAG_INDEX_PATH = "$here\rag_index.bin"
Write-Host "Clé API :" $env:LUMEN_API_KEY
Start-Process -FilePath "$here\Lumen.exe" -ArgumentList "serve","8080" -WindowStyle Hidden
Start-Sleep 5
& "$here\cloudflared.exe" tunnel --no-autoupdate --url http://127.0.0.1:8080
