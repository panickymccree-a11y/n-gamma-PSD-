param(
    [string]$BuildRoot=(Join-Path $env:TEMP 'mca_psd_build'),
    [string]$Vivado=''
)
$ErrorActionPreference='Stop'
if(!$Vivado -and $env:VIVADO_BIN){$Vivado=Join-Path $env:VIVADO_BIN 'vivado.bat'}
if(!$Vivado){$vivadoCommand=Get-Command 'vivado.bat' -ErrorAction SilentlyContinue; if($vivadoCommand){$Vivado=$vivadoCommand.Source}}
if(!$Vivado -or !(Test-Path -LiteralPath $Vivado)){throw 'Set VIVADO_BIN or pass -Vivado with the path to vivado.bat.'}
$sourceRoot=[IO.Path]::GetFullPath($PSScriptRoot)
$buildBase=[IO.Path]::GetFullPath($BuildRoot)
if($buildBase -match '[^\x00-\x7F]'){throw 'Vivado 2020.2 build directory must use ASCII characters.'}
if($buildBase.TrimEnd('\') -eq [IO.Path]::GetPathRoot($buildBase).TrimEnd('\')){throw 'Build root cannot be a drive root.'}
$buildHdl=[IO.Path]::GetFullPath((Join-Path $buildBase 'HDL'))
if($buildHdl -eq $sourceRoot){throw 'Use a separate build directory.'}
$marker=Join-Path $buildBase '.mca_psd_build_source'
if(Test-Path -LiteralPath $buildBase){
    if(!(Test-Path -LiteralPath $marker)){throw 'Existing build directory has no PSD ownership marker. Choose a new BuildRoot.'}
    if(([IO.File]::ReadAllText($marker)).Trim() -ne $sourceRoot){throw 'Build directory belongs to another source checkout.'}
}
New-Item -ItemType Directory -Path $buildHdl -Force | Out-Null
[IO.File]::WriteAllText($marker,$sourceRoot,[Text.UTF8Encoding]::new($false))
foreach($part in @('rtl','ip','constraints','scripts','sim')){
    $from=Join-Path $sourceRoot $part
    $to=Join-Path $buildHdl $part
    & robocopy $from $to /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XD build /NFL /NDL /NJH /NJS /NP | Out-Null
    if($LASTEXITCODE -ge 8){throw "Copy failed: $part"}
}
$xpr=[IO.File]::ReadAllText((Join-Path $sourceRoot 'single_channel_mca.xpr'))
$xpr=$xpr.Replace($sourceRoot.Replace('\','/'),$buildHdl.Replace('\','/'))
[IO.File]::WriteAllText((Join-Path $buildHdl 'single_channel_mca.xpr'),$xpr,[Text.UTF8Encoding]::new($false))
$reports=Join-Path $buildHdl 'reports'
New-Item -ItemType Directory -Path $reports -Force | Out-Null
Push-Location $buildHdl
try {
    & $Vivado -mode batch -source scripts/build_psd.tcl -log reports/psd_build.log -journal reports/psd_build.jou *> reports/psd_console.log
    $buildExit=$LASTEXITCODE
} finally {Pop-Location}
$output=Join-Path $sourceRoot 'reports\psd'
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach($log in @('psd_console.log','psd_build.log','psd_build.jou')){
    Copy-Item -LiteralPath (Join-Path $reports $log) -Destination $output
}
if($buildExit -ne 0 -or !([IO.File]::ReadAllText((Join-Path $reports 'psd_console.log')).Contains('PSD_BUILD_COMPLETE'))){throw 'Vivado build failed; see HDL/reports/psd/psd_console.log.'}
Copy-Item -Path (Join-Path $reports 'psd\*') -Destination $output -Force
$delivery=Join-Path $sourceRoot 'delivery'
New-Item -ItemType Directory -Path $delivery -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildHdl 'single_channel_mca.runs\impl_1\single_channel_mca.bit') -Destination (Join-Path $delivery 'single_channel_mca_psd.bit') -Force
$manifest=Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'rtl') -File | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,$_.Name
}
$manifest | Set-Content -LiteralPath (Join-Path $delivery 'rtl_sha256.txt') -Encoding utf8
Write-Output "PSD build and delivery complete: $delivery (launch_runs -jobs 12)"
