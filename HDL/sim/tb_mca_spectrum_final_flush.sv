`timescale 1ns/1ps

// Spectrum-drain edge coverage for the serial transport:
//   A. STOP (run_enable falling edge) flushes the partial interval as the
//      "final" spectrum and then stops,
//   B. CLEAR discards a queued final report instead of transmitting it,
//   C. the one-second boundary is held while the previous drain is still
//      waiting for its ACK, and the held interval keeps counting events.
//
// CLOCK_HZ=64 keeps a simulated interval short: one_second is exactly 64
// dco_clk cycles, so the whole regression runs in a few microseconds.
module tb_mca_spectrum_final_flush;
  localparam int BINS   = 1024;
  localparam int CHUNKS = BINS / 256;   // 4
  localparam logic [13:0] PEAK_TWO   = 14'h00c0;  // peak[13:4] = 12
  localparam logic [23:0] INTEGRAL   = 24'h0100;  // integral[23:14] = 0x40

  logic clk = 0, rst_n = 0;
  always #4 clk = ~clk;

  logic        run_enable = 1'b0, clear = 1'b0;
  logic        event_valid = 1'b0;
  logic [13:0] event_peak = 14'd0;
  logic [23:0] event_integral = 24'd0;
  logic [7:0]  rd_index = 8'd0;
  logic        ack = 1'b0;
  logic        ready, toggle;
  logic [31:0] session_id, epoch, overflow, rd_data;
  logic        kind;
  logic [7:0]  flags, shift;
  logic [15:0] total_bins, start_bin, count, index, total;
  logic [47:0] ticks;
  logic [31:0] events;

  mca_spectrum_histogram #(
    .BIN_COUNT(BINS), .CHUNK_BINS(256), .CLOCK_HZ(64)
  ) dut (
    .dco_clk(clk), .host_clk(clk), .rst_n(rst_n),
    .run_enable(run_enable), .clear(clear),
    .event_valid(event_valid), .event_peak(event_peak),
    .event_integral(event_integral), .config_version(32'h0000_1234),
    .ready(ready), .chunk_toggle(toggle), .chunk_ack_toggle(ack),
    .session_id(session_id), .epoch(epoch), .chunk_config_version(),
    .chunk_kind(kind), .chunk_flags(flags), .bin_shift(shift),
    .total_bins(total_bins), .chunk_start(start_bin), .chunk_count(count),
    .chunk_index(index), .chunk_total(total), .interval_ticks(ticks),
    .interval_events(events), .histogram_overflow(overflow),
    .chunk_rd_index(rd_index), .chunk_rd_data(rd_data)
  );

  logic   seen_toggle;
  integer chunks_seen;
  integer spectra_seen;
  integer final_chunks;
  integer nonfinal_final_chunks;
  logic   epoch_final;

  // Wait for the next chunk descriptor, verify its grammar, acknowledge it.
  // The "final" flag marks every chunk of the last interval (both the peak and
  // the integral spectrum), so it must never appear on an earlier interval.
  task automatic collect_chunk;
    begin
      wait (toggle != seen_toggle);
      seen_toggle = toggle;
      #1;
      if (total != CHUNKS)
        $fatal(1, "chunk_total %0d != %0d", total, CHUNKS);
      if (count != 16'd256)
        $fatal(1, "chunk_count %0d != 256", count);
      if (start_bin != index * 16'd256)
        $fatal(1, "chunk_start %0d does not match index %0d", start_bin, index);
      if ((!kind && shift != 4) || (kind && shift != 8))
        $fatal(1, "bin shift %0d does not match kind %0d", shift, kind);
      if (flags[2]) begin
        // The first flagged chunk of the run reveals which interval is final.
        if (final_chunks == 0)
          epoch_final = 1'b1;
        final_chunks = final_chunks + 1;
        if (!epoch_final)
          nonfinal_final_chunks = nonfinal_final_chunks + 1;
      end
      chunks_seen = chunks_seen + 1;
      if (index == 16'd0)
        spectra_seen = spectra_seen + 1;
      @(negedge clk);
      ack = ~ack;
    end
  endtask

  // Drain exactly one epoch of both spectra (2 * CHUNKS chunks).
  task automatic drain_epoch(input string what);
    integer k;
    begin
      chunks_seen  = 0;
      spectra_seen = 0;
      for (k = 0; k < 2 * CHUNKS; k = k + 1)
        collect_chunk();
      if (spectra_seen != 2)
        $fatal(1, "%s: saw %0d spectra, expected 2", what, spectra_seen);
    end
  endtask

  task automatic wait_idle(input string what);
    integer timeout;
    begin
      timeout = 0;
      while (dut.state != dut.ST_IDLE && timeout < 400) begin
        @(negedge clk);
        timeout = timeout + 1;
      end
      if (dut.state != dut.ST_IDLE)
        $fatal(1, "%s: drain did not return to ST_IDLE", what);
    end
  endtask

  integer pending_events = 0;

  // Pulse one event into the accumulator.  A dedicated process drives
  // event_valid only while the engine is idle and holds it for a full clock,
  // so the sample-and-clear handshake cannot lose a pulse.
  task automatic inject_event;
    begin
      pending_events = pending_events + 1;
      wait (pending_events == 0);
    end
  endtask

  initial begin
    event_valid = 1'b0;
    forever begin
      @(negedge clk);
      // The histogram queues events while it is scanning a chunk or waiting
      // for its ACK.  Present queued events in every ready state (except the
      // reset/clear sweep); restricting this to ST_IDLE can leave the test
      // driver permanently blocked when a request coincides with a scan.
      if (pending_events > 0 && run_enable && dut.state != dut.ST_CLEAR) begin
        pending_events = pending_events - 1;
        event_peak     = PEAK_TWO;
        event_integral = INTEGRAL;
        event_valid    = 1'b1;
        // Hold across a full clock so the engine samples it on a clean
        // posedge; clearing it on the sampling edge itself races the engine's
        // sample-and-clear handshake and loses the event.
        @(negedge clk);
        event_valid = 1'b0;
      end
    end
  end

  integer i;
  integer k;
  integer guard;

  initial begin
    repeat (4) @(negedge clk);
    rst_n = 1;
    wait (ready);
    if (total_bins != BINS || shift != 4)
      $fatal(1, "1024-bin capability mismatch");
    seen_toggle = toggle;

    // ============ part A: STOP final flush ==========================
    run_enable = 1'b1;
    repeat (4) @(negedge clk);
    inject_event();
    // The accumulator accepts one event per clock only while it is idle, so
    // consecutive pulses must be spaced by at least the event-write latency.
    repeat (8) @(negedge clk);
    inject_event();
    repeat (8) @(negedge clk);
    if (dut.events_running != 32'd2) begin
      $display("  events_running=%0d ticks=%0d state=%0d ready=%0b run=%0b timer=%0d one_second=%0b closing=%0b rp=%0b clr=%0b",
               dut.events_running, dut.ticks_running, dut.state, dut.ready,
               dut.run_enable, dut.report_timer, dut.one_second,
               dut.closing_interval, dut.report_pending, dut.clear);
      $fatal(1, "the accumulator counted %0d of 2 injected events",
             dut.events_running);
    end
    run_enable = 1'b0;
    // The STOP handshake is registered, so wait for the pending report.
    wait (dut.final_pending);
    if (!dut.report_pending)
      $fatal(1, "STOP did not queue the final report");

    final_chunks = 0;
    nonfinal_final_chunks = 0;
    epoch_final = 1'b0;
    seen_toggle = toggle;
    drain_epoch("final flush");
    // Every chunk of the final interval carries the final flag.
    if (final_chunks != 2 * CHUNKS)
      $fatal(1, "final flag on %0d of %0d chunks", final_chunks, 2 * CHUNKS);
    if (nonfinal_final_chunks != 0)
      $fatal(1, "final flag leaked onto a non-final interval");
    if (events != 2)
      $fatal(1, "final interval event count %0d != 2", events);
    if (ticks == 0)
      $fatal(1, "final interval tick count is zero");
    if (epoch != 32'd1)
      $fatal(1, "final report epoch %0d != 1", epoch);
    wait_idle("final flush");
    if (dut.final_pending || dut.report_pending)
      $fatal(1, "final/report pending was not retired after the drain");
    if (overflow != 0)
      $fatal(1, "event skid overflowed");

    // ============ part B: CLEAR drops the queued report =============
    run_enable = 1'b1;
    repeat (4) @(negedge clk);
    inject_event();
    repeat (4) @(negedge clk);
    run_enable = 1'b0;
    wait (dut.report_pending);
    clear = 1'b1;
    // CLEAR must be observed before the queued report can start its scan.
    guard = 0;
    while (dut.state != dut.ST_CLEAR && guard < 64) begin
      @(negedge clk);
      guard = guard + 1;
    end
    if (dut.state != dut.ST_CLEAR)
      $fatal(1, "CLEAR was not accepted before the queued report started");
    clear = 1'b0;
    seen_toggle = toggle;
    // Long enough for a full 4096-bin scan to have been transmitted.
    repeat (4 * BINS + 128) @(negedge clk);
    if (toggle !== seen_toggle)
      $fatal(1, "CLEAR still transmitted the queued spectrum");
    if (dut.report_pending || dut.final_pending)
      $fatal(1, "CLEAR left report/final pending set");
    // CLEAR resets the running counters; the reported interval registers keep
    // the last transmitted value by design.
    if (dut.events_running != 0 || dut.ticks_running != 0)
      $fatal(1, "CLEAR did not reset the interval counters (events=%0d ticks=%0d)",
             dut.events_running, dut.ticks_running);
    if (epoch != 0)
      $fatal(1, "CLEAR did not reset the epoch");
    if (overflow != 0)
      $fatal(1, "CLEAR path produced an skid overflow");

    // ============ part C: boundary held while an ACK is owed ========
    wait (ready);
    repeat (4) @(negedge clk);
    run_enable = 1'b1;
    seen_toggle = toggle;
    inject_event();
    // The first chunk of the interval appears at the one-second boundary.
    wait (toggle != seen_toggle);
    seen_toggle = toggle;
    #1;
    if (index != 16'd0)
      $fatal(1, "first boundary chunk is not chunk 0");
    // Hold the ACK across more than one interval: no other chunk may be
    // emitted while this one is still unacknowledged.
    repeat (4 * 64) @(negedge clk);
    if (toggle !== seen_toggle)
      $fatal(1, "a chunk was emitted while the previous one was unacknowledged");
    @(negedge clk);
    ack = ~ack;   // release the held chunk
    // Drain the rest of the epoch and confirm the next report really happened.
    for (k = 1; k < 2 * CHUNKS; k = k + 1)
      collect_chunk();
    wait_idle("held boundary");
    // Holding the chunk across a boundary must not create a second report: the
    // next report is only produced after this epoch has been acknowledged.
    if (epoch != 32'd1)
      $fatal(1, "a boundary was reported while the epoch was unacknowledged (epoch=%0d)",
             epoch);
    if (events != 1)
      $fatal(1, "the interval counted %0d events, expected 1", events);
    if (ticks < 64)
      $fatal(1, "the held interval reported only %0d ticks", ticks);
    run_enable = 1'b0;
    wait_idle("held boundary stop");

    if (overflow != 0)
      $fatal(1, "final skid overflow");
    $display("tb_mca_spectrum_final_flush PASS");
    $finish;
  end
endmodule
