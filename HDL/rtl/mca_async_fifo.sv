`timescale 1ns/1ps

// XPM FIFO wrapper used for all multi-bit CDC paths. For standalone RTL
// simulation define MCA_NO_XPM to use the equivalent Gray-pointer FIFO.
module mca_async_fifo #(
  parameter int WIDTH = 8,
  parameter int ADDR_WIDTH = 4,
  parameter bit USE_XPM = 1'b1
) (
  input  logic             rst_n,
  input  logic             wr_clk,
  input  logic             wr_en,
  input  logic [WIDTH-1:0] din,
  output logic             full,
  output logic [ADDR_WIDTH:0] wr_data_count,
  input  logic             rd_clk,
  input  logic             rd_en,
  output logic [WIDTH-1:0] dout,
  output logic             empty,
  output logic [ADDR_WIDTH:0] rd_data_count,
  output logic             wr_rst_busy,
  output logic             rd_rst_busy
);

`ifdef MCA_NO_XPM
  localparam bit USE_XPM_INT = 1'b0;
`else
  localparam bit USE_XPM_INT = USE_XPM;
`endif

  generate
    if (USE_XPM_INT) begin : gen_xpm
      xpm_fifo_async #(
        .CDC_SYNC_STAGES(2),
        .DOUT_RESET_VALUE("0"),
        .ECC_MODE("no_ecc"),
        .FIFO_MEMORY_TYPE("auto"),
        .FIFO_READ_LATENCY(1),
        .FIFO_WRITE_DEPTH(1 << ADDR_WIDTH),
        .FULL_RESET_VALUE(1),
        // Vivado 2020.2 validates programmable thresholds even when their
        // output ports are unused.  A five-word guard band is legal for all
        // supported FIFO depths (minimum depth is 16).
        .PROG_EMPTY_THRESH(5),
        .PROG_FULL_THRESH((1 << ADDR_WIDTH) - 5),
        .RD_DATA_COUNT_WIDTH(ADDR_WIDTH + 1),
        .READ_DATA_WIDTH(WIDTH),
        .READ_MODE("std"),
        .RELATED_CLOCKS(0),
        .SIM_ASSERT_CHK(0),
        .USE_ADV_FEATURES("0707"),
        .WAKEUP_TIME(0),
        .WRITE_DATA_WIDTH(WIDTH),
        .WR_DATA_COUNT_WIDTH(ADDR_WIDTH + 1)
      ) u_xpm_fifo (
        .rst(~rst_n),
        .wr_clk(wr_clk),
        .wr_en(wr_en),
        .din(din),
        .full(full),
        .overflow(),
        .almost_full(),
        .wr_ack(),
        .wr_data_count(wr_data_count),
        .wr_rst_busy(wr_rst_busy),
        .rd_clk(rd_clk),
        .rd_en(rd_en),
        .dout(dout),
        .empty(empty),
        .underflow(),
        .almost_empty(),
        .data_valid(),
        .rd_data_count(rd_data_count),
        .rd_rst_busy(rd_rst_busy),
        .injectdbiterr(1'b0),
        .injectsbiterr(1'b0),
        .sleep(1'b0),
        .dbiterr(),
        .prog_empty(),
        .prog_full(),
        .sbiterr()
      );
    end else begin : gen_gray_fifo
      localparam int DEPTH = 1 << ADDR_WIDTH;
      logic [WIDTH-1:0] mem [0:DEPTH-1];
      logic [ADDR_WIDTH:0] wr_bin, wr_gray;
      logic [ADDR_WIDTH:0] rd_bin, rd_gray;
      logic [ADDR_WIDTH:0] rd_gray_wr1, rd_gray_wr2;
      logic [ADDR_WIDTH:0] wr_gray_rd1, wr_gray_rd2;
      logic [ADDR_WIDTH:0] wr_bin_inc, wr_bin_next, wr_gray_next;
      logic [ADDR_WIDTH:0] rd_bin_next, rd_gray_next;
      logic wr_reset_done, rd_reset_done;
      // Registered full flag.  It is computed from the pointer that will be
      // committed on this edge and the synchronised read pointer.  The next
      // pointer is gated by the previous registered full state, so there is
      // no full -> next-pointer -> full combinational loop.
      logic full_reg;

      function automatic logic [ADDR_WIDTH:0] bin2gray(input logic [ADDR_WIDTH:0] b);
        bin2gray = (b >> 1) ^ b;
      endfunction

      function automatic logic [ADDR_WIDTH:0] gray2bin(input logic [ADDR_WIDTH:0] g);
        logic [ADDR_WIDTH:0] b;
        int i;
        begin
          b[ADDR_WIDTH] = g[ADDR_WIDTH];
          for (i = ADDR_WIDTH-1; i >= 0; i = i - 1)
            b[i] = b[i+1] ^ g[i];
          gray2bin = b;
        end
      endfunction

      assign wr_bin_inc  = wr_bin + 1'b1;
      assign wr_bin_next = (wr_en && !full_reg) ? wr_bin_inc : wr_bin;
      assign wr_gray_next= bin2gray(wr_bin_next);
      assign rd_bin_next = rd_bin + ((rd_en && !empty) ? 1'b1 : 1'b0);
      assign rd_gray_next= bin2gray(rd_bin_next);

      assign full  = full_reg;
      assign empty = (rd_gray == wr_gray_rd2);
      assign wr_data_count = wr_bin - gray2bin(rd_gray_wr2);
      assign rd_data_count = gray2bin(wr_gray_rd2) - rd_bin;
      assign wr_rst_busy = !wr_reset_done;
      assign rd_rst_busy = !rd_reset_done;

      always_ff @(posedge wr_clk or negedge rst_n) begin
        if (!rst_n) begin
          wr_bin       <= '0;
          wr_gray      <= '0;
          rd_gray_wr1  <= '0;
          rd_gray_wr2  <= '0;
          full_reg     <= 1'b0;
          wr_reset_done <= 1'b0;
        end else begin
          rd_gray_wr1 <= rd_gray;
          rd_gray_wr2 <= rd_gray_wr1;
          wr_bin      <= wr_bin_next;
          wr_gray     <= wr_gray_next;
          wr_reset_done <= 1'b1;
          full_reg    <= (wr_gray_next ==
                          {~rd_gray_wr2[ADDR_WIDTH:ADDR_WIDTH-1],
                           rd_gray_wr2[ADDR_WIDTH-2:0]});
          if (wr_en && !full_reg)
            mem[wr_bin[ADDR_WIDTH-1:0]] <= din;
        end
      end

      always_ff @(posedge rd_clk or negedge rst_n) begin
        if (!rst_n) begin
          rd_bin       <= '0;
          rd_gray      <= '0;
          wr_gray_rd1  <= '0;
          wr_gray_rd2  <= '0;
          dout         <= '0;
          rd_reset_done <= 1'b0;
        end else begin
          wr_gray_rd1 <= wr_gray;
          wr_gray_rd2 <= wr_gray_rd1;
          rd_bin      <= rd_bin_next;
          rd_gray     <= rd_gray_next;
          rd_reset_done <= 1'b1;
          if (rd_en && !empty)
            dout <= mem[rd_bin[ADDR_WIDTH-1:0]];
        end
      end
    end
  endgenerate
endmodule
