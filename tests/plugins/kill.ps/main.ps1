# A resident Win顺 plugin in Windows PowerShell (JSON-RPC lines on stdio,
# see tests/plugins/bench.ps): `kill <name or PID>` lists the processes so
# named, newest biggest first; Enter ends one, Ctrl+Enter ends it with its
# children. UTF-8 both ways; saved with a BOM (5.1 reads a bare .ps1 as ANSI).
[Console]::OutputEncoding = [Text.Encoding]::UTF8
[Console]::InputEncoding = [Text.Encoding]::UTF8
$OutputEncoding = [Text.Encoding]::UTF8
$in = [Console]::In
$out = [Console]::Out

function Send($message) {
    $message.jsonrpc = '2.0'
    $out.WriteLine(($message | ConvertTo-Json -Compress -Depth 8))
    $out.Flush()
}

while ($null -ne ($line = $in.ReadLine())) {
    if ($line.Trim() -eq '') { continue }
    try { $m = $line | ConvertFrom-Json } catch { continue }
    switch ($m.method) {
        'initialize' {
            Send @{ id = $m.id; result = @{ protocolVersion = '1'; name = 'kill.ps' } }
        }
        'query' {
            $text = ([string]$m.params.text).Trim()
            $procs = Get-Process | Where-Object { $_.Id -ne $PID }
            if ($text -match '^\d+$') {
                $procs = $procs | Where-Object { $_.Id -eq [int]$text }
            } elseif ($text -ne '') {
                $procs = $procs | Where-Object { $_.ProcessName -like "*$text*" }
            }
            $items = @()
            foreach ($p in ($procs | Sort-Object WorkingSet64 -Descending | Select-Object -First 50)) {
                $path = $null
                try { $path = $p.Path } catch { }
                $icon = if ($path) { "file:$path" } else { 'glyph:U+E7F8' }
                $mb = [int]($p.WorkingSet64 / 1MB)
                $items += @{
                    id = "pid-$($p.Id)"; title = $p.ProcessName; subtitle = "PID $($p.Id) · $mb MB"
                    icon = $icon; copy = "$($p.Id)"; data = @{ pid = $p.Id }
                }
            }
            Send @{ id = $m.id; result = @{ items = $items } }
        }
        'action' {
            $target = [int]$m.params.item.data.pid
            $title = [string]$m.params.item.title
            try {
                if ($m.params.action -eq 'killTree') {
                    taskkill /PID $target /T /F | Out-Null
                } else {
                    Stop-Process -Id $target -Force -ErrorAction Stop
                }
                Send @{ id = $m.id; result = @{ effects = @(@{ type = 'notify'; text = "已结束 $title" }, @{ type = 'hide' }) } }
            } catch {
                Send @{ id = $m.id; error = @{ code = -32000; message = "没有结束 ${title}：$($_.Exception.Message)" } }
            }
        }
        'settingsChanged' { }
        'cancel' { }
        'shutdown' {
            Send @{ id = $m.id; result = @{} }
            exit 0
        }
        default {
            if ($null -ne $m.id) { Send @{ id = $m.id; error = @{ code = -32601; message = "Method not found: $($m.method)" } } }
        }
    }
}
