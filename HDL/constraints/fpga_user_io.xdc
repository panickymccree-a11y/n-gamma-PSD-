## User IO constraints
## Per project direction, the Excel package-device name is intentionally not
## used to select the Vivado part. These LOC/IOSTANDARD entries are the board
## constraint source for this project.


set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 50 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.CONFIG.UNUSEDPIN PULLUP [current_design]


## System and ADC (Bank 35 / Bank 15)
set_property PACKAGE_PIN C4 [get_ports clk_125m]
set_property IOSTANDARD LVCMOS33 [get_ports clk_125m]
set_property PACKAGE_PIN B11 [get_ports {adc_data[0]}]
set_property PACKAGE_PIN A12 [get_ports {adc_data[1]}]
set_property PACKAGE_PIN B12 [get_ports {adc_data[2]}]
set_property PACKAGE_PIN A13 [get_ports {adc_data[3]}]
set_property PACKAGE_PIN A14 [get_ports {adc_data[4]}]
set_property PACKAGE_PIN B15 [get_ports {adc_data[5]}]
set_property PACKAGE_PIN A15 [get_ports {adc_data[6]}]
set_property PACKAGE_PIN B16 [get_ports {adc_data[7]}]
set_property PACKAGE_PIN C14 [get_ports {adc_data[8]}]
set_property PACKAGE_PIN C16 [get_ports {adc_data[9]}]
set_property PACKAGE_PIN D15 [get_ports {adc_data[10]}]
set_property PACKAGE_PIN D16 [get_ports {adc_data[11]}]
set_property PACKAGE_PIN E15 [get_ports {adc_data[12]}]
set_property PACKAGE_PIN E16 [get_ports {adc_data[13]}]
set_property PACKAGE_PIN E13 [get_ports DCO125M]
set_property PACKAGE_PIN F15 [get_ports adc_or]
set_property PACKAGE_PIN G15 [get_ports adc_sclk]
set_property PACKAGE_PIN G16 [get_ports adc_sdio]
set_property PACKAGE_PIN H16 [get_ports adc_csb]
set_property IOSTANDARD LVCMOS18 [get_ports {{adc_data[*]} DCO125M adc_or adc_sclk adc_sdio adc_csb}]

## RGMII and PHY management (Bank 14 / LVCMOS33)
set_property PACKAGE_PIN N11 [get_ports rgmii_rxc]
set_property PACKAGE_PIN T12 [get_ports rgmii_rx_ctl]
set_property PACKAGE_PIN T13 [get_ports {rgmii_rxd[0]}]
set_property PACKAGE_PIN T14 [get_ports {rgmii_rxd[1]}]
set_property PACKAGE_PIN T15 [get_ports {rgmii_rxd[2]}]
set_property PACKAGE_PIN R16 [get_ports {rgmii_rxd[3]}]
set_property PACKAGE_PIN N12 [get_ports rgmii_txc]
set_property PACKAGE_PIN P14 [get_ports rgmii_tx_ctl]
set_property PACKAGE_PIN R15 [get_ports {rgmii_txd[0]}]
set_property PACKAGE_PIN P16 [get_ports {rgmii_txd[1]}]
set_property PACKAGE_PIN N16 [get_ports {rgmii_txd[2]}]
set_property PACKAGE_PIN M15 [get_ports {rgmii_txd[3]}]
set_property PACKAGE_PIN L14 [get_ports phy_mdc]
set_property PACKAGE_PIN M16 [get_ports phy_mdio]
set_property PACKAGE_PIN M14 [get_ports phy_rst_n]
set_property IOSTANDARD LVCMOS33 [get_ports {rgmii_rxc rgmii_rx_ctl {rgmii_rxd[*]} rgmii_txc rgmii_tx_ctl {rgmii_txd[*]} phy_mdc phy_mdio phy_rst_n}]

## External trigger and output pulses (Bank 14 / Bank 35)
set_property PACKAGE_PIN T7 [get_ports ex_tri]
set_property PACKAGE_PIN A5 [get_ports Q1]
set_property PACKAGE_PIN A4 [get_ports Q2]
set_property IOSTANDARD LVCMOS33 [get_ports ex_tri]
set_property IOSTANDARD LVCMOS33 [get_ports Q1]
set_property IOSTANDARD LVCMOS33 [get_ports Q2]

## Board CH340C UART (Bank 14 / 3.3 V)
set_property PACKAGE_PIN R5 [get_ports uart_rxd]
set_property PACKAGE_PIN T5 [get_ports uart_txd]
set_property IOSTANDARD LVCMOS33 [get_ports {uart_rxd uart_txd}]

## Timing-only constraints. No board-level input/output delay is asserted
## until the PHY/ADC trace timing and reset source are supplied.
create_clock -period 8.000 -name dco125m [get_ports DCO125M]
create_clock -period 8.000 -name rgmii_rxc [get_ports rgmii_rxc]
create_generated_clock -name rgmii_txc_clk -source [get_ports rgmii_rxc] -divide_by 1 [get_ports rgmii_txc]
set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks clk_125m] -group [get_clocks dco125m] -group [get_clocks rgmii_rxc]

## The first flop of this two-flop synchronizer intentionally samples a
## signal from the 125 MHz domain.  Do not time its metastability aperture as
## an ordinary 125-to-200 MHz data path; the second stage remains timed in the
## 200 MHz domain.
set_false_path -to [get_pins -hierarchical -regexp {.*phy_done_200_sync_reg\[0\]/D}]

## Reset is generated internally from the mca_clk_wiz locked output. VIO
## control/status signals are internal mca_vio probes. Neither appears as a
## package pin.
##SIGNAL END CLK SHOULD USE THE P PIN IN MMCM pins
#set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets {DCO125M_IBUF}]
#set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets {clk_125m_IBUF}]







