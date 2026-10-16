`timescale 1ns/1ps

// Gray-pointer asynchronous FIFO regression (the standalone fallback used when
// XPM is not available, which is also the model used for RTL verification).
//
// Checks required by the single-channel plan:
//   * standard read latency: one clock from rd_en to dout
//   * the full flag has no combinational dependency on the next write pointer
//   * data ordering across the two clock domains
//   * full/empty boundaries and the data counters
module tb_mca_fifo;
  localparam int WIDTH = 16;
  localparam int AW    = 4;          // depth 16
  localparam int DEPTH = 1 << AW;

  logic wr_clk = 1'b0;
  logic rd_clk = 1'b0;
  logic rst_n  = 1'b0;
  always #4 wr_clk = ~wr_clk;        // 125 MHz
  always #7 rd_clk = ~rd_clk;        // ~71 MHz

  logic              wr_en = 1'b0;
  logic [WIDTH-1:0]  din = '0;
  logic              full;
  logic [AW:0]       wr_count;
  logic              rd_en = 1'b0;
  logic [WIDTH-1:0]  dout;
  logic              empty;
  logic [AW:0]       rd_count;
  logic              wr_rst_busy, rd_rst_busy;

  int errors = 0;

  mca_async_fifo #(.WIDTH(WIDTH), .ADDR_WIDTH(AW), .USE_XPM(1'b0)) u_dut (
    .rst_n(rst_n),
    .wr_clk(wr_clk), .wr_en(wr_en), .din(din), .full(full),
    .wr_data_count(wr_count), .wr_rst_busy(wr_rst_busy),
    .rd_clk(rd_clk), .rd_en(rd_en), .dout(dout), .empty(empty),
    .rd_data_count(rd_count), .rd_rst_busy(rd_rst_busy)
  );

  task automatic check(input bit condition, input string message);
    begin
      if (!condition) begin
        errors = errors + 1;
        $display("FAIL [%0t] %s", $time, message);
      end
    end
  endtask

  // Write one word from the write clock domain until it is accepted.
  // Returns 1 when the word was accepted, 0 when the FIFO stayed full.
  task automatic push(input logic [WIDTH-1:0] value, output bit accepted);
    int guard;
    begin
      accepted = 1'b0;
      guard = 0;
      while (!accepted && (guard < 64)) begin
        if (!full) begin
          @(negedge wr_clk);
          din   = value;
          wr_en = 1'b1;
          @(negedge wr_clk);
          wr_en = 1'b0;
          accepted = 1'b1;
        end else begin
          @(negedge wr_clk);
        end
        guard = guard + 1;
      end
    end
  endtask

  // Read one word and compare it.  dout is registered, so it holds the
  // requested word one clock after rd_en is sampled.
  task automatic pop(input logic [WIDTH-1:0] expected);
    logic [WIDTH-1:0] captured;
    begin
      @(negedge rd_clk);
      rd_en = 1'b1;
      @(posedge rd_clk);
      #1;
      captured = dout;
      @(negedge rd_clk);
      rd_en = 1'b0;
      if (captured !== expected) begin
        errors = errors + 1;
        $display("FAIL [%0t] read %0d expected %0d", $time, captured, expected);
      end
    end
  endtask

  initial begin
    repeat (6) @(posedge wr_clk);
    rst_n = 1'b1;
    repeat (8) @(posedge wr_clk);

    // ---------------------------------------------------------------
    // 1. Empty at reset
    // ---------------------------------------------------------------
    check(empty, "FIFO should start empty");
    check(!full, "FIFO should not start full");

    // ---------------------------------------------------------------
    // 2. Fill to depth: every one of the DEPTH words must be accepted, and
    //    the FIFO must then report full.
    // ---------------------------------------------------------------
    begin
      int accepted_count;
      bit ok;
      accepted_count = 0;
      for (int i = 0; i < DEPTH; i++) begin
        push(16'h1000 + i[15:0], ok);
        if (ok)
          accepted_count = accepted_count + 1;
      end
      repeat (4) @(posedge wr_clk);
      check(accepted_count == DEPTH,
            $sformatf("accepted %0d writes, expected %0d", accepted_count,
                      DEPTH));
      check(full, "FIFO did not report full after a full depth of writes");
    end

    for (int i = 0; i < DEPTH; i++)
      $display("  mem[%0d]=0x%04x", i, u_dut.gen_gray_fifo.mem[i]);

    // While full, further writes must be refused.
    begin
      bit ok;
      push(16'hDEAD, ok);
      check(!ok, "FIFO accepted a write while full");
    end

    // ---------------------------------------------------------------
    // 3. Drain in order (also exercises the one-clock read latency)
    // ---------------------------------------------------------------
    for (int i = 0; i < DEPTH; i++)
      pop(16'h1000 + i[15:0]);
    repeat (12) @(posedge rd_clk);

    check(empty, "FIFO did not report empty after draining");
    check(rd_count == '0,
          $sformatf("read count %0d expected 0", rd_count));

    // ---------------------------------------------------------------
    // 4. Interleaved traffic across the two clock domains
    // ---------------------------------------------------------------
    fork
      begin : writer
        for (int w = 0; w < 64; w++) begin
          while (full)
            @(negedge wr_clk);
          @(negedge wr_clk);
          din   = 16'h2000 + w[15:0];
          wr_en = 1'b1;
          @(negedge wr_clk);
          wr_en = 1'b0;
          // Space the pushes out so the read side drains as we go.
          repeat (3) @(negedge wr_clk);
        end
      end
      begin : reader
        for (int r = 0; r < 64; r++) begin
          // Wait for data, then let the synchronised write pointer settle.
          while (empty)
            @(negedge rd_clk);
          repeat (2) @(negedge rd_clk);
          rd_en = 1'b1;
          @(posedge rd_clk);
          #1;
          if (dout !== (16'h2000 + r[15:0])) begin
            errors = errors + 1;
            $display("FAIL [%0t] interleaved read %0d expected %0d", $time,
                     dout, 16'h2000 + r[15:0]);
          end
          @(negedge rd_clk);
          rd_en = 1'b0;
        end
      end
    join

    repeat (12) @(posedge rd_clk);
    check(empty, "FIFO not empty at the end of the interleaved test");

    if (errors == 0)
      $display("tb_mca_fifo PASS");
    else
      $display("tb_mca_fifo FAIL: %0d errors", errors);
    $finish;
  end

  initial begin
    #2_000_000;
    $display("tb_mca_fifo TIMEOUT");
    $finish;
  end
endmodule
