`timescale 1ns/1ps

module tb_single_channel_mca;
  logic clk_125m = 1'b0, dco = 1'b0, rgmii_rxc = 1'b0;
  always #4.000 clk_125m = ~clk_125m;
  always #3.980 dco = ~dco;
  always #4.020 rgmii_rxc = ~rgmii_rxc;
  logic [13:0] adc_data = 14'd8192;
  logic [3:0] rgmii_rxd = 4'd0;
  logic rgmii_rx_ctl = 1'b0;
  logic rgmii_txc, rgmii_tx_ctl;
  logic [3:0] rgmii_txd;
  logic phy_rst_n, phy_mdc;
  wire phy_mdio;
  logic ex_tri = 1'b0;
  logic uart_rxd = 1'b1, uart_txd;
  logic q1, q2;
  logic adc_or = 1'b0;
  logic adc_sclk, adc_csb;
  wire adc_sdio;

  single_channel_mca #(
    .PHY_RESET_CYCLES(16), .PULSE_CYCLES(32), .STATUS_INTERVAL_CYCLES(64)
  ) u_dut (
    .clk_125m(clk_125m), .adc_data(adc_data), .DCO125M(dco),
    .adc_or(adc_or), .adc_sclk(adc_sclk), .adc_sdio(adc_sdio), .adc_csb(adc_csb),
    .rgmii_rxc(rgmii_rxc), .rgmii_rx_ctl(rgmii_rx_ctl), .rgmii_rxd(rgmii_rxd),
    .rgmii_txc(rgmii_txc), .rgmii_tx_ctl(rgmii_tx_ctl), .rgmii_txd(rgmii_txd),
    .phy_rst_n(phy_rst_n), .phy_mdc(phy_mdc), .phy_mdio(phy_mdio), .ex_tri(ex_tri),
    .uart_rxd(uart_rxd), .uart_txd(uart_txd),
    .Q1(q1), .Q2(q2)
  );

  initial begin
    wait (u_dut.rst_n);
    wait (u_dut.init_done);
    // init_done is produced by the pulse controller, while the sampled
    // range value is published by the neighboring control-domain register.
    // Allow that register's next clock edge before checking the published
    // value.
    @(posedge clk_125m);
    #1;
    if (q1 !== 1'b0 || q2 !== 1'b0)
      $fatal(1, "outputs did not return low after automatic range initialization");
    // The automatic power-up pulse is the Q2/5 V range, so the published
    // actual range after init_done is 2'b10.
    if (u_dut.range_applied_value_clk !== 2'b10)
      $fatal(1, "power-up actual range is not Q2/5 V");

    // The retained package pin is intentionally inert.
    ex_tri = 1'b1;
    repeat (8) @(posedge clk_125m);
    ex_tri = 1'b0;
    if (q1 !== 1'b0 || q2 !== 1'b0)
      $fatal(1, "ex_tri unexpectedly triggered a range pulse");

    // Exercise the same request/complete wires used by mca_control without a
    // VIO/debug path.  The actual range changes only after the pulse ends.
    // The power-up default is already Q2/5 V, so this request switches to
    // Q1/1 V to exercise a real published-range update.
    force u_dut.ctrl_range_value = 2'b01;
    force u_dut.ctrl_range_strobe = 1'b1;
    @(posedge clk_125m);
    release u_dut.ctrl_range_strobe;
    release u_dut.ctrl_range_value;
    wait (q1);
    if (u_dut.range_applied_value_clk !== 2'b10)
      $fatal(1, "actual range changed before the Q1 pulse completed");
    wait (!u_dut.range_busy);
    @(posedge clk_125m);
    #1;
    if (u_dut.range_applied_value_clk !== 2'b01 || q1 !== 1'b0 || q2 !== 1'b0)
      $fatal(1, "Q1 range pulse did not complete correctly");
    $display("tb_single_channel_mca PASS");
    $finish;
  end
endmodule
