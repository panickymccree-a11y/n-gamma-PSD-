## Add/recreate the internal 125/200 MHz Clocking Wizard in the delivered XPR.
set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set xpr_file [file join $project_dir single_channel_mca.xpr]
set clk_xci [file join $project_dir ip mca_clk_wiz mca_clk_wiz.xci]

open_project $xpr_file

if {[llength [get_ips -quiet mca_clk_wiz]] == 0} {
  if {[file exists $clk_xci]} {
    add_files -fileset sources_1 $clk_xci
  } else {
    create_ip -name clk_wiz -vendor xilinx.com -library ip -version 6.0 \
      -module_name mca_clk_wiz -dir [file join $project_dir ip]
  }
}

set_property -dict [list \
  CONFIG.PRIMITIVE {MMCM} \
  CONFIG.PRIM_SOURCE {Single_ended_clock_capable_pin} \
  CONFIG.PRIM_IN_FREQ {125.000} \
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {125.000} \
  CONFIG.CLKOUT2_USED {true} \
  CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {200.000} \
  CONFIG.USE_RESET {false}] [get_ips mca_clk_wiz]

generate_target all [get_ips mca_clk_wiz]
export_ip_user_files -of_objects [get_ips mca_clk_wiz] -no_script -sync -force -quiet
update_compile_order -fileset sources_1
close_project
puts "CLOCK_WIZARD_DELIVERY_COMPLETE: $clk_xci"
