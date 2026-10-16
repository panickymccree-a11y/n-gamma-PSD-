param(
    [int]$Seconds=60,
    [string]$QtRoot=$env:QT_ROOT,
    [string]$VcRuntime=$env:VC_RUNTIME
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($PSScriptRoot)
if(!$QtRoot -or !(Test-Path -LiteralPath (Join-Path $QtRoot 'bin\Qt6Core.dll'))){
    throw 'Set QT_ROOT to the Qt MSVC kit directory, or pass -QtRoot.'
}
$env:PATH=(Join-Path $QtRoot 'bin')+';'+$env:PATH
if($VcRuntime -and (Test-Path -LiteralPath (Join-Path $VcRuntime 'vcruntime140.dll'))){
    $env:PATH=$VcRuntime+';'+$env:PATH
}
$env:QT_QPA_PLATFORM='offscreen'
$results=Join-Path $root 'throughput_results'
New-Item -ItemType Directory -Path $results -Force | Out-Null
$cases=@(
    @('spectrum',300000,64),@('spectrum',1500000,64),
    @('psd',300000,40),@('psd',1500000,40),
    @('spectrum',300000,8),@('spectrum',1500000,8),
    @('psd',300000,8),@('psd',1500000,8)
)
$failures=0
foreach($case in $cases){
    $name='{0}_{1}_{2}_{3}s' -f $case[0],$case[1],$case[2],$Seconds
    $json=Join-Path $results ($name+'.json')
    Write-Output "START $name"
    & (Join-Path $root 'build-psd\mca_throughput_bench.exe') $case[0] $case[1] $case[2] $Seconds $json *> (Join-Path $results ($name+'.log'))
    if(!(Test-Path -LiteralPath $json)){throw "Benchmark did not produce results: $name"}
    $result=Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    if(!$result.pass){$failures++}
    Write-Output ('RESULT {0}: pass={1}, received={2}, lost={3}, Hz={4:N2}, P95={5:N2}ms, stop={6:N2}ms' -f $name,$result.pass,$result.received_events,$result.lost_packets,$result.refresh_hz,$result.refresh_p95_ms,$result.stop_latency_ms)
}
Write-Output "COMPLETE failures=$failures"
if($failures){exit 1}
