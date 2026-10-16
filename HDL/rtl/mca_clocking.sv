`timescale 1ns/1ps

// The 200 MHz clock is only used for the RGMII delay-control clock. No
// board-specific I/O delay is instantiated in this stage.
module mca_clocking (
  input  logic clk_125m,
  input  logic rst_n,
  output logic clk_200m,
  output logic locked
);
`ifdef SYNTHESIS
  wire clkfb;
  wire clk200_mmcm;
  MMCME2_BASE #(
    .BANDWIDTH("OPTIMIZED"),
    .CLKFBOUT_MULT_F(8.0),
    .CLKIN1_PERIOD(8.0),
    .CLKOUT0_DIVIDE_F(5.0),
    .DIVCLK_DIVIDE(1),
    .STARTUP_WAIT("FALSE")
  ) u_mmcm (
    .CLKIN1(clk_125m),
    .CLKFBIN(clkfb),
    .RST(~rst_n),
    .PWRDWN(1'b0),
    .CLKFBOUT(clkfb),
    .CLKOUT0(clk200_mmcm),
    .LOCKED(locked)
  );
  BUFG u_bufg_200 (.I(clk200_mmcm), .O(clk_200m));
`else
  // Behavioral clocks model the MMCM outputs closely enough for CDC tests.
  always_ff @(posedge clk_125m or negedge rst_n) begin
    if (!rst_n) begin
      locked   <= 1'b0;
    end else begin
      locked   <= 1'b1;
    end
  end
  initial clk_200m = 1'b0;
  always #2.5 begin
    if (rst_n)
      clk_200m = ~clk_200m;
    else
      clk_200m = 1'b0;
  end
`endif
endmodule
