$WshShell = New-Object -ComObject WScript.Shell
$desktop = [System.Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop "Bitcoin Miner.lnk"
$targetDir = "c:\Users\DELL\Desktop\BTCMining Program"
$vbsPath = Join-Path $targetDir "Start_Miner_GUI.vbs"

$Shortcut = $WshShell.CreateShortcut($shortcutPath)
$Shortcut.TargetPath = "wscript.exe"
$Shortcut.Arguments = "`"$vbsPath`""
$Shortcut.WorkingDirectory = $targetDir
$Shortcut.Description = "Bitcoin Stratum V1 Miner (Dedicated GUI)"

# If miner.exe has an icon, or shell32.dll
$Shortcut.IconLocation = "shell32.dll, 13" # Key/Lock/Hardware icon
$Shortcut.Save()

Write-Host "Shortcut created at: $shortcutPath"
