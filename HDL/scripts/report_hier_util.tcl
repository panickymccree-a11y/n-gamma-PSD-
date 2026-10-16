set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set reports_dir [file join $project_dir reports]
file mkdir $reports_dir

open_project [file join $project_dir single_channel_mca.xpr]
open_run synth_1
report_utilization -hierarchical -hierarchical_depth 5 \
  -file [file join $reports_dir utilization_hierarchical_synth.rpt]
close_design
close_project
