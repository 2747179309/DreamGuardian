param(
    [string]$PortName = "COM9",
    [string]$LogPath = "$PSScriptRoot\..\serial_live.log"
)

$serial = [System.IO.Ports.SerialPort]::new($PortName, 115200, 'None', 8, 'One')
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = 200
$serial.Encoding = [System.Text.Encoding]::UTF8

$utf8 = [System.Text.UTF8Encoding]::new($false)
$writer = [System.IO.StreamWriter]::new($LogPath, $true, $utf8)
$writer.AutoFlush = $true

try {
    $serial.Open()
    $writer.WriteLine("`r`n===== serial watch started $(Get-Date -Format o) $PortName =====")
    while ($true) {
        $chunk = $serial.ReadExisting()
        if ($chunk.Length -gt 0) {
            $writer.Write($chunk)
        }
        Start-Sleep -Milliseconds 40
    }
}
catch {
    $writer.WriteLine("`r`n===== serial watch error $(Get-Date -Format o): $($_.Exception.Message) =====")
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
    $writer.Dispose()
}
