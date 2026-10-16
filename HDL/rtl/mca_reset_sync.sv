`timescale 1ns/1ps

module mca_reset_sync (
  input  logic clk,
  input  logic arst_n,
  output logic srst_n
);
  logic [2:0] sync_ff;
  always_ff @(posedge clk or negedge arst_n) begin
    if (!arst_n) begin
      sync_ff <= 3'b000;
    end else begin
      sync_ff <= {sync_ff[1:0], 1'b1};
    end
  end
  assign srst_n = sync_ff[2];
endmodule
