`timescale 1ns/1ps

// Short functional smoke test for the waveform capture engine.
//
// One capture is exercised end to end at the documented geometry, plus the
// two behaviours that are easiest to get wrong: the trigger sample must land
// at index 3750, and an opposite-polarity pulse must be ignored.
module tb_mca_waveform;
  localparam int PRE        = 3750;
  localparam int TOTAL      = 12500;
  localparam int FRAG_MAX   = 512;
  localparam int FRAG_COUNT = 25;
  localparam int LAST_FRAG  = TOTAL - (FRAG_COUNT - 1) * FRAG_MAX;
  localparam int BASELINE   = 8192;
  localparam int AMPL       = 900;
  localparam int PULSE_LEN  = 40;

  logic dco_clk  = 1'b0;
  logic gmii_clk = 1'b0;
  logic rst_dco_n = 1'b0;
  logic rst_net_n = 1'b0;
  always #4 dco_clk  = ~dco_clk;
  always #4 gmii_clk = ~gmii_clk;

  logic        run_enable = 1'b0;
  logic        holdoff = 1'b0;
  logic        abort = 1'b0;
  logic        config_apply = 1'b0;
  logic        cfg_polarity_negative = 1'b0;
  logic [13:0] cfg_threshold = 14'd100;
  logic [13:0] cfg_return = 14'd20;
  logic [13:0] cfg_baseline = BASELINE[13:0];
  logic        cfg_baseline_ready = 1'b1;
  logic [31:0] cfg_config_version = 32'h0002_0001;
  logic [31:0] cfg_session_id = 32'h1234_5678;
  logic        cfg_single_shot = 1'b0;

  logic [13:0] adc_data = BASELINE[13:0];
  logic        adc_overrange = 1'b0;
  logic [31:0] capture_id;
  logic [47:0] timestamp;
  logic [13:0] active_baseline;
  logic [13:0] active_threshold_out;
  logic        active_polarity_out;
  logic [31:0] status_snapshot, capture_count_snapshot, last_capture_snapshot;

  logic        frag_valid;
  logic [15:0] frag_index, frag_sample_count, frag_sample_offset;
  logic [15:0] frag_fragment_count;
  logic        frag_last;
  logic [31:0] frag_capture_id;
  logic [47:0] frag_timestamp;
  logic [13:0] frag_baseline, frag_threshold, frag_return;
  logic        frag_negative;
  logic [31:0] frag_config_version, frag_session_id;
  logic [15:0] frag_word_index = 16'd0;
  logic        frag_word_req = 1'b0;
  logic [15:0] frag_word;
  logic        frag_word_valid;
  logic        frag_ack = 1'b0;

  mca_waveform #(
    .ADC_WIDTH(14), .PRE_SAMPLES(PRE), .TOTAL_SAMPLES(TOTAL),
    .MEM_DEPTH(16384), .MEM_AW(14), .FRAGMENT_MAX(FRAG_MAX),
    .FRAGMENT_COUNT(FRAG_COUNT), .BASELINE_INIT(BASELINE)
  ) dut (
    .dco_clk(dco_clk), .rst_dco_n(rst_dco_n),
    .run_enable(run_enable), .holdoff(holdoff), .abort(abort),
    .config_apply(config_apply),
    .cfg_polarity_negative(cfg_polarity_negative),
    .cfg_threshold(cfg_threshold), .cfg_return(cfg_return),
    .cfg_baseline(cfg_baseline), .cfg_baseline_ready(cfg_baseline_ready),
    .cfg_config_version(cfg_config_version),
    .cfg_session_id(cfg_session_id), .cfg_single_shot(cfg_single_shot),
    .adc_data(adc_data), .adc_overrange(adc_overrange),
    .timestamp(timestamp), .active_baseline(active_baseline),
    .capture_id(capture_id), .active_threshold_out(active_threshold_out),
    .active_polarity_out(active_polarity_out),
    .gmii_clk(gmii_clk), .rst_net_n(rst_net_n),
    .status_snapshot(status_snapshot),
    .capture_count_snapshot(capture_count_snapshot),
    .last_capture_snapshot(last_capture_snapshot),
    .frag_valid(frag_valid), .frag_ack(frag_ack),
    .frag_index(frag_index), .frag_sample_count(frag_sample_count),
    .frag_sample_offset(frag_sample_offset),
    .frag_fragment_count(frag_fragment_count), .frag_last(frag_last),
    .frag_capture_id(frag_capture_id), .frag_timestamp(frag_timestamp),
    .frag_baseline(frag_baseline), .frag_threshold(frag_threshold),
    .frag_return(frag_return), .frag_negative(frag_negative),
    .frag_config_version(frag_config_version),
    .frag_session_id(frag_session_id),
    .frag_word_index(frag_word_index), .frag_word_req(frag_word_req),
    .frag_word(frag_word), .frag_word_valid(frag_word_valid)
  );

  // ---------------- stimulus ----------------
  integer sample_counter = 0;
  integer pulse_start = -1;
  logic   pulse_negative = 1'b0;

  always @(posedge dco_clk) begin
    if (rst_dco_n) begin
      if ((pulse_start >= 0) && (sample_counter >= pulse_start) &&
          (sample_counter < pulse_start + PULSE_LEN))
        adc_data <= pulse_negative ? (BASELINE - AMPL) : (BASELINE + AMPL);
      else
        adc_data <= BASELINE[13:0];
      sample_counter <= sample_counter + 1;
    end
  end

  integer errors = 0;
  logic [15:0] captured [0:TOTAL-1];

  task automatic check(input integer condition, input string label);
    begin
      if (condition == 0) begin
        $display("FAIL: %s", label);
        errors = errors + 1;
      end
    end
  endtask

  task automatic arm_pulse(input logic negative);
    begin
      pulse_start    = sample_counter;
      pulse_negative = negative;
    end
  endtask

  task automatic clear_pulse;
    begin
      pulse_start = -1;
    end
  endtask

  task automatic wait_samples(input integer count);
    integer target;
    begin
      target = sample_counter + count;
      while (sample_counter < target)
        @(posedge dco_clk);
    end
  endtask

  // Reads every fragment of one capture into `captured`.
  task automatic drain_capture(output integer samples, output integer fragments);
    integer offset, count;
    integer guard;
    logic was_last;
    begin : drain_capture_body
      samples = 0;
      fragments = 0;
      guard = 0;
      while (guard < 20000) begin
        if (frag_valid) begin
          offset = int'(frag_sample_offset);
          count  = int'(frag_sample_count);
          was_last = frag_last;
          check(int'(frag_index) == fragments, "fragment order");
          check(count == ((fragments == FRAG_COUNT - 1) ? LAST_FRAG : FRAG_MAX),
                "fragment sample count");
          check(int'(frag_baseline) == BASELINE, "locked baseline");
          check(int'(frag_threshold) == int'(cfg_threshold),
                "locked threshold");
          check(int'(frag_negative) == int'(cfg_polarity_negative),
                "locked polarity");
          for (int i = 0; i < count; i = i + 1) begin
            @(negedge gmii_clk);
            frag_word_index = i[15:0];
            frag_word_req   = 1'b1;
            do begin
              @(posedge gmii_clk);
              #1;
            end while (!frag_word_valid);
            captured[offset + i] = frag_word;
            @(negedge gmii_clk);
            frag_word_req = 1'b0;
            @(posedge gmii_clk); #1;
            check(!frag_word_valid,
                  "frag_word_valid remained asserted into next request");
          end
          samples   = samples + count;
          fragments = fragments + 1;
          @(negedge gmii_clk); frag_ack = 1'b1;
          @(posedge gmii_clk);
          @(negedge gmii_clk); frag_ack = 1'b0;
          if (was_last)
            disable drain_capture_body;
        end else begin
          @(posedge gmii_clk);
          guard = guard + 1;
        end
      end
      $display("FAIL: drain_capture timed out");
      errors = errors + 1;
    end
  endtask

  // Finds the first matching-polarity pulse sample.  The earlier
  // opposite-polarity pulse is intentionally still present in raw history;
  // it must not be mistaken for the trigger.
  function automatic integer find_trigger;
    integer found;
    begin
      found = -1;
      for (int i = 0; i < TOTAL; i = i + 1)
        if (found < 0 &&
            (captured[i] & 16'h3fff) == ((BASELINE + AMPL) & 16383))
          found = i;
      find_trigger = found;
    end
  endfunction

  integer samples_read, fragments_read, trigger_at;

  initial begin
    $display("=== tb_mca_waveform (smoke) ===");
    repeat (8) @(posedge dco_clk);
    rst_dco_n = 1'b1;
    rst_net_n = 1'b1;
    repeat (8) @(posedge dco_clk);

    // Apply the non-default threshold/session values before the first run.
    @(negedge dco_clk); config_apply = 1'b1;
    @(posedge dco_clk);
    @(negedge dco_clk); config_apply = 1'b0;
    @(posedge dco_clk);

    // 1. No capture before the pre-window is complete.
    run_enable = 1'b1;
    arm_pulse(1'b0);
    wait_samples(300);
    check(!frag_valid, "capture before the pre-window is complete");
    clear_pulse;

    // 2. Opposite polarity is ignored; the matching pulse triggers once.
    wait_samples(TOTAL + 300);
    arm_pulse(1'b1);
    wait_samples(400);
    check(!frag_valid, "opposite polarity exclusion");
    clear_pulse;
    arm_pulse(1'b0);
    wait_samples(400);
    drain_capture(samples_read, fragments_read);
    check(samples_read == TOTAL, "capture sample count");
    check(fragments_read == FRAG_COUNT, "capture fragment count");
    // Completion acknowledgement crosses back from gmii_clk through two
    // synchronizer stages before the DCO-domain completed-capture counter
    // advances.
    wait_samples(4);
    check(int'(capture_count_snapshot) == 1, "capture count");
    trigger_at = find_trigger();
    check(trigger_at == PRE,
          $sformatf("trigger index %0d expected %0d", trigger_at, PRE));
    check((captured[0] & 16'h3fff) == BASELINE[13:0], "first sample");
    check((captured[PRE] & 16'h3fff) == ((BASELINE + AMPL) & 16383),
          $sformatf("trigger sample %0d expected %0d",
                    captured[PRE] & 16'h3fff,
                    (BASELINE + AMPL) & 16383));
    check((captured[TOTAL-1] & 16'h3fff) == BASELINE[13:0], "last sample");
    wait_samples(20);
    check(!dut.mem_owned_net, "buffer released after upload");
    clear_pulse;

    // 3. Single-shot captures exactly once and reports state 5.
    run_enable = 1'b0;
    wait_samples(10);
    cfg_single_shot = 1'b1;
    @(negedge dco_clk); config_apply = 1'b1;
    @(posedge dco_clk);
    @(negedge dco_clk); config_apply = 1'b0;
    @(posedge dco_clk);
    run_enable = 1'b1;
    wait_samples(TOTAL + 300);
    arm_pulse(1'b0);
    wait_samples(400);
    drain_capture(samples_read, fragments_read);
    check(samples_read == TOTAL, "single-shot sample count");
    clear_pulse;
    wait_samples(TOTAL + 300);
    arm_pulse(1'b0);
    wait_samples(500);
    check(!frag_valid, "single-shot second capture");
    clear_pulse;
    check(int'(status_snapshot) == 5, "single-shot status code");

    if (errors == 0)
      $display("PASS: tb_mca_waveform");
    else
      $display("FAIL: tb_mca_waveform with %0d errors", errors);
    $finish;
  end

  initial begin
    #120_000_000;
    $display("FAIL: tb_mca_waveform timed out");
    $finish;
  end
endmodule
