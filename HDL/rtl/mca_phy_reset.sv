`timescale 1ns/1ps

module mca_phy_reset #(
  parameter int RESET_CYCLES = 1_250_000 // 10 ms at 125 MHz
) (
  input  logic clk,
  input  logic rst_n,
  output logic phy_rst_n,
  output logic done
);
  localparam int CW = (RESET_CYCLES < 2) ? 1 : $clog2(RESET_CYCLES + 1);
  logic [CW-1:0] count;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count     <= '0;
      phy_rst_n <= 1'b0;
      done      <= 1'b0;
    end else if (!done) begin
      phy_rst_n <= 1'b0;
      if (count == RESET_CYCLES-1) begin
        phy_rst_n <= 1'b1;
        done      <= 1'b1;
      end else begin
        count <= count + 1'b1;
      end
    end else begin
      phy_rst_n <= 1'b1;
    end
  end
endmodule
