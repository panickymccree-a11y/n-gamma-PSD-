@echo off
rem Compile and run the single-channel MCA behavioural regressions with XSim.
rem
rem Usage:  sim\run_xsim.cmd [testbench ...]
rem
rem Every generated file lands in sim\build\xsim.  With no arguments all
rem regressions run; otherwise only the named testbenches run.
setlocal enabledelayedexpansion

set "SIM_DIR=%~dp0"
set "PROJ_DIR=%SIM_DIR%.."
set "RTL_DIR=%PROJ_DIR%\rtl"
set "WORK_DIR=%SIM_DIR%build\xsim"
rem Set VIVADO_BIN to the Vivado 2020.2 bin directory before running.
if not defined VIVADO_BIN (
  echo Set VIVADO_BIN to the Vivado 2020.2 bin directory.
  exit /b 2
)
if not exist "%VIVADO_BIN%\xvlog.bat" (
  echo Cannot find xvlog.bat under "%VIVADO_BIN%".
  exit /b 2
)
set "PATH=%VIVADO_BIN%;%PATH%"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
pushd "%WORK_DIR%"

set RTL_FILES=%RTL_DIR%\mca_pkg.sv %RTL_DIR%\mca_frontend_filter.sv %RTL_DIR%\mca_peak_interpolator.sv %RTL_DIR%\mca_baseline.sv %RTL_DIR%\mca_psd_charge.sv %RTL_DIR%\mca_acquisition.sv %RTL_DIR%\mca_waveform.sv %RTL_DIR%\mca_async_fifo.sv %RTL_DIR%\mca_status_snapshot.sv %RTL_DIR%\mca_control.sv %RTL_DIR%\mca_external_trigger.sv %RTL_DIR%\mca_net_tx.sv %RTL_DIR%\mca_spectrum_histogram.sv %RTL_DIR%\mca_uart_phy.sv %RTL_DIR%\mca_uart_protocol.sv %RTL_DIR%\mca_reset_sync.sv %RTL_DIR%\mca_phy_reset.sv %RTL_DIR%\mca_pulse_controller.sv %RTL_DIR%\mca_rgmii_ddr.sv %RTL_DIR%\mca_range_handshake.sv %RTL_DIR%\eth\crc32_d8.v %RTL_DIR%\eth\arp_rx.v %RTL_DIR%\eth\arp_tx.v %RTL_DIR%\eth\arp.v %RTL_DIR%\eth\udp_rx.v %RTL_DIR%\eth\udp_tx.v %RTL_DIR%\eth\udp.v %RTL_DIR%\eth\rgmii_rx.v %RTL_DIR%\eth\rgmii_tx.v %RTL_DIR%\eth\gmii_to_rgmii.v %RTL_DIR%\mca_eth_ctrl.v %RTL_DIR%\mca_eth_udp.v %RTL_DIR%\single_channel_mca.sv %SIM_DIR%mca_ip_stubs.sv %VIVADO_BIN%\..\data\verilog\src\glbl.v

if "%~1"=="" (
  set TESTS=tb_mca_throughput tb_mca_psd_acquisition tb_mca_psd_net_tx tb_mca_peak_interpolator tb_mca_acquisition tb_mca_pretrigger tb_mca_fifo tb_mca_baseline tb_mca_baseline_recovery tb_mca_frontend_filter tb_mca_frontend_filter_response tb_mca_waveform tb_mca_external_trigger tb_mca_status_snapshot tb_mca_net_tx tb_mca_net_udp_chain tb_mca_control tb_mca_uart_phy tb_mca_uart_protocol tb_mca_uart_ack_retry tb_mca_spectrum_histogram tb_mca_spectrum_params tb_mca_spectrum_final_flush tb_single_channel_mca
) else (
  set TESTS=%*
)

set FAILED=0
for %%T in (%TESTS%) do (
  echo === xsim: %%T ===
  call xvlog.bat -sv -i "%RTL_DIR%" -d MCA_NO_XPM %RTL_FILES% "%SIM_DIR%%%T.sv" > "%%T.xvlog.log" 2>&1
  set "STEP_RC=!errorlevel!"
  findstr /C:"ERROR:" "%%T.xvlog.log" >nul
  if not errorlevel 1 set STEP_RC=1
  if not "!STEP_RC!"=="0" (
    echo COMPILE FAILED: %%T
    type "%%T.xvlog.log"
    set FAILED=1
  ) else (
    call xelab.bat %%T glbl -s %%T_snap -L unisims_ver --timescale 1ns/1ps --debug typical > "%%T.xelab.log" 2>&1
    set "STEP_RC=!errorlevel!"
    findstr /C:"ERROR:" "%%T.xelab.log" >nul
    if not errorlevel 1 set STEP_RC=1
    if not "!STEP_RC!"=="0" (
      echo ELABORATION FAILED: %%T
      type "%%T.xelab.log"
      set FAILED=1
    ) else (
      call xsim.bat %%T_snap -runall --onerror quit > "%%T.xsim.log" 2>&1
      set "STEP_RC=!errorlevel!"
      findstr /C:"FAIL" /C:"Fatal:" /C:"ERROR:" "%%T.xsim.log" >nul
      if not errorlevel 1 set STEP_RC=1
      findstr /C:"PASS" "%%T.xsim.log" >nul
      if errorlevel 1 set STEP_RC=1
      if not "!STEP_RC!"=="0" (
        echo SIMULATION FAILED: %%T
        type "%%T.xsim.log"
        set FAILED=1
      ) else (
        findstr /C:"PASS" "%%T.xsim.log"
      )
    )
  )
)

popd
if not "%FAILED%"=="0" (
  echo run_xsim: failures detected
  exit /b 1
)
echo run_xsim: all testbenches passed
exit /b 0
