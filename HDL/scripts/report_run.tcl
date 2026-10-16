## Report the synthesis run configuration of the existing project.
set script_dir  [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
open_project [file join $project_dir single_channel_mca.xpr]

puts "runs: [get_runs]"
set run synth_1
puts "status      : [get_property STATUS [get_runs $run]]"
puts "strategy    : [get_property STRATEGY [get_runs $run]]"
puts "part        : [get_property PART [get_runs $run]]"
puts "top         : [get_property TOP [get_filesets sources_1]]"
puts "constrs     : [get_property CONSTRSET [get_runs $run]]"
puts "steps       : [get_property STEPS.SYNTH_DESIGN.ARGS [get_runs $run]]"
close_project
puts "report_run: done"
