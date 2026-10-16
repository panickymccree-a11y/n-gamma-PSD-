## Run synthesis for the existing single_channel_mca.xpr and write reports.
##
## Usage:
##   vivado -mode batch -source scripts/run_synth.tcl
##
## This keeps the project as create_project.tcl/rebuild_project.tcl built it and
## only drives the synth_1 run.  It does not create a bitstream and does not run
## implementation.
set script_dir  [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set project_xpr [file join $project_dir single_channel_mca.xpr]
set reports_dir [file join $project_dir reports]
file mkdir $reports_dir

if {![file exists $project_xpr]} {
  error "project not found: $project_xpr (run scripts/rebuild_project.tcl first)"
}

open_project $project_xpr

set run synth_1
puts "top        : [get_property top [get_filesets sources_1]]"
puts "part       : [get_property part [current_project]]"
puts "strategy   : [get_property STRATEGY [get_runs $run]]"
puts "run before : [get_property STATUS [get_runs $run]]"

reset_run $run
launch_runs $run -jobs 12
wait_on_run $run

set status [get_property STATUS [get_runs $run]]
puts "run after  : $status"
if {[string first "synth_design Complete!" $status] < 0} {
  error "Synthesis failed: $status"
}

open_run $run
report_utilization    -file [file join $reports_dir utilization_synth.rpt]
report_timing_summary -file [file join $reports_dir timing_synth.rpt]
report_cdc            -file [file join $reports_dir cdc_synth.rpt]
report_drc            -file [file join $reports_dir drc_synth.rpt]

puts "------------------------------------------------------------"
puts "utilization : [file join $reports_dir utilization_synth.rpt]"
puts "timing      : [file join $reports_dir timing_synth.rpt]"
puts "cdc         : [file join $reports_dir cdc_synth.rpt]"
puts "drc         : [file join $reports_dir drc_synth.rpt]"
puts "------------------------------------------------------------"

close_design
close_project
puts "run_synth: synthesis complete, reports written to $reports_dir"
