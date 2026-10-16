## Place, route and report the synthesized design.
set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set reports_dir [file join $project_dir reports]
file mkdir $reports_dir

open_project [file join $project_dir single_channel_mca.xpr]
reset_run impl_1
launch_runs impl_1 -to_step route_design -jobs 12
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "route_design Complete!"} {
  error "Implementation failed: [get_property STATUS [get_runs impl_1]]"
}
open_run impl_1
report_utilization -file [file join $reports_dir utilization_impl.rpt]
report_timing_summary -delay_type min_max -report_unconstrained \
  -check_timing_verbose -max_paths 10 -input_pins -routable_nets \
  -file [file join $reports_dir timing_impl.rpt]
report_cdc -file [file join $reports_dir cdc_impl.rpt]
report_drc -file [file join $reports_dir drc_impl.rpt]
close_project
