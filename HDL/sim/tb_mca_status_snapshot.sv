`timescale 1ns/1ps

module tb_mca_status_snapshot;
  localparam int N = 134;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic force_refresh = 1'b0;
  logic fifo_full = 1'b0;
  logic peak_interpolation_enabled = 1'b0;
  logic wr_en;
  logic [N*8-1:0] payload;
  int errors = 0;

  mca_status_snapshot #(.INTERVAL_CYCLES(20), .PAYLOAD_BYTES(N)) dut (
    .dco_clk(clk), .rst_n(rst_n), .force_refresh(force_refresh),
    .config_version(32'h1122_3344), .timestamp(48'h0102_0304_0506),
    .event_count(32'h1020_3040), .rejected_count(32'h5060_7080),
    .dropped_count(32'h90A0_B0C0), .baseline(14'h2ABC),
    .baseline_state(2'd2), .baseline_ready(1'b1), .baseline_auto(1'b0),
    .manual_baseline(14'h1234), .run_enable(1'b1), .mode(3'd3),
    .event_active(1'b1),
    .event_fifo_level(8'h5A), .adc_aligned(1'b1),
    .range_actual(2'd2), .range_busy(1'b0), .filter_enabled(1'b1),
    .frontend_init_done(1'b1), .recovery_tolerance(14'd32), .wr_en(wr_en),
    .peak_interpolation_enabled(peak_interpolation_enabled),
    .payload(payload), .fifo_full(fifo_full)
  );

  function automatic logic [7:0] b(input int index);
    b = payload[index*8 +: 8];
  endfunction

  task automatic check(input bit condition, input string message);
    if (!condition) begin
      errors++;
      $display("FAIL [%0t] %s", $time, message);
    end
  endtask

  initial begin
    int offset;
    int seen;
    int expected_type [0:9];
    int expected_len  [0:9];
    expected_type[0]=1;  expected_len[0]=8;
    expected_type[1]=2;  expected_len[1]=20;
    expected_type[2]=3;  expected_len[2]=28;
    expected_type[3]=4;  expected_len[3]=20;
    expected_type[4]=5;  expected_len[4]=12;
    expected_type[5]=9;  expected_len[5]=6;
    expected_type[6]=10; expected_len[6]=3;
    expected_type[7]=11; expected_len[7]=9;
    expected_type[8]=12; expected_len[8]=3;
    expected_type[9]=13; expected_len[9]=4;

    repeat (4) @(posedge clk);
    rst_n = 1'b1;
    #1;

    offset = 0;
    seen = 0;
    while ((offset < N) && (b(offset) != 0)) begin
       check(seen < 10, "too many TLVs");
       if (seen < 10) begin
        check(b(offset) == expected_type[seen],
              $sformatf("TLV %0d type %0d expected %0d", seen,
                        b(offset), expected_type[seen]));
        check(b(offset+1) == expected_len[seen],
              $sformatf("TLV %0d length %0d expected %0d", seen,
                        b(offset+1), expected_len[seen]));
      end
      offset += 2 + b(offset+1);
      seen++;
    end
    check(seen == 10, $sformatf("saw %0d TLVs expected 10", seen));
    check(offset == N-1, $sformatf("end marker offset %0d expected %0d", offset, N-1));
    check(b(N-1) == 0, "missing end marker");

    check({b(12),b(13),b(14),b(15)} == 32'h1122_3344,
          "config version byte order");
    check(b(7) == 8'd3 && b(8) == 8'd1,
          "waveform mode/running status fields");
    check({b(16),b(17),b(18),b(19),b(20),b(21)} == 48'h0102_0304_0506,
          "timestamp byte order");
    check({b(34),b(35),b(36),b(37)} == 32'h1020_3040,
          "event counter byte order");
    check(b(109) == 8'h12 && b(110) == 8'h34,
          "manual baseline value");
    check({b(113),b(114),b(115),b(116)} == 32'h5060_7080,
          "rejected counter byte order");
    check({b(117),b(118),b(119),b(120)} == 32'h90A0_B0C0,
          "dropped counter byte order");
    check(b(124) == 8'd0 && b(125) == 8'h2A && b(126) == 8'hBC,
           "14-bit baseline value");
    check(b(129) == 8'd2 && b(130) == 8'd6 &&
          b(131) == 8'd0 && b(132) == 8'd32,
          "front-end TLV value");

    // A forced report is delayed a few cycles to coalesce simultaneous
    // configuration changes, then produces exactly one FIFO write.
    seen = 0;
    @(negedge clk); force_refresh = 1'b1;
    @(negedge clk); force_refresh = 1'b0;
    repeat (8) begin
      @(posedge clk);
      #1;
      if (wr_en) seen++;
    end
    check(seen == 1, "forced refresh did not emit exactly one snapshot");

    // Periodic expiry while full must not write the FIFO.
    fifo_full = 1'b1;
    repeat (25) begin
      @(posedge clk);
      check(!wr_en, "snapshot wrote while FIFO full");
    end

    if (errors == 0)
      $display("tb_mca_status_snapshot PASS");
    else
      $display("tb_mca_status_snapshot FAIL: %0d errors", errors);
    $finish;
  end
endmodule
