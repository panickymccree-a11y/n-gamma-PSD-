## Rebuild single_channel_mca.xpr from the current RTL tree.
##
## Usage:
##   vivado -mode batch -source scripts/rebuild_project.tcl
##
## This is a thin wrapper around scripts/create_project.tcl, which builds the
## whole project: the clock IP from its .xci, the reachable RTL only, the board
## constraints and the simulation testbenches.  It never launches synthesis,
## implementation or bitstream generation.

set script_dir  [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]

catch {close_project}
source [file join $script_dir create_project.tcl]

close_project
puts "rebuild_project: [file join $project_dir single_channel_mca.xpr] rebuilt (no design runs)"
