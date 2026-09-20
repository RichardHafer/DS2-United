$ErrorActionPreference = 'Stop'
$dumpPath = 'C:\DS2CrashDumps'
New-Item -ItemType Directory -Path $dumpPath -Force | Out-Null
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine,
    [Microsoft.Win32.RegistryView]::Registry64)
try {
    $key = $base.CreateSubKey('SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\deadspace2.exe')
    try {
        $key.SetValue('DumpFolder', $dumpPath, [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $key.SetValue('DumpType', 2, [Microsoft.Win32.RegistryValueKind]::DWord)
        $key.SetValue('DumpCount', 5, [Microsoft.Win32.RegistryValueKind]::DWord)
    } finally {
        if ($key) { $key.Dispose() }
    }
} finally {
    $base.Dispose()
}
