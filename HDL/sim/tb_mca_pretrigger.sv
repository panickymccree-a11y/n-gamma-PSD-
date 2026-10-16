`timescale 1ns/1ps

// Minimal pre-trigger window check.  The input is held at one level for the
// whole window so the expected integral is simply level * samples.
module tb_mca_pretrigger;
  logic clk = 1'b0;
  always #4 clk = ~clk;
  logic rst_n = 1'b0;

  logic run_enable = 1'b0, holdoff = 1'b0, abort_event = 1'b0, clear_counts = 1'b0;
  logic config_apply = 1'b0;
  logic [13:0] cfg_threshold = 14'd64, cfg_return = 14'd32;
  logic [11:0] cfg_window_total = 12'd100, cfg_window_pre = 12'd50;

  logic        auto_mode = 1'b1;
  logic [13:0] manual_baseline = 14'd8192;
  logic [13:0] base_guard = 14'd16;
  logic [13:0] base_pulse_threshold = 14'd32;
  logic [5:0]  fast_shift = 6'd4, track_shift = 6'd8, recovery_shift = 6'd8;
  logic [15:0] fast_samples = 16'd1024, recovery_samples = 16'd1024;

  logic [13:0] adc_data = 14'd8192;
  logic        adc_overrange = 1'b0;

  logic        baseline_hold;
  logic [13:0] baseline;
  logic [1:0]  baseline_state;
  logic        baseline_ready, baseline_update;

  logic        event_wr;
  logic [95:0] event_data;
  logic        event_full = 1'b0;
  logic [47:0] timestamp;
  logic [13:0] active_baseline;
  logic [1:0]  active_baseline_state;
  logic [31:0] event_count, rejected_count, dropped_count;
  logic        event_active, accepting, window_open;
  // Observe the serial spectrum tap so the DUT output is not left dangling.
  logic        histogram_event_valid;
  logic [13:0] histogram_event_peak;
  logic [23:0] histogram_event_integral;

  int events_seen = 0;
  logic [95:0] captured;

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
    .offset_qualify(16'd2048),
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
    .cfg_window_pre(cfg_window_pre), .cfg_polarity_negative(1'b0),
    .baseline_hold(baseline_hold), .baseline(baseline),
    .baseline_state(baseline_state), .baseline_ready(baseline_ready),
    .adc_data(adc_data), .adc_overrange(adc_overrange),
    .event_wr(event_wr), .event_data(event_data), .event_full(event_full),
    .timestamp(timestamp),
    .active_baseline(active_baseline),
    .active_baseline_state(active_baseline_state),
    .event_count(event_count), .rejected_count(rejected_count),
    .dropped_count(dropped_count), .event_active(event_active),
    .accepting(accepting), .window_open(window_open),
    .histogram_event_valid(histogram_event_valid),
    .histogram_peak(histogram_event_peak),
    .histogram_integral(histogram_event_integral)
  );

  always @(posedge clk) begin
    if (event_wr) begin
      captured = event_data;
      events_seen = events_seen + 1;
      $display("EVENT peak=%0d integral=%0d (total=%0d pre=%0d)",
               int'(event_data[47:32])/4, int'(event_data[31:8]),
               u_dut.active_total_q, u_dut.active_pre_q);
    end
  end

  task automatic drive_samples(input int value, input int count);
    begin
      repeat (count) begin
        @(negedge clk);
        adc_data = value[13:0];
      end
    end
  endtask

  initial begin
    repeat (8) @(posedge clk);
    rst_n = 1'b1;

    repeat (100) @(posedge clk);
    $display("post-reset: wptr=%0d rptr=%0d clear_done=%0b mem0=%0d mem1000=%0d",
             u_dut.ring_wptr, u_dut.ring_rptr, u_dut.ring_clear_done,
             u_dut.ring_mem[0], u_dut.ring_mem[1000]);
    drive_samples(8192, 30000);
    @(negedge clk); config_apply = 1'b1;
    @(negedge clk); config_apply = 1'b0;
    drive_samples(8192, 64);
    $display("configured total=%0d pre=%0d", u_dut.active_total_q,
             u_dut.active_pre_q);

    // The pulse starts 100 samples before the trigger is expected: the
    // threshold is crossed on the first pulse sample, so the pre-window
    // covers 50 pulse samples and the post window covers 49 more.
    run_enable = 1'b1;
    drive_samples(8192, 200);
    events_seen = 0;
    fork
      begin
        drive_samples(8192 + 500, 250);
        drive_samples(8192, 200);
      end
      begin
        for (int i = 0; i < 120; i++) begin
          @(posedge clk);
          if (event_active)
            $display("  ph=%0d pre=%0d start=%0d apre=%0d wptr=%0d rdata=%0d rabs=%0d acc=%0d rdv=%0b",
                     u_dut.mode_phase_q, u_dut.pre_remaining_q, u_dut.ring_rptr, u_dut.active_pre_q,
                     u_dut.ring_wptr, u_dut.ring_rdata, u_dut.ring_abs,
                     u_dut.integral_acc,
                     u_dut.ring_rd_valid);
        end
        repeat (190) @(negedge clk);
      end
    join
    repeat (8) @(posedge clk);
    $display("events=%0d", events_seen);
    for (int a = 1300; a < 2000; a++)
      if (u_dut.ring_mem[a] != 8192) $display("  PULSE mem[%0d]=%0d", a, u_dut.ring_mem[a]);
    for (int a = 2000; a < 2010; a++)
      if (u_dut.ring_mem[a] != 8192) $display("  PULSE mem[%0d]=%0d", a, u_dut.ring_mem[a]);
    if (events_seen == 1)
      $display("PRE-TRIGGER %s (integral=%0d, expect 25000)", (int'(captured[31:8]) == 25000) ? "PASS" : "FAIL", int'(captured[31:8]));
    else
      $display("PRE-TRIGGER FAIL: no event");
    $finish;
  end
endmodule
