set root [file normalize [file join [file dirname [info script]] ..]]
set src  [file join $root rtl mca_spectrum_histogram.sv]
set out  [file join $root reports histogram_ooc]

file mkdir $out
read_verilog -sv $src
synth_design -top mca_spectrum_histogram -part xc7a50tftg256-2 -mode out_of_context
report_utilization -file [file join $out utilization.rpt]
puts "CHECK_HISTOGRAM_OOC_DONE"
