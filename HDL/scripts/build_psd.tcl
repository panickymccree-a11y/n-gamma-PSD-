set project_dir [file normalize [file join [file dirname [info script]] ..]]
set reports [file join $project_dir reports psd]
file mkdir $reports
open_project [file join $project_dir single_channel_mca.xpr]
if {[file normalize [get_property DIRECTORY [current_project]]] ne $project_dir} {
  error "Project directory mismatch; refuse to build another checkout"
}
set_property part xc7a50tftg256-2 [current_project]
add_files -norecurse [file join $project_dir rtl mca_psd_charge.sv]
# Always regenerate products for the 50T device rather than using copied 35T IP caches.
foreach ip [get_ips] {
  if {[get_property IS_LOCKED $ip]} {upgrade_ip $ip}
  reset_target all $ip
  generate_target all $ip
}
foreach xci [get_files -all *.xci] {create_ip_run $xci}
set ip_runs [get_runs -filter {IS_SYNTHESIS == 1 && NAME != synth_1}]
foreach ip_run $ip_runs {reset_run $ip_run}
if {[llength $ip_runs]} {
  launch_runs $ip_runs -jobs 12
  foreach ip_run $ip_runs {wait_on_run $ip_run}
}
update_compile_order -fileset sources_1
reset_run synth_1
launch_runs synth_1 -jobs 12
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} {error "PSD synthesis failed"}
open_run synth_1
report_utilization -hierarchical -file [file join $reports utilization_synth.rpt]
report_timing_summary -delay_type min_max -file [file join $reports timing_synth.rpt]
report_cdc -details -file [file join $reports cdc_synth.rpt]
close_design
launch_runs impl_1 -to_step write_bitstream -jobs 12
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {error "PSD implementation failed"}
open_run impl_1
report_utilization -hierarchical -file [file join $reports utilization_impl.rpt]
report_timing_summary -delay_type min_max -max_paths 30 -report_unconstrained -file [file join $reports timing_impl.rpt]
report_cdc -details -file [file join $reports cdc_impl.rpt]
report_drc -file [file join $reports drc_impl.rpt]
report_timing -to [get_cells -hierarchical -filter {NAME =~ *u_psd_charge*}] -max_paths 20 -file [file join $reports psd_paths.rpt]
write_checkpoint -force [file join $reports psd_routed.dcp]
set worst_setup [get_timing_paths -delay_type max -max_paths 1]
set worst_hold [get_timing_paths -delay_type min -max_paths 1]
if {[llength $worst_setup] == 0 || [llength $worst_hold] == 0} {
  error "Missing timing paths; refuse successful delivery"
}
if {[get_property SLACK $worst_setup] < 0 || [get_property SLACK $worst_hold] < 0} {
  error "Setup/hold timing violation remains; reports saved, delivery rejected"
}
puts "PSD_BUILD_COMPLETE"
close_project
