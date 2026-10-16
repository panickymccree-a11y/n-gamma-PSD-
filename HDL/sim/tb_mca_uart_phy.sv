`timescale 1ns/1ps

module tb_mca_uart_phy;
  localparam int CLOCK_HZ = 125_000_000;
  localparam int BAUD_HZ = 500_000;
  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;
  logic line;
  logic [7:0] tx_data, rx_data;
  logic tx_valid, tx_ready, rx_valid, rx_error;

  mca_uart_phy #(.CLOCK_HZ(CLOCK_HZ), .BAUD_HZ(BAUD_HZ)) dut (
    .clk(clk), .rst_n(rst_n), .rxd(line), .txd(line),
    .rx_data(rx_data), .rx_valid(rx_valid), .rx_frame_error(rx_error),
    .tx_data(tx_data), .tx_valid(tx_valid), .tx_ready(tx_ready)
  );

  task automatic send(input logic [7:0] value);
    begin
      wait (tx_ready);
      @(negedge clk); tx_data = value; tx_valid = 1;
      @(negedge clk); tx_valid = 0;
      wait (rx_valid);
      if (rx_data !== value) $fatal(1, "UART mismatch %02x != %02x", rx_data, value);
      if (rx_error) $fatal(1, "unexpected frame error");
    end
  endtask

  initial begin
    tx_data = 0; tx_valid = 0;
    repeat (4) @(negedge clk); rst_n = 1;
    send(8'h00); send(8'h55); send(8'haa); send(8'hff);
    $display("tb_mca_uart_phy PASS");
    $finish;
  end
endmodule
