set ip_dir [file normalize [file dirname [info script]]]
file mkdir $ip_dir
create_project -in_memory -part xc7a35tftg256-2
create_ip -name div_gen -vendor xilinx.com -library ip -version 5.1 \
  -module_name mca_peak_divider -dir $ip_dir
set ip [get_ips mca_peak_divider]
set_property -dict [list \
  CONFIG.algorithm_type {Radix2} \
  CONFIG.dividend_and_quotient_width {28} \
  CONFIG.divisor_width {16} \
  CONFIG.remainder_type {Remainder} \
  CONFIG.operand_sign {Unsigned} \
  CONFIG.clocks_per_division {1} \
  CONFIG.FlowControl {NonBlocking} \
  CONFIG.OptimizeGoal {Resources} \
  CONFIG.latency_configuration {Automatic} \
  CONFIG.dividend_has_tuser {true} \
  CONFIG.dividend_tuser_width {128} \
  CONFIG.divisor_has_tuser {false} \
  CONFIG.divide_by_zero_detect {false} \
  CONFIG.OutTready {false} \
  CONFIG.ACLKEN {false} \
  CONFIG.ARESETN {false}] $ip
# Vivado 2020.2 reports OptimizeGoal as a disabled parameter for this
# Radix2/one-clock-per-division configuration and retains Performance in the
# XCI despite the Resources request.  Keep the request here so newer Vivado
# releases can honor it without changing the functional interface.
generate_target all $ip
close_project
