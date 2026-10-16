`timescale 1ns/1ps

module tb_mca_spectrum_histogram;
  logic clk = 0, rst_n = 0, run_enable = 0, clear = 0;
  always #4 clk = ~clk;
  logic event_valid;
  logic [13:0] event_peak;
  logic [23:0] event_integral;
  logic ready, toggle, ack;
  logic [31:0] session_id, epoch, overflow, rd_data;
  logic kind;
  logic [7:0] flags, shift, rd_index;
  logic [15:0] total_bins_tb, start_bin, count, index, total;
  logic [47:0] ticks;
  logic [31:0] events;

  mca_spectrum_histogram #(
    .BIN_COUNT(4096), .CHUNK_BINS(256), .CLOCK_HZ(64)
  ) dut (
    .dco_clk(clk), .host_clk(clk), .rst_n(rst_n),
    .run_enable(run_enable), .clear(clear),
    .event_valid(event_valid), .event_peak(event_peak),
    .event_integral(event_integral), .config_version(32'h1234),
    .ready(ready), .chunk_toggle(toggle),
    .chunk_ack_toggle(ack), .session_id(session_id), .epoch(epoch),
    .chunk_config_version(), .chunk_kind(kind), .chunk_flags(flags), .bin_shift(shift),
    .total_bins(total_bins_tb), .chunk_start(start_bin), .chunk_count(count),
    .chunk_index(index), .chunk_total(total), .interval_ticks(ticks),
    .interval_events(events), .histogram_overflow(overflow),
    .chunk_rd_index(rd_index), .chunk_rd_data(rd_data)
  );

  integer chunks_seen;
  logic seen_toggle;
  initial begin
    // Deliberately use different peak and integral addresses.  This catches
    // accidental reuse of the peak address for the integral RAM.
    event_valid = 0; event_peak = 14'd40; event_integral = 24'h000500;
    ack = 0; rd_index = 0;
    repeat (4) @(negedge clk); rst_n = 1;
    wait (ready);
    if (total_bins_tb != 4096 || shift != 2) $fatal(1, "4096-bin capability mismatch");
    run_enable = 1;
    @(negedge clk); event_valid = 1;
    repeat (2) @(negedge clk);
    event_valid = 0;
    chunks_seen = 0; seen_toggle = toggle;
    while (chunks_seen < 32) begin
      wait (toggle != seen_toggle);
      seen_toggle = toggle;
      if (index == 0) begin
        // The chunk buffer is a registered block RAM read on host_clk, so the
        // word for rd_index is available one clock after the index is applied.
        rd_index = kind ? 5 : 10; @(posedge clk); #1;
        if (rd_data !== 32'd2)
          $fatal(1, "kind %0d event bin count %0d", kind, rd_data);
        if ((!kind && shift != 2) || (kind && shift != 8))
          $fatal(1, "bin shift mismatch");
        if (events != 2) $fatal(1, "interval event count mismatch");
      end
      @(negedge clk); ack = ~ack;
      chunks_seen = chunks_seen + 1;
    end
    wait (dut.state == dut.ST_IDLE);
    if (overflow != 0) $fatal(1, "event skid overflow");
    $display("tb_mca_spectrum_histogram PASS");
    $finish;
  end
endmodule
