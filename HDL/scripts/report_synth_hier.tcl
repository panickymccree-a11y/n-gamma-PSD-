## Report synthesis resource usage per hierarchy level, to locate the LUT
## over-utilisation.  Opens the completed synth_1 run; runs no further step.
set script_dir  [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set reports_dir [file join $project_dir reports]
file mkdir $reports_dir

open_project [file join $project_dir single_channel_mca.xpr]
set run synth_1
puts "status: [get_property STATUS [get_runs $run]]"
open_run $run

report_utilization -hierarchical -hierarchical_depth 2 \
  -file [file join $reports_dir utilization_hier.rpt]
report_utilization -hierarchical -hierarchical_depth 3 \
  -file [file join $reports_dir utilization_hier3.rpt]

close_design
close_project
puts "report_hier: done"
