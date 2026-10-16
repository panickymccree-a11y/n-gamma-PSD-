## Rebuild, elaborate, simulate, synthesize, and report timing/CDC.
set script_dir [file dirname [file normalize [info script]]]
source [file join $script_dir create_project.tcl]

set project_dir [file normalize [file join $script_dir ..]]
set reports_dir [file join $project_dir reports]
file mkdir $reports_dir

launch_simulation -simset sim_1 -mode behavioral
run all
close_sim -quiet

reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
open_run synth_1
report_utilization -file [file join $reports_dir utilization.rpt]
report_timing_summary -file [file join $reports_dir timing_summary.rpt]
report_cdc -file [file join $reports_dir cdc.rpt]
close_design

puts "Vivado flow completed. Reports are in $reports_dir"
