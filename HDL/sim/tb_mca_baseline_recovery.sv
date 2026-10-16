`timescale 1ns/1ps

// Recovery regression for modal reacquisition and the invalid-sample guard.
module tb_mca_baseline_recovery;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic sample_valid = 1'b0;
  logic [13:0] sample = 14'd8192;
  logic sample_overrange = 1'b0;
  logic auto_mode = 1'b1;
  logic restart = 1'b0;
  logic [13:0] baseline;
  logic [1:0] state;
  logic ready, update_enable;

  mca_baseline dut (
    .clk(clk), .rst_n(rst_n), .sample_valid(sample_valid), .sample(sample),
    .sample_overrange(sample_overrange), .auto_mode(auto_mode),
    .manual_baseline(14'd8192), .guard(14'd16),
    .pulse_threshold(14'd32), .track_shift(6'd8),
    .recovery_shift(6'd8), .fast_shift(6'd4),
    .fast_samples(16'd1024), .recovery_samples(16'd1024),
    .recovery_tolerance_lsb(14'd32), .offset_qualify(16'd16),
    .pulse_hold_samples(16'd64), .hold(1'b0), .train_hold(1'b0),
    .restart(restart), .restart_keep_valid(1'b0),
    .baseline(baseline), .state(state), .ready(ready),
    .update_enable(update_enable)
  );

  task automatic feed(input logic [13:0] value,
                      input logic overrange);
    begin
      @(negedge clk);
      sample = value;
      sample_overrange = overrange;
      sample_valid = 1'b1;
      @(posedge clk); #1;
    end
  endtask

  task automatic wait_collect;
    begin
      while (dut.core_state_q != 3'd1)
        feed(14'd8192, 1'b0);
    end
  endtask

  task automatic wait_track(input logic [13:0] value);
    int timeout;
    begin
      timeout = 0;
      while ((state != 2'd1) && (timeout < 7200)) begin
        feed(value, 1'b0);
        timeout++;
      end
      if (state != 2'd1)
        $fatal(1, "modal training did not reach TRACK");
    end
  endtask

  task automatic pulse_restart;
    begin
      @(negedge clk); restart = 1'b1;
      @(posedge clk); #1; restart = 1'b0;
    end
  endtask

  initial begin
    repeat (3) @(posedge clk);
    rst_n = 1'b1;

    // Initial training under a 25% positive-pulse stream. The baseline bin
    // remains modal and must be selected despite there being no 95%-quiet
    // recovery window.
    wait_collect();
    for (int i = 0; i < 4096; i++)
      feed((i % 125) < 31 ? 14'd8800 : 14'd8192, 1'b0);
    wait_track(14'd8192);
    if (!ready || baseline != 14'd8200)
      $fatal(1, "25%% pulse stream trained baseline=%0d ready=%0b",
             baseline, ready);

    // A persistent step qualifies a relearn. The old baseline becomes
    // invalid, new triggers are blocked, and the new mode restores ready.
    for (int i = 0; i < 18; i++) feed(14'd9000, 1'b0);
    if (state != 2'd2 || ready)
      $fatal(1, "persistent step did not enter non-ready RELEARN");
    wait_collect();
    for (int i = 0; i < 4096; i++) feed(14'd9000, 1'b0);
    wait_track(14'd9000);
    if (!ready || baseline != 14'd9000)
      $fatal(1, "step recovery baseline=%0d ready=%0b", baseline, ready);

    // Fewer than 3072 non-overrange samples must enter ERROR and must not
    // publish a candidate baseline.
    pulse_restart();
    wait_collect();
    for (int i = 0; i < 4096; i++)
      feed(14'd7000, i >= 2048);
    for (int i = 0; i < 8; i++) feed(14'd7000, 1'b1);
    if (state != 2'd3 || ready)
      $fatal(1, "insufficient valid samples did not enter ERROR");

    $display("tb_mca_baseline_recovery PASS");
    $finish;
  end
endmodule
