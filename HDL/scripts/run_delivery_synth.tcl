## Vivado 2020.2 non-project synthesis used when the GUI owns the .runs tree.
set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set reports_dir [file join $project_dir reports]
file mkdir $reports_dir

read_verilog -sv [glob -nocomplain -directory [file join $project_dir rtl] *.sv]
read_verilog [concat \
  [glob -nocomplain -directory [file join $project_dir rtl] *.v] \
  [glob -nocomplain -directory [file join $project_dir rtl eth] *.v]]
read_xdc [file join $project_dir constraints fpga_user_io.xdc]

synth_design -top single_channel_mca -part xc7a50tftg256-2
write_checkpoint -force [file join $reports_dir single_channel_mca_delivery_synth.dcp]
report_utilization -file [file join $reports_dir utilization_delivery_synth.rpt]
report_timing_summary -file [file join $reports_dir timing_delivery_synth.rpt]
report_cdc -details -file [file join $reports_dir cdc_delivery_synth.rpt]
report_drc -file [file join $reports_dir drc_delivery_synth.rpt]
puts "DELIVERY_SYNTH_RESULT complete"
