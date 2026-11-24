param([switch]$Create,[string]$CompileProject)
$ErrorActionPreference='Stop'
if(Get-Process ms4d -ErrorAction SilentlyContinue){throw 'IDE already running'}
$taskDir=Join-Path $env:TEMP 'ms4d_fast_modbus_20261122'
$taskLog=Join-Path $taskDir 'project-inspect.log'
$taskHook=Join-Path $PSScriptRoot 'bin/Release/net8.0-windows/BenchProject.dll'
$taskPsi=[Diagnostics.ProcessStartInfo]::new()
$taskPsi.FileName='C:/Program Files/MPSSoft/MasterSCADA 4D 1.3/bin/ms4d.exe'
$taskPsi.WorkingDirectory=Split-Path $taskPsi.FileName
$taskPsi.UseShellExecute=$false
$taskPsi.CreateNoWindow=$true
$taskPsi.RedirectStandardOutput=$true
$taskPsi.RedirectStandardError=$true
$taskPsi.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$taskPsi.Environment['DOTNET_STARTUP_HOOKS']=$taskHook
$taskPsi.Environment['FMB_PROJECT_LOG']=$taskLog
if($Create){$taskPsi.Environment['FMB_CREATE_PROJECT']='1'}
if($CompileProject){$taskPsi.Environment['FMB_COMPILE_PROJECT']=$CompileProject; $taskPsi.ArgumentList.Add('/o');$taskPsi.ArgumentList.Add($CompileProject)}
$taskProcess=[Diagnostics.Process]::Start($taskPsi)
@{pid=$taskProcess.Id;log=$taskLog}|ConvertTo-Json
$taskStdout=$taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr=$taskProcess.StandardError.ReadToEndAsync()
if(-not $taskProcess.WaitForExit(180000)){throw "Owned test IDE timeout, pid=$($taskProcess.Id)"}
$taskStdout.Result | Set-Content -LiteralPath (Join-Path $taskDir 'ide-stdout.log')
$taskStderr.Result | Set-Content -LiteralPath (Join-Path $taskDir 'ide-stderr.log')
if($taskProcess.ExitCode -ne 0){throw "IDE exit $($taskProcess.ExitCode)"}

