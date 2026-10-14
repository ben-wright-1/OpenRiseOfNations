# Run Ghidra post-scripts against the saved, already-analyzed RON project (read-only, no re-analysis).
# Usage: .\ghidra-run.ps1 [-Project RON|RON_d3dgl] Script.java jobs\args.txt [Script2.java jobs\args2.txt ...]
# Script arguments are passed as @argfiles (see ghidra_scripts/ArgFiles.java) because analyzeHeadless.bat
# goes through cmd.exe, which mangles regex characters.
[CmdletBinding(PositionalBinding = $false)]
param(
	[ValidateSet('RON', 'RON_d3dgl')][string]$Project = 'RON',
	[Parameter(ValueFromRemainingArguments)][string[]]$Pairs
)
$ErrorActionPreference = 'Stop'
$re = Split-Path $PSScriptRoot -Parent
$env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot'
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
$env:GHIDRA_HEADLESS_MAXMEM = '12G'

$projDir = if ($Project -eq 'RON') { "$re\ghidra" } else { "$re\ghidra_d3dgl" }
$program = if ($Project -eq 'RON') { 'riseofnations.exe' } else { 'd3dgl.dll' }

$scriptArgs = @()
for ($i = 0; $i -lt $Pairs.Count; $i += 2) {
	$argFile = (Resolve-Path ($Pairs[$i + 1].TrimStart('@'))).Path
	$scriptArgs += @('-postScript', $Pairs[$i], "@$argFile")
}
$log = "$re\jobs\last-run.log"
& 'C:\Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat' $projDir $Project -process $program -noanalysis -readOnly `
	-scriptPath "$re\ghidra_scripts" @scriptArgs -log $log *> "$re\jobs\last-console.txt"
$code = $LASTEXITCODE
Select-String -Path $log -Pattern 'ERROR|Exception|decompiled \d+|> ' | ForEach-Object { $_.Line }
exit $code
