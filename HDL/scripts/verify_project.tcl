## Verification script for the rebuilt project.  Opens the xpr, reports the
## source sets and compile order, and parses the HDL sources to prove the
## source set loads.  Runs no design run (no synthesis/implementation).
set script_dir  [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set project_xpr [file join $project_dir single_channel_mca.xpr]

open_project $project_xpr

set srcset [get_filesets sources_1]
set simset [get_filesets sim_1]

puts "part        : [get_property part [current_project]]"
puts "top synth   : [get_property top $srcset]"
puts "top sim     : [get_property top $simset]"
puts "sources_1   : [llength [get_files -of_objects $srcset]] files"
puts "sim_1       : [llength [get_files -of_objects $simset]] files"
puts "constrs_1   : [llength [get_files -of_objects [get_filesets constrs_1]]] files"

# Only HDL files can be parsed; the xci sub-design also contributes templates,
# memory files and checkpoints that belong to the IP.
set hdl [get_files -quiet -of_objects $srcset -filter {FILE_TYPE == "SystemVerilog" || FILE_TYPE == "Verilog"}]
puts "HDL sources : [llength $hdl] files"

puts "--- key RTL presence ---"
foreach name {mca_pkg.sv mca_control.sv mca_acquisition.sv mca_spectrum_histogram.sv \
              mca_uart_phy.sv mca_uart_protocol.sv mca_status_snapshot.sv \
              single_channel_mca.sv mca_eth_ctrl.v} {
  set hits [get_files -quiet -of_objects $srcset */$name]
  puts [format "  %-28s %s" $name [expr {[llength $hits] ? "present" : "MISSING"}]]
}

puts "--- simulation testbenches ---"
foreach f [lsort [get_files -quiet -of_objects $simset -filter {FILE_TYPE == "SystemVerilog"}]] {
  puts "  [file tail $f]"
}

set bad 0
foreach f $hdl {
  if {[catch {read_verilog -sv $f} msg]} {
    puts "PARSE FAIL: $f -> $msg"
    incr bad
  }
}
puts "hdl parse failures: $bad"

close_project
puts "verify_project: done"
