## Vivado 2020.2 project reconstruction script.
##
## Builds single_channel_mca.xpr with a deliberate, self-maintaining source set:
##
##   * sources_1 holds mca_clk_wiz.xci, the two user XDC files, and only those
##     RTL files that actually take part in the design.  Dead/duplicate RTL is
##     filtered out automatically by reachability from the top module instead of
##     by a hardcoded file list.
##   * The clock IP is added through its .xci only.  The generated templates,
##     changelogs, VHDL stubs and simulation netlists that the IP flow parks in
##     ip/mca_clk_wiz are pruned so they can neither be parsed as HDL nor show
##     up as syntax-error files.
##   * sim_1 holds the simulation testbenches.
##
## This script only creates the project and its source sets.  It never launches
## synthesis, implementation or bitstream generation.

set script_dir  [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set project_name single_channel_mca
set part_name    xc7a50tftg256-2
set top_name     single_channel_mca
set sim_top      tb_single_channel_mca

set HDL_EXT {.sv .v .svh .vh}

# Language keywords that must never be mistaken for a module name.
set SV_KEYWORDS {
  module endmodule interface endinterface package endpackage program endprogram
  class endclass function endfunction task endtask generate endgenerate
  begin end fork join join_any join_none if else for while do repeat forever
  case casex casez endcase default assign always always_ff always_comb
  always_latch initial final import export typedef localparam parameter
  wire reg logic bit byte shortint int longint integer real time genvar
  input output inout ref const var static automatic signed unsigned
  virtual pure extern return break continue posedge negedge or and not
  specify endspecify primitive endprimitive table endtable defparam
  `timescale `include `define `ifdef `ifndef `else `elsif `endif `undef
  `default_nettype `celldefine `endcelldefine `resetall
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

proc has_hdl_ext {path} {
  global HDL_EXT
  return [expr {[lsearch -exact $HDL_EXT [string tolower [file extension $path]]] >= 0}]
}

# Collect HDL files under $dir, recursively.
proc collect_hdl {dir} {
  set out {}
  foreach f [lsort [glob -nocomplain -directory $dir *]] {
    if {[string equal -nocase [file type $f] "directory"]} {
      foreach g [collect_hdl $f] { lappend out $g }
    } elseif {[has_hdl_ext $f]} {
      lappend out $f
    }
  }
  return $out
}

# `module <name>` and `package <name>` declarations.
proc module_defs {path} {
  set fp [open $path r]
  set data [read $fp]
  close $fp
  set names {}
  foreach line [split $data "\n"] {
    if {[regexp {^\s*module\s+([A-Za-z_][A-Za-z0-9_$]*)} $line -> n]} {
      lappend names $n
    }
  }
  return $names
}

proc package_defs {path} {
  set fp [open $path r]
  set data [read $fp]
  close $fp
  set names {}
  foreach line [split $data "\n"] {
    if {[regexp {^\s*package\s+([A-Za-z_][A-Za-z0-9_$]*)} $line -> n]} {
      lappend names $n
    }
  }
  return $names
}

# Packages are used as "<name>::" rather than instantiated, and this covers
# both their constants and their functions.
proc package_refs {path} {
  set fp [open $path r]
  set data [read $fp]
  close $fp
  set refs {}
  foreach line [split $data "\n"] {
    foreach {- n} [regexp -all -inline {([A-Za-z_][A-Za-z0-9_$]*)::} $line] {
      lappend refs $n
    }
  }
  return $refs
}

# Instantiated module names.  Three explicit shapes are matched because Tcl's
# regexp alternation is not reliable for the parenthesised alternatives:
#   A. "<module> #("                    parameterised, one line
#   B. "<module> <instance> ("          plain, one line
#   C. "<module>" then "#(" on the next line
proc module_instances {path} {
  global SV_KEYWORDS
  set fp [open $path r]
  set data [read $fp]
  close $fp

  set out   {}
  set lines {}
  foreach line [split $data "\n"] {
    set code [string trim [lindex [split $line "//"] 0]]
    if {$code ne ""} { lappend lines $code }
  }

  for {set i 0} {$i < [llength $lines]} {incr i} {
    set code [lindex $lines $i]
    if {[regexp {^([A-Za-z_][A-Za-z0-9_$]*)[ \t]*\#[ \t]*\(} $code -> m]} {
      if {[lsearch -exact $SV_KEYWORDS $m] < 0} { lappend out $m }
      continue
    }
    if {[regexp {^([A-Za-z_][A-Za-z0-9_$]*)[ \t]+[A-Za-z_][A-Za-z0-9_$]*[ \t]*\(} \
             $code -> m]} {
      if {[lsearch -exact $SV_KEYWORDS $m] < 0} { lappend out $m }
      continue
    }
    if {[regexp {^([A-Za-z_][A-Za-z0-9_$]*) *$} $code -> m]} {
      set nxt [expr {$i + 1 < [llength $lines] ? [lindex $lines [expr {$i + 1}]] : ""}]
      if {[string match "#(*" $nxt] && [lsearch -exact $SV_KEYWORDS $m] < 0} {
        lappend out $m
      }
    }
  }
  return $out
}

# ---------------------------------------------------------------------------
# Work out which RTL files actually take part in the design
# ---------------------------------------------------------------------------
set rtl_dir        [file join $project_dir rtl]
set hdl_candidates [collect_hdl $rtl_dir]

set mod_file [dict create]
set pkg_file [dict create]
foreach f $hdl_candidates {
  foreach m [module_defs $f]  { dict set mod_file $m $f }
  foreach p [package_defs $f] { dict set pkg_file $p $f }
}

set reached [dict create]
set queue   [list $top_name]
if {![dict exists $mod_file $top_name]} {
  error "top module '$top_name' not found under $rtl_dir"
}
while {[llength $queue] > 0} {
  set m [lindex $queue 0]
  set queue [lrange $queue 1 end]
  if {[dict exists $reached $m]} { continue }
  dict set reached $m 1
  if {![dict exists $mod_file $m]} { continue }
  set src [dict get $mod_file $m]
  foreach inst [module_instances $src] {
    if {[dict exists $mod_file $inst] && ![dict exists $reached $inst]} {
      lappend queue $inst
    }
  }
  foreach ref [package_refs $src] {
    if {[dict exists $pkg_file $ref] && ![dict exists $reached $ref]} {
      lappend queue $ref
    }
  }
}

set rtl_used    {}
set rtl_dropped {}
foreach f $hdl_candidates {
  set used 0
  foreach m [module_defs $f]  { if {[dict exists $reached $m]} { set used 1 } }
  foreach p [package_defs $f] { if {[dict exists $reached $p]} { set used 1 } }
  if {$used} { lappend rtl_used $f } else { lappend rtl_dropped $f }
}

# ---------------------------------------------------------------------------
# Create the project
# ---------------------------------------------------------------------------
catch {close_project}
create_project -force $project_name $project_dir -part $part_name
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]

set srcset [get_filesets sources_1]

foreach f $rtl_used {
  add_files -norecurse -fileset $srcset $f
  set added [get_files -quiet -of_objects $srcset $f]
  if {[string equal -nocase [file extension $f] ".sv"]} {
    set_property file_type {SystemVerilog} $added
  } else {
    set_property file_type {Verilog} $added
  }
}

# Clock IP: add the .xci only and let the IP flow own its generated outputs.
set clk_xci [file join $project_dir ip mca_clk_wiz mca_clk_wiz.xci]
if {[file exists $clk_xci]} {
  add_files -norecurse -fileset $srcset $clk_xci
} else {
  create_ip -name clk_wiz -vendor xilinx.com -library ip -version 6.0 \
    -module_name mca_clk_wiz -dir [file join $project_dir ip]
  set_property -dict [list \
    CONFIG.PRIMITIVE {MMCM} \
    CONFIG.PRIM_SOURCE {Single_ended_clock_capable_pin} \
    CONFIG.PRIM_IN_FREQ {125.000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {125.000} \
    CONFIG.CLKOUT2_USED {true} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {200.000} \
    CONFIG.USE_RESET {false}] [get_ips mca_clk_wiz]
  generate_target all [get_ips mca_clk_wiz]
}

# The IP flow records its generated templates, changelog and VHDL/simulation
# stubs inside the xci sub-design.  Vivado refuses to remove them from the
# fileset ("must be removed via the sub-design parent"), so they are disabled
# instead: that keeps them out of synthesis and out of the error views.  The
# IP itself (.xci/.dcp/.v/.xdc) stays fully enabled.
set ip_disable {
  clk_wiz_v6_0_changelog.txt
  mca_clk_wiz.veo
  mca_clk_wiz_stub.vhdl
  mca_clk_wiz_sim_netlist.vhdl
  mca_clk_wiz_stub.v
  mca_clk_wiz_sim_netlist.v
}
foreach name $ip_disable {
  set victim [get_files -quiet -of_objects $srcset */$name]
  if {[llength $victim]} {
    if {[catch {set_property IS_ENABLED 0 $victim} msg]} {
      puts "note: could not disable $name ($msg)"
    } else {
      puts "disabled in sources_1: $name"
    }
  }
}

# The ordinary MCA mode still uses its existing optional peak divider.
set divider_xci [file join $project_dir ip mca_peak_divider mca_peak_divider mca_peak_divider.xci]
if {[file exists $divider_xci]} {add_files -norecurse -fileset $srcset $divider_xci}

# Board constraints.
set constrs [get_filesets constrs_1]
add_files -norecurse -fileset $constrs [file join $project_dir constraints fpga_user_io.xdc]

set impl_xdc [file join $project_dir constraints impl_only.xdc]
if {[file exists $impl_xdc]} {
  add_files -norecurse -fileset $constrs $impl_xdc
  set_property used_in_synthesis false        [get_files -of_objects $constrs $impl_xdc]
  set_property used_in_implementation true    [get_files -of_objects $constrs $impl_xdc]
}

# ---------------------------------------------------------------------------
# Simulation source set.  Only testbenches go in; sim/mca_ip_stubs.sv is the
# behavioural stand-in used by the standalone XSim regression and must not be
# added here, or it would define mca_clk_wiz a second time next to the IP.
# ---------------------------------------------------------------------------
set simset [get_filesets sim_1]
foreach f [lsort [glob -nocomplain -directory [file join $project_dir sim] tb_*.sv]] {
  add_files -norecurse -fileset $simset $f
  set added [get_files -quiet -of_objects $simset $f]
  set_property file_type {SystemVerilog} $added
}
set_property top $sim_top $simset
set_property top_file [get_files -quiet -of_objects $simset \
    [file join $project_dir sim ${sim_top}.sv]] $simset

# ---------------------------------------------------------------------------
# Finalise
# ---------------------------------------------------------------------------
set_property top $top_name $srcset
update_compile_order -fileset $srcset
update_compile_order -fileset $simset
set_property simulator_language Mixed [current_project]
set_property default_lib xil_defaultlib [current_project]

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
puts "============================================================"
puts "project        : [file join $project_dir ${project_name}.xpr]"
puts "part           : [get_property part [current_project]]"
puts "top (synth)    : [get_property top $srcset]"
puts "top (sim)      : [get_property top $simset]"
puts "sources_1      : [llength [get_files -of_objects $srcset]] files"
puts "sim_1          : [llength [get_files -of_objects $simset]] files"
puts "constrs_1      : [llength [get_files -of_objects $constrs]] files"
puts "clock IP       : [file tail $clk_xci]"
puts "------------------------------------------------------------"
puts "RTL files kept : [llength $rtl_used]"
foreach f $rtl_used { puts "   + [file tail $f]" }
puts "RTL files dropped (not reachable from $top_name): [llength $rtl_dropped]"
foreach f $rtl_dropped { puts "   - [file tail $f]" }
puts "============================================================"
puts "create_project: source sets built; no synthesis or implementation run"
