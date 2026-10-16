`timescale 1ns/1ps

// Histogram datapath regression: consecutive-address forwarding, alternating
// bins, deterministic ties, and manual-to-auto ready preservation.
module tb_mca_baseline;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic sample_valid = 1'b0;
  logic [13:0] sample = 14'd8192;
  logic sample_overrange = 1'b0;
  logic auto_mode = 1'b0;
  logic [13:0] manual_baseline = 14'd8192;
  logic [13:0] guard = 14'd16;
  logic [13:0] pulse_threshold = 14'd32;
  logic [5:0] track_shift = 6'd8;
  logic [5:0] recovery_shift = 6'd8;
  logic [5:0] fast_shift = 6'd4;
  logic [15:0] fast_samples = 16'd1024;
  logic [15:0] recovery_samples = 16'd1024;
  logic [13:0] recovery_tolerance_lsb = 14'd32;
  logic [15:0] offset_qualify = 16'd4096;
  logic [15:0] pulse_hold_samples = 16'd64;
  logic hold = 1'b0;
  logic train_hold = 1'b0;
  logic restart = 1'b0;
  logic restart_keep_valid = 1'b0;
  logic [13:0] baseline;
  logic [1:0] state;
  logic ready, update_enable;
  int errors = 0;

  mca_baseline dut (
    .clk(clk), .rst_n(rst_n), .sample_valid(sample_valid), .sample(sample),
    .sample_overrange(sample_overrange), .auto_mode(auto_mode),
    .manual_baseline(manual_baseline), .guard(guard),
    .pulse_threshold(pulse_threshold), .track_shift(track_shift),
    .recovery_shift(recovery_shift), .fast_shift(fast_shift),
    .fast_samples(fast_samples), .recovery_samples(recovery_samples),
    .recovery_tolerance_lsb(recovery_tolerance_lsb),
    .offset_qualify(offset_qualify),
    .pulse_hold_samples(pulse_hold_samples), .hold(hold),
    .train_hold(train_hold), .restart(restart),
    .restart_keep_valid(restart_keep_valid),
    .baseline(baseline), .state(state), .ready(ready),
    .update_enable(update_enable)
  );

  task automatic check(input bit condition, input string message);
    if (!condition) begin
      errors++;
      $display("FAIL [%0t] %s", $time, message);
    end
  endtask

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

  task automatic begin_training(input bit keep_valid);
    begin
      @(negedge clk);
      restart_keep_valid = keep_valid;
      restart = 1'b1;
      @(posedge clk); #1;
      restart = 1'b0;
      restart_keep_valid = 1'b0;
      while (dut.core_state_q != 3'd1)
        feed(14'd8192, 1'b0);
    end
  endtask

  task automatic finish_training;
    int timeout;
    begin
      timeout = 0;
      while ((state != 2'd1) && (timeout < 1100)) begin
        feed(14'd8192, 1'b0);
        timeout++;
      end
      check(state == 2'd1, "histogram scan did not finish");
    end
  endtask

  initial begin
    repeat (3) @(posedge clk);
    rst_n = 1'b1;
    @(posedge clk); #1;
    check(ready && baseline == 14'd8192,
          "manual mode was not immediately ready");

    // Manual-to-auto keeps the manual baseline valid while a 5% duty-cycle
    // positive pulse stream is already present.
    auto_mode = 1'b1;
    begin_training(1'b1);
    for (int i = 0; i < 4096; i++) begin
      feed((i % 625) < 31 ? 14'd8656 : 14'd8256, 1'b0);
      check(ready, "manual-to-auto training dropped ready");
    end
    finish_training();
    check(baseline == 14'd8264,
          $sformatf("modal centre mismatch: got %0d", baseline));
    check(dut.mode_count_q > 13'd3800,
          "baseline mode did not dominate the pulse samples");

    // Same-bin traffic exercises forwarding on every clock. The count must
    // reach 4096 without a lost read/modify/write update.
    begin_training(1'b0);
    for (int i = 0; i < 4096; i++) feed(14'd4101, 1'b0);
    finish_training();
    check(baseline == 14'd4104, "constant-bin modal centre mismatch");
    check(dut.mode_count_q == 13'd4096,
          $sformatf("constant-bin count got %0d", dut.mode_count_q));

    // Alternating bins create an exact tie. Strict greater-than must retain
    // the lower address, and each bucket must contain exactly 2048 samples.
    begin_training(1'b0);
    for (int i = 0; i < 4096; i++)
      feed(i[0] ? 14'd8213 : 14'd8197, 1'b0);
    finish_training();
    check(baseline == 14'd8200,
          $sformatf("tie did not select lower bin: got %0d", baseline));
    check(dut.mode_count_q == 13'd2048,
          $sformatf("alternating-bin count got %0d", dut.mode_count_q));

    sample_valid = 1'b0;
    if (errors == 0)
      $display("tb_mca_baseline PASS");
    else
      $display("tb_mca_baseline FAIL: %0d errors", errors);
    $finish;
  end
endmodule
