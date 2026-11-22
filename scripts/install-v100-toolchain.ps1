# Installs the VC++ 2010 (v100) x64 compiler without full VS2010, by installing
# the compiler MSIs from the Windows SDK 7.1 ISO directly. That sidesteps the SDK
# bootstrapper, which fails when a newer VC++ 2010 redistributable is present.
# Then applies the VC++ 2010 SP1 compiler update (KB2519277).
#
# Run elevated:  powershell -ExecutionPolicy Bypass -File install-v100-toolchain.ps1 <isoDrive> <dir with KB2519277 exe>
#
# Downloads (Microsoft):
#   https://download.microsoft.com/download/F/1/0/F10113F5-B750-4969-A255-274341AC6BCE/GRMSDKX_EN_DVD.iso
#   https://download.microsoft.com/download/7/5/0/75040801-126C-4591-BCE4-4CD1FD1499AA/VC-Compiler-KB2519277.exe
param([string]$Drive = 'D', [string]$KbDir = "$env:USERPROFILE\Downloads\vc2010-toolchain")

$log = Join-Path $KbDir 'install-log.txt'
"started $(Get-Date)" | Out-File $log -Encoding utf8

foreach ($c in 'vc_stdx86', 'vc_stdamd64', 'WinSDKBuild_amd64') {
    $msi = "${Drive}:\Setup\$c\$c.msi"
    $p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart /l*v `"$KbDir\$c.log`"" -Wait -PassThru
    "$c exit $($p.ExitCode)" | Out-File $log -Append -Encoding utf8
}

$p = Start-Process "$KbDir\VC-Compiler-KB2519277.exe" -ArgumentList '/q /norestart' -Wait -PassThru
"KB2519277 exit $($p.ExitCode)" | Out-File $log -Append -Encoding utf8
"done $(Get-Date)" | Out-File $log -Append -Encoding utf8
