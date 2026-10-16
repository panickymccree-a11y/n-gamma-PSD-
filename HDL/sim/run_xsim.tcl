## Compile and run the single-channel MCA behavioural regressions with XSim.
##
## Usage:
##   vivado -mode batch -source sim/run_xsim.tcl
## or from PowerShell:
##   powershell -ExecutionPolicy Bypass -File sim/run_sim.ps1
##
## The script keeps every generated file under sim/build/xsim so the source
## tree stays clean.

set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set rtl_dir  [file join $project_dir rtl]
set sim_dir  [file join $project_dir sim]
set work_dir [file join $sim_dir build xsim]
file mkdir $work_dir
cd $work_dir

set rtl_files [list \
  [file join $rtl_dir mca_pkg.sv] \
  [file join $rtl_dir mca_frontend_filter.sv] \
  [file join $rtl_dir mca_peak_interpolator.sv] \
  [file join $rtl_dir mca_baseline.sv] \
  [file join $rtl_dir mca_psd_charge.sv] \
  [file join $rtl_dir mca_acquisition.sv] \
  [file join $rtl_dir mca_waveform.sv] \
  [file join $rtl_dir mca_async_fifo.sv] \
  [file join $rtl_dir mca_status_snapshot.sv] \
  [file join $rtl_dir mca_control.sv] \
  [file join $rtl_dir mca_external_trigger.sv] \
  [file join $rtl_dir mca_net_tx.sv] \
  [file join $rtl_dir mca_spectrum_histogram.sv] \
  [file join $rtl_dir mca_uart_phy.sv] \
  [file join $rtl_dir mca_uart_protocol.sv] \
  [file join $sim_dir mca_ip_stubs.sv] \
]

set testbenches [list \
  [list tb_mca_throughput [list [file join $rtl_dir eth udp_tx.v] [file join $sim_dir tb_mca_throughput.sv]]] \
  [list tb_mca_acquisition  [list [file join $sim_dir tb_mca_acquisition.sv]]] \
  [list tb_mca_pretrigger   [list [file join $sim_dir tb_mca_pretrigger.sv]]] \
  [list tb_mca_control      [list [file join $sim_dir tb_mca_control.sv]]] \
  [list tb_mca_net_tx       [list [file join $sim_dir tb_mca_net_tx.sv]]] \
  [list tb_mca_net_udp_chain [list [file join $rtl_dir eth udp_tx.v] [file join $sim_dir tb_mca_net_udp_chain.sv]]] \
  [list tb_mca_fifo         [list [file join $sim_dir tb_mca_fifo.sv]]] \
  [list tb_mca_baseline     [list [file join $sim_dir tb_mca_baseline.sv]]] \
  [list tb_mca_baseline_recovery [list [file join $sim_dir tb_mca_baseline_recovery.sv]]] \
  [list tb_mca_frontend_filter [list [file join $sim_dir tb_mca_frontend_filter.sv]]] \
  [list tb_mca_peak_interpolator [list [file join $sim_dir tb_mca_peak_interpolator.sv]]] \
  [list tb_mca_frontend_filter_response [list [file join $sim_dir tb_mca_frontend_filter_response.sv]]] \
  [list tb_mca_status_snapshot [list [file join $sim_dir tb_mca_status_snapshot.sv]]] \
  [list tb_mca_waveform     [list [file join $sim_dir tb_mca_waveform.sv]]] \
  [list tb_mca_external_trigger [list [file join $sim_dir tb_mca_external_trigger.sv]]] \
  [list tb_mca_uart_phy [list [file join $sim_dir tb_mca_uart_phy.sv]]] \
  [list tb_mca_uart_protocol [list [file join $sim_dir tb_mca_uart_protocol.sv]]] \
  [list tb_mca_uart_ack_retry [list [file join $sim_dir tb_mca_uart_ack_retry.sv]]] \
  [list tb_mca_spectrum_histogram [list [file join $sim_dir tb_mca_spectrum_histogram.sv]]] \
  [list tb_mca_spectrum_params [list [file join $sim_dir tb_mca_spectrum_params.sv]]] \
  [list tb_mca_spectrum_final_flush [list [file join $sim_dir tb_mca_spectrum_final_flush.sv]]] \
]

set failed 0
foreach tb $testbenches {
  set name [lindex $tb 0]
  set extra [lindex $tb 1]
  puts "=== xsim: $name ==="
  set rc [catch {
    set log [exec xvlog -sv -i $rtl_dir -d MCA_NO_XPM {*}$rtl_files {*}$extra]
    puts $log
    set log2 [exec xelab $name -s ${name}_snap -debug typical --timescale 1ns/1ps]
    puts $log2
    set log3 [exec xsim ${name}_snap -runall]
    puts $log3
    if {[string first "PASS" $log3] < 0 || [string first "FAIL" $log3] >= 0} {
      error "$name did not report PASS"
    }
  } msg]
  if {$rc} {
    puts "SIMULATION FAILED: $name"
    puts $msg
    set failed [expr {$failed + 1}]
  } else {
    puts "SIMULATION PASSED: $name"
  }
}

if {$failed != 0} {
  puts "run_xsim: $failed testbench(es) failed"
  exit 1
}
puts "run_xsim: all testbenches passed"
