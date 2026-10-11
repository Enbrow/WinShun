# A resident Win顺 plugin in Windows PowerShell: JSON-RPC lines on stdio.
# The template every PowerShell plugin starts from: UTF-8 both ways, one
# line read, one line written, no profile.
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

$settings = @{}
while ($null -ne ($line = $in.ReadLine())) {
    if ($line.Trim() -eq '') { continue }
    try { $m = $line | ConvertFrom-Json } catch { continue }
    switch ($m.method) {
        'initialize' {
            $settings = $m.params.settings
            Send @{ id = $m.id; result = @{ protocolVersion = '1'; name = 'bench.ps' } }
        }
        'query' {
            $text = [string]$m.params.text
            $items = @()
            $items += @{ id = 'echo'; title = "回声：$text"; subtitle = "PID $PID · $($m.params.command)/$($m.params.trigger)"; score = 80; copy = $text }
            $items += @{ id = 'upper'; title = $text.ToUpperInvariant(); subtitle = '大写 · upper case'; copy = $text.ToUpperInvariant() }
            $items += @{ id = 'len'; title = "$($text.Length) 个字符"; subtitle = 'length'; valid = $false }
            Send @{ id = $m.id; result = @{ items = $items } }
        }
        'action' {
            Send @{ id = $m.id; result = @{ effects = @(@{ type = 'notify'; text = "已执行 $($m.params.action)" }, @{ type = 'hide' }) } }
        }
        'settingsChanged' { $settings = $m.params.settings }
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
