`timescale 1ns/1ps

module hist_param_wrap #(parameter integer N = 1024) (
  input logic clk, rst_n, output logic ready,
  output logic [7:0] shift, output logic [15:0] total_bins
);
  logic toggle, ack;
  logic [7:0] rd_index;
  initial begin ack = 0; rd_index = 0; end
  mca_spectrum_histogram #(.BIN_COUNT(N), .CLOCK_HZ(125_000_000)) dut (
    .dco_clk(clk), .host_clk(clk), .rst_n(rst_n),
    .run_enable(1'b0), .clear(1'b0),
    .event_valid(1'b0), .event_peak('0), .event_integral('0),
    .config_version('0), .ready(ready), .chunk_toggle(toggle),
    .chunk_ack_toggle(ack), .session_id(), .epoch(),
    .chunk_config_version(), .chunk_kind(), .chunk_flags(),
    .bin_shift(shift), .total_bins(total_bins), .chunk_start(),
    .chunk_count(), .chunk_index(), .chunk_total(), .interval_ticks(),
    .interval_events(), .histogram_overflow(), .chunk_rd_index(rd_index),
    .chunk_rd_data()
  );
endmodule

module tb_mca_spectrum_params;
  logic clk = 0, rst_n = 0;
  always #4 clk = ~clk;
  logic r1, r2, r8;
  logic [7:0] s1, s2, s8;
  logic [15:0] b1, b2, b8;
  hist_param_wrap #(.N(1024)) p1(clk,rst_n,r1,s1,b1);
  hist_param_wrap #(.N(2048)) p2(clk,rst_n,r2,s2,b2);
  hist_param_wrap #(.N(8192)) p8(clk,rst_n,r8,s8,b8);
  initial begin
    repeat (4) @(negedge clk); rst_n = 1;
    wait (r1 && r2 && r8);
    if (b1 != 1024 || s1 != 4 || b2 != 2048 || s2 != 3 ||
        b8 != 8192 || s8 != 1) $fatal(1, "parameter mapping failed");
    $display("tb_mca_spectrum_params PASS");
    $finish;
  end
endmodule
