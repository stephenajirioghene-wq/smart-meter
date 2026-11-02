# Serves the dashboard folder at http://localhost:8080 for local testing.
# ES modules don't load from file://, so the pages need a web server.
# Run:  powershell -ExecutionPolicy Bypass -File .\serve-dashboard.ps1
# Stop: Ctrl+C
# Use "localhost" (not 127.0.0.1): Firebase Auth allows localhost by default.

param([int]$Port = 8080)

$root = Join-Path $PSScriptRoot "dashboard"
$types = @{
  ".html" = "text/html; charset=utf-8"; ".js" = "text/javascript; charset=utf-8"
  ".css" = "text/css; charset=utf-8"; ".json" = "application/json"
  ".svg" = "image/svg+xml"; ".png" = "image/png"; ".ico" = "image/x-icon"
}

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
Write-Host "Serving $root at http://localhost:$Port/  (Ctrl+C to stop)"

try {
  while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    # One request failing (e.g. the browser cancels a load) must not stop the server
    try {
      $path = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart("/"))
      if ($path -eq "") { $path = "index.html" }
      $file = [IO.Path]::GetFullPath((Join-Path $root $path))

      if ($file.StartsWith($root) -and (Test-Path $file -PathType Leaf)) {
        $bytes = [IO.File]::ReadAllBytes($file)
        $ext = [IO.Path]::GetExtension($file).ToLower()
        $ctx.Response.ContentType = $(if ($types[$ext]) { $types[$ext] } else { "application/octet-stream" })
        $ctx.Response.Headers.Add("Cache-Control", "no-store")
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
      } else {
        $ctx.Response.StatusCode = 404
      }
      Write-Host "$($ctx.Response.StatusCode) /$path"
    } catch {
      Write-Host "dropped /$path ($($_.Exception.Message))"
    } finally {
      try { $ctx.Response.Close() } catch {}
    }
  }
} finally {
  $listener.Stop()
}
