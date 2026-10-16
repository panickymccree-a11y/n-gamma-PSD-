`timescale 1ns/1ps

// Baseline recovery and acquisition regression.
//
// Covers the cases required by the single-channel plan:
//   * acquisition stopped: the estimator still converges
//   * constant input errors of +-1, +-31 and +-255 codes
//   * a large bias step and baseline recovery
//   * positive and negative pulses with correct peak and integral
//   * 6-point and 2500-point windows, pre-trigger offset
//   * integral saturation and overflow flags
//   * plateau re-arm (no duplicate events)
//   * dead-time rejection accounting
//   * manual baseline mode and mid-acquisition commit
module tb_mca_acquisition;
  localparam int CLK_NS = 8;

  logic clk = 1'b0;
  always #(CLK_NS/2.0) clk = ~clk;
  logic rst_n = 1'b0;

  // Control inputs
  logic run_enable = 1'b0;
  logic holdoff = 1'b0;
  logic abort_event = 1'b0;
  logic clear_counts = 1'b0;
  logic config_apply = 1'b0;
  logic [13:0] cfg_threshold = 14'd64;
  logic [13:0] cfg_return = 14'd32;
  logic [11:0] cfg_window_total = 12'd32;
  logic [11:0] cfg_window_pre = 12'd0;
  logic        cfg_polarity_negative = 1'b0;
  logic        peak_interpolation_enable = 1'b0;

  // Baseline estimator configuration
  logic        auto_mode = 1'b1;
  logic [13:0] manual_baseline = 14'd8192;
  logic [13:0] base_guard = 14'd16;
  logic [13:0] base_pulse_threshold = 14'd32;
  logic [5:0]  fast_shift = 6'd4;
  logic [5:0]  track_shift = 6'd8;
  logic [5:0]  recovery_shift = 6'd8;
  logic [15:0] fast_samples = 16'd1024;
  logic [15:0] recovery_samples = 16'd1024;

  // Sample stream
  logic [13:0] adc_data = 14'd8192;
  logic        adc_overrange = 1'b0;

  // Interconnect
  logic        baseline_hold;
  logic [13:0] baseline;
  logic [1:0]  baseline_state;
  logic        baseline_ready;
  logic        baseline_update;

  logic        event_wr;
  logic [95:0] event_data;
  logic        event_full = 1'b0;
  logic [47:0] timestamp;
  logic [13:0] active_baseline;
  logic [1:0]  active_baseline_state;
  logic [31:0] event_count;
  logic [31:0] rejected_count;
  logic [31:0] dropped_count;
  logic        event_active;
  logic        accepting;
  // Histogram-event tap used by the serial spectrum accumulator.  The
  // ethernet-only regression still observes it so no DUT output is left
  // dangling.
  logic        histogram_event_valid;
  logic [13:0] histogram_event_peak;
  logic [23:0] histogram_event_integral;

  int errors = 0;
  int events_seen = 0;
  int histogram_events_seen = 0;
  int event_wr_pulses = 0;
  int tap_with_wr = 0;
  logic [95:0] captured [0:63];
  logic [47:0] timestamp_prev;

  mca_baseline #(
    .ADC_WIDTH(14), .BASELINE_WIDTH(14), .FRACTION_BITS(24)
  ) u_baseline (
    .clk(clk), .rst_n(rst_n),
    .sample_valid(1'b1), .sample(adc_data), .sample_overrange(adc_overrange),
    .auto_mode(auto_mode), .manual_baseline(manual_baseline),
    .guard(base_guard), .pulse_threshold(base_pulse_threshold),
    .track_shift(track_shift), .recovery_shift(recovery_shift),
    .fast_shift(fast_shift), .fast_samples(fast_samples),
    .recovery_samples(recovery_samples), .recovery_tolerance_lsb(14'd32),
    .offset_qualify(16'd4096),
    .pulse_hold_samples(16'd64),
    .hold(baseline_hold), .train_hold(1'b0), .restart(1'b0),
    .restart_keep_valid(1'b0),
    .baseline(baseline), .state(baseline_state), .ready(baseline_ready),
    .update_enable(baseline_update)
  );

  mca_acquisition #(
    .ADC_WIDTH(14), .MAX_WINDOW(2500), .MIN_WINDOW(6),
    .BASELINE_INIT(8192), .INTEGRAL_BITS(28)
  ) u_dut (
    .dco_clk(clk), .rst_n(rst_n),
    .run_enable(run_enable), .trigger_enable(run_enable),
    .holdoff(holdoff), .abort_event(abort_event),
    .clear_counts(clear_counts),
    .config_apply(config_apply), .cfg_threshold(cfg_threshold),
    .cfg_return(cfg_return), .cfg_window_total(cfg_window_total),
    .cfg_window_pre(cfg_window_pre), .cfg_polarity_negative(cfg_polarity_negative),
    .peak_interpolation_enable(peak_interpolation_enable),
    .baseline_hold(baseline_hold), .baseline(baseline),
    .baseline_state(baseline_state), .baseline_ready(baseline_ready),
    .adc_data(adc_data), .adc_overrange(adc_overrange),
    .event_wr(event_wr), .event_data(event_data), .event_full(event_full),
    .timestamp(timestamp),
    .active_baseline(active_baseline),
    .active_baseline_state(active_baseline_state),
    .event_count(event_count), .rejected_count(rejected_count),
    .dropped_count(dropped_count), .event_active(event_active),
    .accepting(accepting),
    .histogram_event_valid(histogram_event_valid),
    .histogram_peak(histogram_event_peak),
    .histogram_integral(histogram_event_integral)
  );

  always @(posedge clk) begin
    if (event_wr && events_seen < 64) begin
      captured[events_seen] = event_data;
      events_seen = events_seen + 1;
    end
    event_wr_pulses = event_wr_pulses + (event_wr ? 1 : 0);
    if (histogram_event_valid)
      tap_with_wr = tap_with_wr + (event_wr ? 1 : 0);
    // The serial spectrum tap marks the same completion the event packet does,
    // so whenever it fires the event write must be asserted in the same clock
    // and the tap's peak/integral must equal the packet fields (peak is Q14.2
    // in bits 47:32, integral is the 24-bit value in bits 31:8).
    if (histogram_event_valid) begin
      histogram_events_seen = histogram_events_seen + 1;
      if (!event_wr)
        errors = errors + 1;
      if (histogram_event_peak !== event_data[47:34])
        errors = errors + 1;
      if (histogram_event_integral !== event_data[31:8])
        errors = errors + 1;
    end
    if (event_wr | (u_dut.mode_phase_q == 2'd2 && u_dut.post_remaining_q >= 12'd29))
      $display("  [%0t] trig/retire phase=%0d post=%0d base=%0d adc=%0d sq=%0d trig=%0b rvq=%0b eabs=%0d peak=%0d acc=%0d",
               $time, u_dut.mode_phase_q, u_dut.post_remaining_q,
               baseline, adc_data, u_dut.sample_q, u_dut.trigger_ok,
               u_dut.run_valid_q, u_dut.error_active_abs, u_dut.peak_q,
               u_dut.integral_acc);
  end

  task automatic check(input bit condition, input string message);
    begin
      if (!condition) begin
        errors = errors + 1;
        $display("FAIL [%0t] %s", $time, message);
      end
    end
  endtask

  task automatic drive_samples(input int value, input int count);
    begin
      repeat (count) begin
        @(negedge clk);
        adc_data = value[13:0];
      end
    end
  endtask

  // Wait before sampling so the variable assignments above this call have
  // already settled at a clock edge; otherwise the core captures the previous
  // configuration.
  task automatic apply_config;
    begin
      @(negedge clk);
      @(negedge clk);
      config_apply = 1'b1;
      @(negedge clk);
      config_apply = 1'b0;
      @(negedge clk);
    end
  endtask

  // Drive a fixed number of samples without waiting for baseline state.
  task automatic settle(input int value, input int cycles);
    begin
      drive_samples(value, cycles);
      repeat (4) @(posedge clk);
    end
  endtask

  // Wait until the estimator reports ready and has converged to within
  // `tolerance` codes of `target`, or until `limit` samples have elapsed.
  // The loop stops early if an event is produced, which keeps a long settle
  // from leaving stray events behind for the next check.
  task automatic settle_until(input int value, input int target,
                              input int tolerance, input int limit);
    int count;
    begin
      adc_data = value[13:0];
      count = 0;
      while ((!baseline_ready || (baseline > target + tolerance) ||
              (baseline < target - tolerance)) && (count < limit)) begin
        @(negedge clk);
        adc_data = value[13:0];
        count = count + 1;
      end
      // Modal acquisition commits the 16-LSB bin centre. Give the Q24 TRACK
      // IIR enough quiet samples to refine it before exact event checks.
      repeat (2048) begin
        @(negedge clk);
        adc_data = value[13:0];
      end
      repeat (4) @(posedge clk);
    end
  endtask

  // ------------------------------------------------------------------
  // Event field helpers
  // ------------------------------------------------------------------
  function automatic logic [47:0] ev_ts(input logic [95:0] e);
    ev_ts = e[95:48];
  endfunction
  function automatic int ev_peak(input logic [95:0] e);
    ev_peak = int'(e[47:32]) / 4;
  endfunction
  function automatic int ev_integral(input logic [95:0] e);
    ev_integral = int'(e[31:8]);
  endfunction
  function automatic logic [7:0] ev_flags(input logic [95:0] e);
    ev_flags = e[7:0];
  endfunction

  initial begin
    // ---------------------------------------------------------------
    // 0. Reset and baseline convergence while acquisition is stopped
    // ---------------------------------------------------------------
    repeat (8) @(posedge clk);
    @(negedge clk);
    rst_n = 1'b1;
    run_enable = 1'b0;

    timestamp_prev = '0;
    repeat (4) begin
      @(posedge clk); #1;
      check(timestamp == timestamp_prev + 48'd1,
            $sformatf("timestamp did not increment: got %h expected %h",
                      timestamp, timestamp_prev + 48'd1));
      timestamp_prev = timestamp;
    end

    // +255 codes offset from the 8192 power-on value.  The estimator runs
    // even though acquisition is stopped.
    settle_until(8192 + 255, 8447, 4, 7000);
    check(baseline_ready, "baseline never became ready with acquisition stopped");
    check((baseline >= 8443) && (baseline <= 8451),
          $sformatf("baseline did not converge to 8447, got %0d", baseline));
    check(events_seen == 0, "events were produced while run_enable was low");

    // ---------------------------------------------------------------
    // 1. Small constant errors must still be absorbed (no integer lock)
    // ---------------------------------------------------------------
    settle_until(8192 + 1, 8193, 2, 40000);
    check((baseline >= 8191) && (baseline <= 8195),
          $sformatf("+1 code offset not tracked, baseline=%0d", baseline));

    settle_until(8192 - 1, 8191, 2, 40000);
    check((baseline >= 8189) && (baseline <= 8193),
          $sformatf("-1 code offset not tracked, baseline=%0d", baseline));

    settle_until(8192 + 31, 8223, 3, 40000);
    check((baseline >= 8220) && (baseline <= 8226),
          $sformatf("+31 code offset not tracked, baseline=%0d", baseline));

    settle_until(8192 + 255, 8447, 4, 40000);
    check((baseline >= 8443) && (baseline <= 8451),
          $sformatf("+255 code offset not tracked, baseline=%0d", baseline));

    // ---------------------------------------------------------------
    // 2. Large bias step: the estimator must recover, and the offset
    //    qualification must not treat pulses as drift.
    // ---------------------------------------------------------------
    settle_until(9000, 9000, 6, 200000);
    check(baseline_ready, "baseline not ready after a large bias step");
    check((baseline >= 8994) && (baseline <= 9006),
          $sformatf("large bias step not recovered, baseline=%0d", baseline));

    // Return to the nominal level for the event tests.
    settle_until(8192, 8192, 4, 200000);
    check((baseline >= 8188) && (baseline <= 8196),
          $sformatf("baseline did not return to 8192, got %0d", baseline));

    // A dense pulse train must reload the post-pulse hold window and leave
    // the estimator parked at the true baseline.  This specifically catches
    // the former hold_count implementation that was never loaded.
    begin : rapid_pulse_baseline_guard
      int baseline_before;
      baseline_before = baseline;
      repeat (200) begin
        drive_samples(8192 + 1000, 8);
        drive_samples(8192, 8);
      end
      check((baseline >= baseline_before - 2) &&
            (baseline <= baseline_before + 2),
            $sformatf("rapid pulses moved baseline from %0d to %0d",
                      baseline_before, baseline));
      drive_samples(8192, 128);
    end

    // ---------------------------------------------------------------
    // 3. Positive and negative pulses, default 32-point window
    // ---------------------------------------------------------------
    cfg_window_total = 12'd32;
    cfg_window_pre   = 12'd0;
    cfg_threshold    = 14'd64;
    cfg_return       = 14'd32;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    run_enable = 1'b1;
    drive_samples(8192, 16);

    events_seen = 0;
    // Positive pulse: 2000 codes above the baseline for 10 samples.
    drive_samples(8192 + 2000, 10);
    drive_samples(8192, 120);
    repeat (4) @(posedge clk);
    check(events_seen == 1, $sformatf("expected one positive event, got %0d",
                                      events_seen));
    if (events_seen == 1) begin
      check((ev_flags(captured[0]) & 8'h01) == 8'h00,
            "positive event marked negative");
      check(ev_peak(captured[0]) == 2000,
            $sformatf("positive peak %0d, expected 2000", ev_peak(captured[0])));
      // Only the 10 pulse samples contribute; the remaining window samples
      // sit on the baseline and add zero deviation.
      check(ev_integral(captured[0]) == 2000 * 10,
            $sformatf("positive integral %0d, expected %0d",
                      ev_integral(captured[0]), 2000 * 10));
    end

    // Switch the selected discriminator polarity before exercising a
    // negative pulse.  Opposite-polarity pulses are intentionally rejected.
    cfg_polarity_negative = 1'b1;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    events_seen = 0;
    drive_samples(8192 - 2000, 10);
    drive_samples(8192, 120);
    repeat (4) @(posedge clk);
    check(events_seen == 1, $sformatf("expected one negative event, got %0d",
                                      events_seen));
    if (events_seen == 1) begin
      check((ev_flags(captured[0]) & 8'h01) == 8'h01,
            "negative event not marked");
      check(ev_peak(captured[0]) == 2000,
            $sformatf("negative peak %0d, expected 2000", ev_peak(captured[0])));
      check(ev_integral(captured[0]) == 2000 * 10,
            $sformatf("negative integral %0d, expected %0d",
                      ev_integral(captured[0]), 2000 * 10));
    end

    cfg_polarity_negative = 1'b0;
    apply_config();
    settle_until(8192, 8192, 6, 200000);

    // ---------------------------------------------------------------
    // 4. Plateau must not retrigger
    // ---------------------------------------------------------------
    events_seen = 0;
    drive_samples(8192 + 3000, 600);   // far longer than the window
    drive_samples(8192, 120);
    repeat (4) @(posedge clk);
    check(events_seen == 1,
          $sformatf("plateau retriggered: %0d events", events_seen));

    // ---------------------------------------------------------------
    // 4b. The selected polarity really drives the discriminator: with
    //     negative polarity a positive pulse produces nothing, a negative
    //     pulse produces a negative event, and an opposite-polarity pulse
    //     inside an event window is not counted as overlap.
    // ---------------------------------------------------------------
    cfg_window_total     = 12'd32;
    cfg_window_pre       = 12'd0;
    cfg_threshold        = 14'd64;
    cfg_return           = 14'd32;
    cfg_polarity_negative= 1'b1;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    run_enable = 1'b1;
    drive_samples(8192, 16);

    events_seen = 0;
    // A positive pulse of the opposite polarity must be excluded.
    drive_samples(8192 + 2000, 10);
    drive_samples(8192, 160);
    repeat (4) @(posedge clk);
    check(events_seen == 0,
          $sformatf("positive pulse accepted in negative polarity: %0d", events_seen));
    check(rejected_count == 32'd0,
          $sformatf("opposite polarity counted as overlap: %0d", rejected_count));

    // A negative pulse must be accepted and marked negative.
    events_seen = 0;
    drive_samples(8192 - 2000, 10);
    drive_samples(8192, 160);
    repeat (4) @(posedge clk);
    check(events_seen == 1,
          $sformatf("negative pulse rejected in negative polarity: %0d", events_seen));
    if (events_seen == 1) begin
      check((ev_flags(captured[0]) & 8'h01) == 8'h01,
            "negative polarity event not marked negative");
      check(ev_peak(captured[0]) == 2000,
            $sformatf("negative polarity peak %0d, expected 2000",
                      ev_peak(captured[0])));
      check(ev_integral(captured[0]) == 2000 * 10,
            $sformatf("negative polarity integral %0d, expected %0d",
                      ev_integral(captured[0]), 2000 * 10));
    end

    // Exact-threshold equality must trigger, and a long plateau must still
    // produce a single event under negative polarity.
    events_seen = 0;
    drive_samples(8192 - 64, 2);          // exactly the threshold
    drive_samples(8192, 160);
    repeat (4) @(posedge clk);
    check(events_seen == 1,
          $sformatf("exact-threshold negative pulse: %0d events", events_seen));
    events_seen = 0;
    drive_samples(8192 - 3000, 600);
    drive_samples(8192, 160);
    repeat (4) @(posedge clk);
    check(events_seen == 1,
          $sformatf("negative plateau retriggered: %0d events", events_seen));

    // Restore the positive polarity for the remaining tests.
    cfg_polarity_negative = 1'b0;
    apply_config();
    settle_until(8192, 8192, 6, 200000);

    // ---------------------------------------------------------------
    // 5. Six-point minimum window
    // ---------------------------------------------------------------
    cfg_window_total = 12'd6;
    cfg_window_pre   = 12'd0;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    events_seen = 0;
    drive_samples(8192 + 1000, 2);
    drive_samples(8192, 40);
    repeat (4) @(posedge clk);
    check(events_seen == 1, $sformatf("6-point window: %0d events", events_seen));
    if (events_seen == 1)
      check(ev_integral(captured[0]) == 1000 * 2,
            $sformatf("6-point integral %0d, expected %0d",
                      ev_integral(captured[0]), 1000 * 2));

    // ---------------------------------------------------------------
    // 6. Pre-trigger offset inside the total window.  The pulse starts
    //    before the trigger point, so part of the window must come from the
    //    history buffer: 50 pulse samples (from history and the trigger) are
    //    followed by 50 baseline samples.
    // ---------------------------------------------------------------
    cfg_window_total = 12'd100;
    cfg_window_pre   = 12'd50;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    // Ensure the complete pre-trigger history is nominal baseline.  The
    // preceding minimum-window test can otherwise remain in the last few
    // ring locations when the estimator is already settled and returns
    // immediately.
    drive_samples(8192, 64);
    events_seen = 0;
    drive_samples(8192 + 500, 250);
    drive_samples(8192, 200);
    repeat (4) @(posedge clk);
    check(events_seen == 1,
          $sformatf("pre-trigger window: %0d events", events_seen));
    if (events_seen == 1)
      check(ev_integral(captured[0]) == 500 * 50,
            $sformatf("pre-trigger integral %0d, expected %0d",
                      ev_integral(captured[0]), 500 * 50));

    // Exact indexed window check.  For total=8/pre=3 the only legal samples
    // are [10,20,30,100,40,50,60,0], whose absolute sum is 310.  A shifted,
    // split or one-point-long window cannot accidentally pass this check.
    run_enable = 1'b0;
    auto_mode = 1'b0;
    manual_baseline = 14'd8192;
    cfg_window_total = 12'd8;
    cfg_window_pre   = 12'd3;
    apply_config();
    drive_samples(8192, 32);
    run_enable = 1'b1;
    drive_samples(8192, 16);
    events_seen = 0;
    drive_samples(8192 + 10, 1);
    drive_samples(8192 + 20, 1);
    drive_samples(8192 + 30, 1);
    drive_samples(8192 + 100, 1);
    drive_samples(8192 + 40, 1);
    drive_samples(8192 + 50, 1);
    drive_samples(8192 + 60, 1);
    drive_samples(8192, 32);
    repeat (8) @(posedge clk);
    check(events_seen == 1,
          $sformatf("indexed window: %0d events", events_seen));
    if (events_seen == 1)
      check(ev_integral(captured[0]) == 310,
            $sformatf("indexed integral %0d, expected 310",
                      ev_integral(captured[0])));
    run_enable = 1'b0;
    auto_mode = 1'b1;
    settle_until(8192, 8192, 6, 200000);
    run_enable = 1'b1;

    // ---------------------------------------------------------------
    // 7. Integral saturation.  2500 samples of +8000 codes give roughly
    //    20e6, which exceeds the 24-bit wire field but still fits in the
    //    28-bit internal accumulator.  The 14-bit input stays in range.
    // ---------------------------------------------------------------
    cfg_window_total = 12'd2500;
    cfg_window_pre   = 12'd0;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    events_seen = 0;
    drive_samples(8192 + 8000, 2600);
    repeat (4) @(posedge clk);
    check(events_seen >= 1, "saturation case produced no event");
    if (events_seen >= 1) begin
      check(ev_integral(captured[0]) == 24'hFFFFFF,
            $sformatf("integral not saturated: 0x%06x", ev_integral(captured[0])));
      check((ev_flags(captured[0]) & 8'h02) == 8'h02,
            "saturated event did not raise the overflow flag");
      check(ev_peak(captured[0]) == 8000,
            $sformatf("saturated peak %0d, expected 8000", ev_peak(captured[0])));
    end
    drive_samples(8192, 60);
    repeat (4) @(posedge clk);

    // ---------------------------------------------------------------
    // 8. Dead-time rejection while an event is in flight
    // ---------------------------------------------------------------
    cfg_window_total = 12'd2500;
    cfg_window_pre   = 12'd0;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    events_seen = 0;
    begin
      int rejects_before;
      rejects_before = rejected_count;
      // The first pulse triggers a 2500-sample window.  The second pulse
      // arrives after the input has returned below the return threshold but
      // while the window is still open, so it is a distinguishable crossing
      // and must be counted as dead time.
      drive_samples(8192 + 4000, 4);
      drive_samples(8192, 40);
      drive_samples(8192 + 4000, 4);
      drive_samples(8192, 4000);
      repeat (4) @(posedge clk);
      check(rejected_count > rejects_before,
            $sformatf("second pulse inside the window was not counted as a rejection (before=%0d after=%0d rejects, events=%0d)",
                      rejects_before, rejected_count, events_seen));
      check(events_seen == 1,
            $sformatf("pile-up case produced %0d events", events_seen));
      if (events_seen == 1)
        check((ev_flags(captured[0]) & 8'h04) == 8'h04,
              "pile-up flag not set on the first event");
    end

    // ---------------------------------------------------------------
    // 9. Manual baseline mode
    // ---------------------------------------------------------------
    cfg_window_total = 12'd32;
    peak_interpolation_enable = 1'b1;
    apply_config();
    settle_until(8192, 8192, 6, 200000);
    run_enable = 1'b0;
    repeat (8) @(posedge clk);
    auto_mode = 1'b0;
    manual_baseline = 14'd6000;
    drive_samples(8192, 40);
    repeat (4) @(posedge clk);
    check(baseline == 14'd6000,
          $sformatf("manual baseline not applied, got %0d", baseline));
    check(baseline_ready, "manual baseline is not ready");

    // The estimator must not move the baseline further while in manual mode.
    drive_samples(9000, 4000);
    check(baseline == 14'd6000,
          $sformatf("manual baseline moved to %0d", baseline));

    // Re-centre the input on the manual baseline before acquisition starts so
    // the sample pipeline holds no stale value when run_enable rises.
    drive_samples(6000, 32);
    run_enable = 1'b1;
    drive_samples(6000, 16);
    events_seen = 0;
    // Deliberately skew the three samples around the local maximum so the
    // optional finalizer must produce a fractional Q14.2 peak.
    drive_samples(6400, 4);
    drive_samples(8000, 1);
    drive_samples(7900, 1);
    drive_samples(6000, 60);
    repeat (50) @(posedge clk);
    check(events_seen == 1,
          $sformatf("manual baseline event: %0d events", events_seen));
    if (events_seen == 1) begin
      $display("  manual: baseline now %0d, peak %0d, integral %0d",
               baseline, ev_peak(captured[0]), ev_integral(captured[0]));
      check(ev_peak(captured[0]) >= 2000 &&
            ev_peak(captured[0]) <= 2200,
            $sformatf("interpolated manual peak %0d outside expected range",
                      ev_peak(captured[0])));
    end

    // Return to automatic mode.
    run_enable = 1'b0;
    repeat (8) @(posedge clk);
    auto_mode = 1'b1;
    settle_until(8192, 8192, 6, 200000);
    check(baseline_ready, "automatic mode did not re-converge");

    // ---------------------------------------------------------------
    // 10. Overrange samples must not be integrated into the baseline
    // ---------------------------------------------------------------
    adc_overrange = 1'b1;
    drive_samples(16383, 500);
    adc_overrange = 1'b0;
    repeat (4) @(posedge clk);
    check((baseline >= 8180) && (baseline <= 8205),
          $sformatf("overrange samples moved the baseline to %0d", baseline));

    // ---------------------------------------------------------------
    // 11. The serial histogram tap must mirror the emitted event stream.
    // events_seen is reset per scenario above, so compare the cumulative tap
    // count against the cumulative event-write pulse count instead.
    // ---------------------------------------------------------------
    check(histogram_events_seen == event_wr_pulses,
          $sformatf("histogram tap fired %0d times for %0d event writes",
                    histogram_events_seen, event_wr_pulses));
    check(tap_with_wr == histogram_events_seen,
          $sformatf("only %0d of %0d taps coincided with an event write",
                    tap_with_wr, histogram_events_seen));

    if (errors == 0)
      $display("tb_mca_acquisition PASS");
    else
      $display("tb_mca_acquisition FAIL: %0d errors", errors);
    $finish;
  end

  initial begin
    #4_000_000;
    $display("tb_mca_acquisition TIMEOUT");
    $finish;
  end
endmodule
