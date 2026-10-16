`timescale 1ns/1ps

// Single-channel acquisition core: trigger qualification, peak detection and
// integration for one 14-bit ADC stream.
//
// Key properties required by the single-channel MCA format:
//   * Bipolar threshold crossing.  A crossing arms one event, and the
//     discriminator only re-arms after the magnitude has returned to or below
//     the return threshold, so long plateaus cannot retrigger.
//   * The polarity selected in the channel control word is part of the
//     comparison.  Positive polarity triggers on `ADC - B >= H` and negative
//     polarity on `B - ADC >= H`, both evaluated by the shared helpers in
//     mca_pkg.  A pulse of the opposite polarity therefore never produces an
//     event, a pile-up rejection, or an overlap count.
//   * The integration window is 6..2500 samples of 8 ns (48 ns..20 us).  The
//     pre-trigger count is part of the total window, so the window covers
//     [trigger-pre, trigger+total-pre-1] and always contains exactly
//     `cfg_window_total` samples.
//   * The baseline and the polarity are latched at the trigger point, and the
//     reported peak is the largest absolute deviation from that latched
//     baseline inside the window (Q14.2 on the wire).
//   * The integration accumulates the absolute deviation of every window
//     sample.  It uses 28 bits internally and saturates to 24 bits on the
//     wire while raising the overflow flag.
//   * A distinguishable new threshold crossing while an event is in progress
//     is counted as a dead-time rejection and marks the current event as
//     pile-up.  FIFO drops are counted separately.
//   * A new configuration is only taken over once the in-flight event has
//     retired, so the commit acknowledgement reflects a fully applied change.
module mca_acquisition #(
  parameter int ADC_WIDTH     = 14,
  parameter int MAX_WINDOW    = 2500,
  parameter int MIN_WINDOW    = 6,
  parameter int BASELINE_INIT = 8192,
  parameter int INTEGRAL_BITS = 28
) (
  input  logic                        dco_clk,
  input  logic                        rst_n,

  // Control
  input  logic                        run_enable,
  // Keeps the sample/integration pipeline alive while an event is draining;
  // only this qualifier is gated by the external-trigger window.
  input  logic                        trigger_enable,
  input  logic                        holdoff,
  input  logic                        abort_event,
  input  logic                        clear_counts,

  // Sampled configuration.  A value is only taken over while `config_apply`
  // is asserted, so the whole configuration changes atomically.
  input  logic                        config_apply,
  input  logic [ADC_WIDTH-1:0]        cfg_threshold,
  input  logic [ADC_WIDTH-1:0]        cfg_return,
  input  logic [11:0]                 cfg_window_total,
  input  logic [11:0]                 cfg_window_pre,
  input  logic                        cfg_polarity_negative,
  input  logic                        peak_interpolation_enable,
  input logic psd_enable,
  input logic [11:0] psd_tail_start, psd_tail_end,
  input logic [31:0] config_version,
  output logic [231:0] tagged_event_data,

  // Baseline estimator interface
  output logic                        baseline_hold,
  input  logic [ADC_WIDTH-1:0]        baseline,
  input  logic [1:0]                  baseline_state,
  input  logic                        baseline_ready,

  // Sample stream
  input  logic [ADC_WIDTH-1:0]        adc_data,
  input  logic                        adc_overrange,

  // Event stream
  output logic                        event_wr,
  output logic [95:0]                 event_data,
  input  logic                        event_full,

  // Transport-independent event tap.  This pulse is produced for every
  // completed event before the raw-event FIFO full decision, allowing the
  // local UART spectra to remain lossless when Ethernet is disconnected.
  output logic                        histogram_event_valid,
  output logic [ADC_WIDTH-1:0]        histogram_peak,
  output logic [23:0]                 histogram_integral,

  // DCO-domain timestamp and counters
  output logic [47:0]                 timestamp,
  output logic [ADC_WIDTH-1:0]        active_baseline,
  output logic [1:0]                  active_baseline_state,
  output logic [31:0]                 event_count,
  output logic [31:0]                 rejected_count,
  output logic [31:0]                 dropped_count,
  output logic                        event_active,
  output logic                        accepting,
  output logic                        window_open
);
  localparam int RING_AW = 12;                    // 4096 entries >= 2500
  localparam int WINW    = 12;
  // The polarity-aware difference is evaluated in at least 15 signed bits by
  // mca_pkg::polarity_delta, whose width is exactly this value.
  localparam int ABSW    = mca_pkg::POLARITY_DIFF_WIDTH;

  localparam logic [WINW-1:0] WINDOW_MAX_U = MAX_WINDOW[WINW-1:0];
  localparam logic [WINW-1:0] WINDOW_MIN_U = MIN_WINDOW[WINW-1:0];
  localparam logic [ADC_WIDTH-1:0] ADC_MAX_U = {ADC_WIDTH{1'b1}};

  logic [ADC_WIDTH-1:0] active_threshold_q;
  logic [ADC_WIDTH-1:0] active_return_q;
  logic [WINW-1:0]      active_total_q;
  logic [WINW-1:0]      active_pre_q;
  logic                 active_polarity_q;
  logic                 event_negative_q;

  // -------------------------------------------------------------------
  // Sample pipeline.  Sample data is registered once; every downstream
  // computation happens in the following cycle, which keeps the 125 MHz
  // sample path free of long combinational chains.
  // -------------------------------------------------------------------
  logic [ADC_WIDTH-1:0] sample_q;
  logic                 overrange_q;
  logic                 valid_q;
  logic                 run_valid_q;
  logic                 run_valid_q2;
  logic [47:0]          sample_timestamp_q;
  logic [RING_AW-1:0]   sample_wptr_q;

  // Two registered processing stages follow the ADC input register.  The
  // first stage performs the subtract/absolute-value operation; the second
  // registers the comparisons and all sample metadata consumed by the event
  // state machine.  This keeps arithmetic and state-enable logic in separate
  // 125 MHz timing cones while retaining one-sample-per-cycle throughput.
  logic [ABSW-1:0]       magnitude_q;
  logic                  magnitude_negative_q;
  logic                  magnitude_overrange_q;
  logic                  magnitude_valid_q;
  logic [47:0]           magnitude_timestamp_q;
  logic [RING_AW-1:0]    magnitude_wptr_q;
  logic [ABSW-1:0]       decision_magnitude_q;
  logic                  decision_negative_q;
  logic                  decision_overrange_q;
  logic                  decision_valid_q;
  logic                  decision_threshold_q;
  logic                  decision_returned_q;
  logic [47:0]           decision_timestamp_q;
  logic [RING_AW-1:0]    decision_wptr_q;
  logic [ADC_WIDTH-1:0] latched_baseline;
  logic [1:0]           mode_phase_q;      // 0=idle, 1=window active

  wire signed [ABSW-1:0] error_active =
      mca_pkg::polarity_delta(sample_q, baseline, active_polarity_q);
  wire [ABSW-1:0] error_active_abs = mca_pkg::polarity_magnitude(error_active);

  // -------------------------------------------------------------------
  // Pre-trigger history ring buffer
  // -------------------------------------------------------------------
  logic [RING_AW-1:0]   ring_wptr;
  logic [RING_AW-1:0]   ring_rptr;
  logic                 ring_rd_valid;
  logic                 ring_clear_done;
  logic [ADC_WIDTH:0] ring_mem_rdata;
  logic [ADC_WIDTH:0] ring_bypass_data_q;
  logic                 ring_bypass_valid_q;
  wire [ADC_WIDTH-1:0] ring_rdata =
      ring_bypass_valid_q ? ring_bypass_data_q[ADC_WIDTH-1:0] : ring_mem_rdata[ADC_WIDTH-1:0];
  wire ring_overrange = ring_bypass_valid_q ? ring_bypass_data_q[ADC_WIDTH] : ring_mem_rdata[ADC_WIDTH];

  // Simulation uses an equivalent inferred memory so no XPM libraries are
  // required.  Production synthesis explicitly selects block RAM; relying on
  // inference alone lets Vivado 2020.2 choose thousands of LUTRAM bits here.
`ifdef MCA_NO_XPM
  logic [ADC_WIDTH:0] ring_mem [0:(1<<RING_AW)-1];
  always_ff @(posedge dco_clk) begin
    if (rst_n) begin
      if (!ring_clear_done)
        ring_mem[ring_wptr] <= {ADC_WIDTH{1'b0}};
      else
        ring_mem[ring_wptr] <= {adc_overrange,adc_data};
      ring_mem_rdata <= ring_mem[ring_rptr];
    end else begin
      ring_mem_rdata <= {ADC_WIDTH{1'b0}};
    end
  end
`else
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(RING_AW),
    .ADDR_WIDTH_B(RING_AW),
    .AUTO_SLEEP_TIME(0),
    .BYTE_WRITE_WIDTH_A(ADC_WIDTH+1),
    .CLOCKING_MODE("common_clock"),
    .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"),
    .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"),
    .MEMORY_PRIMITIVE("block"),
    .MEMORY_SIZE((1 << RING_AW) * (ADC_WIDTH+1)),
    .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(ADC_WIDTH+1),
    .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"),
    .USE_EMBEDDED_CONSTRAINT(0),
    .USE_MEM_INIT(0),
    .WAKEUP_TIME("disable_sleep"),
    .WRITE_DATA_WIDTH_A(ADC_WIDTH+1),
    .WRITE_MODE_B("no_change")
  ) u_pretrigger_bram (
    .dbiterrb(),
    .doutb(ring_mem_rdata),
    .sbiterrb(),
    .addra(ring_wptr),
    .addrb(ring_rptr),
    .clka(dco_clk),
    .clkb(dco_clk),
    .dina(ring_clear_done ? {adc_overrange,adc_data} : {(ADC_WIDTH+1){1'b0}}),
    .ena(rst_n),
    .enb(1'b1),
    .injectdbiterra(1'b0),
    .injectsbiterra(1'b0),
    .regceb(1'b1),
    .rstb(~rst_n),
    .sleep(1'b0),
    .wea(rst_n)
  );
`endif

  // Make same-address read/write behavior deterministic instead of relying
  // on the selected BRAM collision mode.
  always_ff @(posedge dco_clk) begin
    if (!rst_n) begin
      ring_bypass_valid_q <= 1'b0;
      ring_bypass_data_q  <= {ADC_WIDTH{1'b0}};
    end else begin
      ring_bypass_valid_q <= ring_rd_valid && ring_clear_done &&
                             (ring_rptr == ring_wptr);
      ring_bypass_data_q  <= {adc_overrange,adc_data};
    end
  end
  wire signed [ABSW-1:0] ring_error =
      mca_pkg::polarity_delta(ring_rdata, latched_baseline,
                              event_negative_q);
  wire [ABSW-1:0] ring_abs = mca_pkg::polarity_magnitude(ring_error);

  // Register the BRAM result after subtract/absolute-value and before the
  // accumulator.  Without this stage the pre-trigger path was
  // BRAM -> subtract -> absolute value -> 28-bit saturating add in one 8 ns
  // cycle (0.004 ns setup slack in the synthesis report).
  logic [ABSW-1:0] ring_magnitude_q;
  logic            ring_magnitude_valid_q;

  localparam logic [RING_AW-1:0] RING_SIZE = {RING_AW{1'b1}} + 12'd1;
  // decision_wptr_q is carried alongside the trigger sample and therefore is
  // its exact physical RAM address.  Walking back `pre` locations lands on
  // T-pre; registered RAM latency is represented by ring_rd_valid and does
  // not change the address calculation.
  wire [RING_AW-1:0] ring_pre_back = active_pre_q;
  wire [RING_AW-1:0] ring_pre_start =
      (decision_wptr_q >= ring_pre_back)
          ? (decision_wptr_q - ring_pre_back)
          : (decision_wptr_q + RING_SIZE - ring_pre_back);

  // Pre-trigger history read address.  Modular (ring-size) arithmetic lands
  // the read pointer on the oldest sample of the pre-window; it then advances
  // one address per pre cycle so the window covers exactly [T-pre, T-1].

  // -------------------------------------------------------------------
  // Integration accumulator (saturating)
  // -------------------------------------------------------------------
  logic [INTEGRAL_BITS-1:0] integral_acc;
  logic [INTEGRAL_BITS-1:0] pre_integral_acc;
  function automatic logic [INTEGRAL_BITS-1:0] integral_add(
      input logic [INTEGRAL_BITS-1:0] acc,
      input logic [ABSW-1:0]          add);
    logic [INTEGRAL_BITS:0] sum;
    begin
      sum = {1'b0, acc} + {{(INTEGRAL_BITS + 1 - ABSW){1'b0}}, add};
      integral_add = sum[INTEGRAL_BITS] ? {INTEGRAL_BITS{1'b1}}
                                        : sum[INTEGRAL_BITS-1:0];
    end
  endfunction

  // -------------------------------------------------------------------
  // Event and discriminator state
  // -------------------------------------------------------------------
  logic                 armed_q;
  logic                 busy_q;
  logic                 returned_q;
  logic                 rearmed_q;
  logic                 threshold_hit_q;
  logic [WINW-1:0]      pre_remaining_q;
  logic [WINW-1:0]      pre_result_remaining_q;
  logic [WINW-1:0]      post_remaining_q;
  logic                 pre_done_q;
  logic                 post_done_q;
  logic                 event_pileup_q;
  logic                 event_overflow_q;
  logic                 event_integral_sat_q;
  logic [ADC_WIDTH-1:0] peak_q;
  logic [ADC_WIDTH-1:0] pre_peak_q;
  logic [47:0]          event_timestamp;
  logic [ADC_WIDTH-1:0]  trigger_peak_q;

  // Separate chronological trackers for the BRAM pre-window and the live
  // post-window.  Their last samples are combined at event completion so the
  // two boundary centres (T-1 and T) can also be tested without a second BRAM
  // pass.
  logic [ADC_WIDTH-1:0] pre_prev2_q, pre_prev1_q;
  logic [ADC_WIDTH-1:0] post_prev2_q, post_prev1_q, post_first_q;
  logic [WINW-1:0] pre_count_q, post_count_q;
  logic             interp_best_valid_q;
  logic [ADC_WIDTH-1:0] interp_best_left_q;
  logic [ADC_WIDTH-1:0] interp_best_center_q;
  logic [ADC_WIDTH-1:0] interp_best_right_q;

  logic                  candidate_valid_w;
  logic [ADC_WIDTH-1:0]  candidate_left_w;
  logic [ADC_WIDTH-1:0]  candidate_center_w;
  logic [ADC_WIDTH-1:0]  candidate_right_w;
  logic event_psd_q;
  logic                  interp_busy;
  logic                  interp_result_valid;
  logic [95:0]           interp_event_data;
  logic [ADC_WIDTH-1:0]  interp_histogram_peak;
  logic [23:0]           interp_histogram_integral;

  wire [ADC_WIDTH-1:0] live_peak_candidate =
      (decision_magnitude_q > {1'b0, peak_q})
          ? decision_magnitude_q[ADC_WIDTH-1:0] : peak_q;
  wire [ADC_WIDTH-1:0] pre_peak_candidate =
      (ring_magnitude_q > {1'b0, pre_peak_q})
          ? ring_magnitude_q[ADC_WIDTH-1:0] : pre_peak_q;
  wire [ADC_WIDTH-1:0] event_peak_final =
      (pre_peak_q > peak_q) ? pre_peak_q : peak_q;
  wire peak_overflow = (decision_magnitude_q > {1'b0, ADC_MAX_U});

  wire boundary_pre_valid = (pre_count_q >= 12'd2) &&
                            (pre_prev1_q > pre_prev2_q) &&
                            (pre_prev1_q > trigger_peak_q);
  wire boundary_trigger_valid = (pre_count_q >= 12'd1) &&
                                (post_count_q >= 12'd1) &&
                                (trigger_peak_q > pre_prev1_q) &&
                                (trigger_peak_q > post_first_q);

  // Pick the largest strict local maximum.  Equal-height plateaus are left
  // invalid intentionally; the interpolator then emits the raw Q14.2 peak.
  always_comb begin
    candidate_valid_w  = interp_best_valid_q;
    candidate_left_w   = interp_best_left_q;
    candidate_center_w = interp_best_center_q;
    candidate_right_w  = interp_best_right_q;
    if (boundary_pre_valid &&
        (!candidate_valid_w || pre_prev1_q > candidate_center_w)) begin
      candidate_valid_w  = 1'b1;
      candidate_left_w   = pre_prev2_q;
      candidate_center_w = pre_prev1_q;
      candidate_right_w  = trigger_peak_q;
    end
    if (boundary_trigger_valid &&
        (!candidate_valid_w || trigger_peak_q > candidate_center_w)) begin
      candidate_valid_w  = 1'b1;
      candidate_left_w   = pre_prev1_q;
      candidate_center_w = trigger_peak_q;
      candidate_right_w  = post_first_q;
    end
    if (!candidate_valid_w) begin
      candidate_left_w   = event_peak_final;
      candidate_center_w = event_peak_final;
      candidate_right_w  = event_peak_final;
    end
  end

  // Saturated wire value of the accumulator.  Every consumer of the integral
  // uses this, so the 24-bit field can never wrap.
  wire [INTEGRAL_BITS:0] integral_sum_ext =
      {1'b0, integral_acc} + {1'b0, pre_integral_acc};
  wire integral_internal_sat = integral_sum_ext[INTEGRAL_BITS];
  wire [INTEGRAL_BITS-1:0] integral_combined = integral_internal_sat
      ? {INTEGRAL_BITS{1'b1}} : integral_sum_ext[INTEGRAL_BITS-1:0];
  wire integral_sat = integral_internal_sat ||
      (integral_combined[INTEGRAL_BITS-1:24] != '0);
  wire [23:0] integral_wire = integral_sat ? 24'hFFFFFF
                                           : integral_combined[23:0];

  wire event_finalize = busy_q && pre_done_q && post_done_q;
  wire [7:0] interpolation_flags = mca_pkg::event_flags(
      event_negative_q,
      event_overflow_q | event_integral_sat_q | integral_sat,
      event_pileup_q,
      5'd0);

  mca_peak_interpolator u_peak_interpolator (
    .clk(dco_clk),
    .rst_n(rst_n),
    .flush(clear_counts || abort_event || config_apply),
    .enable((peak_interpolation_enable === 1'b1) && !event_psd_q),
    .submit(event_finalize),
    .raw_peak(event_peak_final),
    .left_sample(candidate_left_w),
    .center_sample(candidate_center_w),
    .right_sample(candidate_right_w),
    .candidate_valid(candidate_valid_w),
    .invalid_overrange(event_overflow_q | event_integral_sat_q | integral_sat),
    .timestamp(event_timestamp),
    .integral(integral_wire),
    .flags(interpolation_flags),
    .busy(interp_busy),
    .result_valid(interp_result_valid),
    .event_data(interp_event_data),
    .histogram_peak(interp_histogram_peak),
    .histogram_integral(interp_histogram_integral)
  );

  // -------------------------------------------------------------------
  // Trigger and pile-up discrimination
  // -------------------------------------------------------------------
  logic [13:0] magnitude_raw_q, decision_raw_q, ring_raw_q;
  logic ring_overrange_q;
  logic [31:0] event_version_q;
  logic psd_pending, psd_have_legacy, psd_have_charge;
  logic [95:0] psd_legacy_q;
  logic [13:0] psd_base_q;
  logic signed [31:0] psd_total_q, psd_tail_q;
  logic [7:0] psd_quality_q;
  logic psd_busy, psd_result_valid;
  logic signed [31:0] psd_total_result, psd_tail_result;
  logic [7:0] psd_quality_result;
  wire trigger_ok;
  wire psd_start = trigger_ok && decision_threshold_q && (psd_enable === 1'b1);
  logic [11:0] psd_post_index;
  logic [11:0] psd_initial_post_index;
  wire psd_pre_tail = (pre_count_q >= psd_tail_start) && (pre_count_q < psd_tail_end);
  wire psd_post_tail = psd_start
      ? ((active_pre_q >= psd_tail_start) && (active_pre_q < psd_tail_end))
      : ((psd_post_index >= psd_tail_start) && (psd_post_index < psd_tail_end));
  mca_psd_charge u_psd_charge (
    .clk(dco_clk), .rst_n(rst_n),
    .flush(abort_event || holdoff || config_apply || clear_counts),
    .start(psd_start), .finish(event_finalize && event_psd_q),
    .baseline(baseline), .negative(active_polarity_q),
    .sample_valid({psd_start || (busy_q && event_psd_q && !post_done_q),
                   busy_q && event_psd_q && ring_magnitude_valid_q && !pre_done_q}),
    .sample_tail({psd_post_tail,psd_pre_tail}),
    .sample_overrange({decision_overrange_q,ring_overrange_q}),
    .sample_pre(ring_raw_q), .sample_post(decision_raw_q),
    .busy(psd_busy), .result_valid(psd_result_valid),
    .qtotal(psd_total_result), .qtail(psd_tail_result), .quality(psd_quality_result)
  );
  wire threshold_hit_now = decision_threshold_q;
  wire returned_now      = decision_returned_q;
  wire window_active     = (mode_phase_q != 2'd0);
  // Samples latched while acquisition was stopped (or while the domain was
  // paused) are not valid trigger candidates: run_enable is sampled alongside
  // the data pipeline so a stale sample can never start an event, and a
  // second stage guarantees the pipeline has fully flushed.
  assign trigger_ok        = trigger_enable && !holdoff && !abort_event && !clear_counts &&
                           decision_valid_q && baseline_ready &&
                           !busy_q && !interp_busy && !psd_pending && armed_q && !window_active;

  // A new, distinguishable crossing while an event is in flight is dead time.
  // "Distinguishable" means the input has already returned below the return
  // threshold since the trigger, so a plateau or a slow edge is never counted
  // twice.  The window and the busy period coincide, so a second crossing
  // anywhere inside the window is a rejection.
  wire pileup_reject = trigger_enable && !holdoff && !abort_event && !clear_counts &&
                       decision_valid_q && busy_q &&
                       threshold_hit_now && !threshold_hit_q && rearmed_q;

  // -------------------------------------------------------------------
  // Sequential logic
  // -------------------------------------------------------------------
  always_ff @(posedge dco_clk or negedge rst_n) begin
    if (!rst_n) begin
      active_threshold_q   <= ADC_WIDTH'(32);
      active_return_q      <= ADC_WIDTH'(16);
      active_total_q       <= 12'd32;
      active_pre_q         <= 12'd0;
      active_polarity_q    <= 1'b0;
      sample_q             <= {ADC_WIDTH{1'b0}};
      overrange_q          <= 1'b0;
      valid_q              <= 1'b0;
      run_valid_q          <= 1'b0;
      run_valid_q2         <= 1'b0;
      sample_timestamp_q   <= '0;
      sample_wptr_q        <= '0;
      magnitude_q          <= '0;
      magnitude_negative_q <= 1'b0;
      magnitude_overrange_q<= 1'b0;
      magnitude_valid_q    <= 1'b0;
      magnitude_timestamp_q<= '0;
      magnitude_wptr_q     <= '0;
      decision_magnitude_q <= '0;
      decision_negative_q  <= 1'b0;
      decision_overrange_q <= 1'b0;
      decision_valid_q     <= 1'b0;
      decision_threshold_q <= 1'b0;
      decision_returned_q  <= 1'b1;
      decision_timestamp_q <= '0;
      decision_wptr_q      <= '0;
      ring_magnitude_q     <= '0;
      ring_magnitude_valid_q <= 1'b0;
      ring_wptr            <= '0;
      ring_rptr            <= '0;
      ring_rd_valid        <= 1'b0;
      ring_clear_done      <= 1'b0;
      integral_acc         <= '0;
      pre_integral_acc     <= '0;
      armed_q              <= 1'b1;
      busy_q               <= 1'b0;
      returned_q           <= 1'b0;
      rearmed_q            <= 1'b1;
      threshold_hit_q      <= 1'b0;
      mode_phase_q         <= 2'd0;
      pre_remaining_q      <= '0;
      pre_result_remaining_q <= '0;
      post_remaining_q     <= '0;
      pre_done_q           <= 1'b1;
      post_done_q          <= 1'b1;
      latched_baseline     <= ADC_WIDTH'(BASELINE_INIT);
      event_negative_q     <= 1'b0;
      event_pileup_q       <= 1'b0;
      event_overflow_q     <= 1'b0;
      event_integral_sat_q <= 1'b0;
      peak_q               <= {ADC_WIDTH{1'b0}};
      pre_peak_q           <= {ADC_WIDTH{1'b0}};
      event_timestamp      <= '0;
      trigger_peak_q       <= '0;
      pre_prev2_q          <= '0;
      pre_prev1_q          <= '0;
      post_prev2_q         <= '0;
      post_prev1_q         <= '0;
      post_first_q         <= '0;
      pre_count_q          <= '0;
      post_count_q         <= '0;
      interp_best_valid_q  <= 1'b0;
      interp_best_left_q   <= '0;
      interp_best_center_q <= '0;
      interp_best_right_q  <= '0;
      event_wr             <= 1'b0;
      event_data           <= '0;
      histogram_event_valid <= 1'b0;
      histogram_peak       <= '0;
      histogram_integral   <= '0;
      timestamp            <= '0;
      magnitude_raw_q<=0; decision_raw_q<=0; ring_raw_q<=0; ring_overrange_q<=0;
      event_version_q<=0; event_psd_q<=0; psd_pending<=0;
      psd_have_legacy<=0; psd_have_charge<=0; psd_legacy_q<=0;
      psd_base_q<=0; psd_total_q<=0; psd_tail_q<=0; psd_quality_q<=0;
      tagged_event_data<=0; psd_post_index<=0; psd_initial_post_index<=1;
      event_count          <= '0;
      rejected_count       <= '0;
      dropped_count        <= '0;
    end else begin
      event_wr   <= 1'b0;
      histogram_event_valid <= 1'b0;
      valid_q    <= 1'b1;
      run_valid_q<= run_enable;
      run_valid_q2 <= run_valid_q;

      // The pre-trigger history is cleared once after reset so a window that
      // starts before the buffer has been filled can never integrate stale
      // or unknown data.  The BRAM write port above uses this pointer and
      // clear state; this block only advances the associated control state.
      if (!ring_clear_done) begin
        ring_wptr            <= ring_wptr + 1'b1;
        if (ring_wptr == {RING_AW{1'b1}})
          ring_clear_done <= 1'b1;
      end else begin
        ring_wptr            <= ring_wptr + 1'b1;
      end
      timestamp  <= timestamp + 48'd1;
      sample_q   <= adc_data;
      overrange_q<= adc_overrange;
      sample_timestamp_q <= timestamp;
      sample_wptr_q      <= ring_wptr;

      magnitude_raw_q <= sample_q;
      decision_raw_q <= magnitude_raw_q;
      ring_raw_q <= ring_rdata;
      ring_overrange_q <= ring_overrange;
      magnitude_q           <= error_active_abs;
      magnitude_negative_q  <= error_active[ABSW-1];
      magnitude_overrange_q <= overrange_q;
      magnitude_valid_q     <= valid_q && run_valid_q && run_valid_q2;
      magnitude_timestamp_q <= sample_timestamp_q;
      magnitude_wptr_q      <= sample_wptr_q;

      decision_magnitude_q <= magnitude_q;
      decision_negative_q  <= magnitude_negative_q;
      decision_overrange_q <= magnitude_overrange_q;
      decision_valid_q     <= magnitude_valid_q;
      decision_threshold_q <= !magnitude_negative_q &&
                              (magnitude_q >= {1'b0, active_threshold_q});
      decision_returned_q  <= magnitude_negative_q ||
                              (magnitude_q <= {1'b0, active_return_q});
      decision_timestamp_q <= magnitude_timestamp_q;
      decision_wptr_q      <= magnitude_wptr_q;

      ring_magnitude_q       <= ring_abs;
      ring_magnitude_valid_q <= (mode_phase_q == 2'd1) && ring_rd_valid;

      if (clear_counts) begin
        event_count    <= '0;
        rejected_count <= '0;
        dropped_count  <= '0;
      end

      // The optional finalizer emits the event after either one bypass
      // register or the Divider Generator pipeline.  Keeping the FIFO write
      // here preserves the original drop/count semantics even though the
      // acquisition window itself has already retired.
      if (interp_result_valid && !abort_event && !holdoff && !clear_counts && !config_apply) begin
        histogram_event_valid <= 1'b1;
        histogram_peak       <= interp_histogram_peak;
        histogram_integral   <= interp_histogram_integral;
        if (event_psd_q) begin
          psd_have_legacy<=1;
          psd_legacy_q<=interp_event_data;
        end else if (event_full) begin
          dropped_count <= dropped_count + 1'b1;
        end else begin
          event_wr    <= 1'b1;
          event_count <= event_count + 1'b1;
          event_data  <= interp_event_data;
          tagged_event_data <= {event_version_q,8'h01,96'd0,interp_event_data};
        end
      end

      if (psd_result_valid) begin
        psd_have_charge<=1; psd_total_q<=psd_total_result;
        psd_tail_q<=psd_tail_result; psd_quality_q<=psd_quality_result;
      end
      if (psd_pending && psd_have_legacy && psd_have_charge &&
          !abort_event && !holdoff && !clear_counts && !config_apply) begin
        psd_pending<=0; psd_have_legacy<=0; psd_have_charge<=0;
        if (event_full) dropped_count<=dropped_count+1'b1;
        else begin
          event_wr<=1; event_count<=event_count+1'b1;
          event_data<=psd_legacy_q;
          tagged_event_data<={event_version_q,8'h07,psd_legacy_q,
             psd_total_q,psd_tail_q,2'd0,psd_base_q,psd_quality_q,8'd0};
        end
      end
      if (clear_counts || config_apply) begin
        psd_pending<=0; psd_have_charge<=0; psd_have_legacy<=0;
      end
      if (abort_event || holdoff || (clear_counts && event_psd_q)) begin
        psd_pending<=0; psd_have_charge<=0; psd_have_legacy<=0;
        // A range change, an explicit abort or a manual pulse terminates the
        // in-flight event so the next event cannot mix two baselines.
        busy_q         <= 1'b0;
        returned_q     <= 1'b0;
        mode_phase_q   <= 2'd0;
        integral_acc   <= '0;
        pre_integral_acc <= '0;
        pre_remaining_q<= '0;
        pre_result_remaining_q <= '0;
        post_remaining_q <= '0;
        pre_done_q     <= 1'b1;
        post_done_q    <= 1'b1;
        armed_q        <= 1'b1;
        ring_rd_valid  <= 1'b0;
        ring_magnitude_valid_q <= 1'b0;
      end else begin
        // A fresh configuration is taken over only while the core is idle.
        if (config_apply && !busy_q && !interp_busy && !window_active) begin
          psd_initial_post_index<=cfg_window_pre+12'd1;
          active_threshold_q <= (cfg_threshold == 0) ? ADC_WIDTH'(1)
                                                     : cfg_threshold;
          active_return_q    <= cfg_return;
          active_total_q     <= (cfg_window_total < WINDOW_MIN_U) ? WINDOW_MIN_U
                               : (cfg_window_total > WINDOW_MAX_U) ? WINDOW_MAX_U
                               : cfg_window_total;
          active_pre_q       <= (cfg_window_pre >= cfg_window_total)
                                ? (cfg_window_total - 1'b1) : cfg_window_pre;
          active_polarity_q  <= cfg_polarity_negative;
          armed_q            <= 1'b1;
          returned_q         <= 1'b0;
        end

        // Dead-time rejection accounting.
        if (pileup_reject) begin
          event_pileup_q <= 1'b1;
          rejected_count <= rejected_count + 1'b1;
        end

        // Discriminator re-arm.  A crossing stores the new edge state and
        // clears trigger enable until the magnitude has returned to or below
        // the return threshold, which gives one event per threshold
        // excursion and a clean plateau guard.
        threshold_hit_q <= threshold_hit_now;
        if (returned_now)
          armed_q <= 1'b1;
        else if (trigger_ok && threshold_hit_now)
          armed_q <= 1'b0;

        // While an event is in flight, remember whether the input has come
        // back below the return threshold between threshold crossings.
        if (returned_now)
          rearmed_q <= 1'b1;
        else if (threshold_hit_now)
          rearmed_q <= 1'b0;

        if (trigger_ok && threshold_hit_now) begin
          busy_q               <= 1'b1;
          returned_q           <= 1'b0;
          rearmed_q            <= 1'b0;
          latched_baseline     <= baseline;
          event_version_q<=config_version;
          event_psd_q<=(psd_enable === 1'b1);
          psd_pending<=(psd_enable === 1'b1);
          psd_post_index<=psd_initial_post_index;
          psd_have_legacy<=0; psd_have_charge<=0; psd_base_q<=baseline;
          event_negative_q     <= active_polarity_q;
          event_pileup_q       <= 1'b0;
          event_overflow_q     <= decision_overrange_q;
          event_integral_sat_q <= 1'b0;
          peak_q               <= decision_magnitude_q[ADC_WIDTH-1:0];
          pre_peak_q           <= '0;
          trigger_peak_q       <= decision_magnitude_q[ADC_WIDTH-1:0];
          pre_prev2_q          <= '0;
          pre_prev1_q          <= '0;
          post_prev2_q         <= decision_magnitude_q[ADC_WIDTH-1:0];
          post_prev1_q         <= '0;
          post_first_q         <= '0;
          pre_count_q          <= '0;
          post_count_q         <= '0;
          interp_best_valid_q  <= 1'b0;
          interp_best_left_q   <= '0;
          interp_best_center_q <= '0;
          interp_best_right_q  <= '0;
          event_timestamp      <= decision_timestamp_q;
          integral_acc         <= integral_add('0, decision_magnitude_q);
          pre_integral_acc     <= '0;
          pre_remaining_q      <= (active_pre_q >= active_total_q) ? '0
                                                                  : active_pre_q;
          pre_result_remaining_q <= (active_pre_q >= active_total_q) ? '0
                                                                     : active_pre_q;
          post_remaining_q     <= (active_total_q > active_pre_q)
                                  ? (active_total_q - active_pre_q - 1'b1)
                                  : '0;
          pre_done_q           <= (active_pre_q == 0);
          post_done_q          <= (active_total_q <= (active_pre_q + 1'b1));
          // The oldest history sample is at T-pre.  The BRAM-valid pipeline
          // handles read latency without shifting this physical address.
          ring_rptr            <= ring_pre_start;
          // Debug probe removed in production build.
          ring_rd_valid        <= 1'b0;
          mode_phase_q         <= 2'd1;
        end else if (busy_q) begin
          // Issue the pre-trigger BRAM reads while the live post-trigger
          // stream is accumulated independently below.  The old mutually
          // exclusive phases skipped T+1..T+pre and then integrated samples
          // beyond the requested right edge.
          if (pre_remaining_q != 0) begin
            ring_rptr      <= ring_rptr + 12'd1;
            ring_rd_valid  <= 1'b1;
            pre_remaining_q <= pre_remaining_q - 1'b1;
          end else begin
            ring_rd_valid <= 1'b0;
          end

          if (ring_magnitude_valid_q && !pre_done_q) begin
            pre_integral_acc <= integral_add(pre_integral_acc,
                                             ring_magnitude_q);
            pre_peak_q <= pre_peak_candidate;
            if ((pre_count_q >= 12'd2) &&
                (pre_prev1_q > pre_prev2_q) &&
                (pre_prev1_q > ring_magnitude_q) &&
                (!interp_best_valid_q ||
                 (pre_prev1_q > interp_best_center_q))) begin
              interp_best_valid_q  <= 1'b1;
              interp_best_left_q   <= pre_prev2_q;
              interp_best_center_q <= pre_prev1_q;
              interp_best_right_q  <= ring_magnitude_q;
            end
            pre_prev2_q <= pre_prev1_q;
            pre_prev1_q <= ring_magnitude_q;
            if (pre_count_q < 12'd2500)
              pre_count_q <= pre_count_q + 1'b1;
            if (pre_result_remaining_q <= 12'd1) begin
              pre_result_remaining_q <= '0;
              pre_done_q <= 1'b1;
            end else begin
              pre_result_remaining_q <= pre_result_remaining_q - 1'b1;
            end
          end

          if (!post_done_q) begin
            psd_post_index<=psd_post_index+12'd1;
            integral_acc <= integral_add(integral_acc, decision_magnitude_q);
            peak_q       <= live_peak_candidate;
            if (post_count_q == 0) begin
              post_first_q <= decision_magnitude_q[ADC_WIDTH-1:0];
              post_prev1_q <= decision_magnitude_q[ADC_WIDTH-1:0];
              post_count_q <= 12'd1;
            end else begin
              if ((post_prev1_q > post_prev2_q) &&
                  (post_prev1_q > decision_magnitude_q) &&
                  (!interp_best_valid_q ||
                   (post_prev1_q > interp_best_center_q))) begin
                interp_best_valid_q  <= 1'b1;
                interp_best_left_q   <= post_prev2_q;
                interp_best_center_q <= post_prev1_q;
                interp_best_right_q  <= decision_magnitude_q[ADC_WIDTH-1:0];
              end
              post_prev2_q <= post_prev1_q;
              post_prev1_q <= decision_magnitude_q[ADC_WIDTH-1:0];
              if (post_count_q < 12'd2500)
                post_count_q <= post_count_q + 1'b1;
            end
            if (decision_overrange_q || peak_overflow)
              event_overflow_q <= 1'b1;
            if (post_remaining_q <= 12'd1) begin
              post_remaining_q <= '0;
              post_done_q <= 1'b1;
            end else begin
              post_remaining_q <= post_remaining_q - 1'b1;
            end
          end

          // Both flags are observed one clock after their final samples were
          // committed, so the combined sum and peak below include every point
          // of [T-pre, T+total-pre-1] exactly once.
          if (pre_done_q && post_done_q) begin
            busy_q       <= 1'b0;
            mode_phase_q <= 2'd0;
            returned_q   <= returned_now;
            ring_rd_valid<= 1'b0;
            integral_acc     <= '0;
            pre_integral_acc <= '0;
          end

        end
      end
    end
  end

  assign active_baseline       = baseline;
  assign active_baseline_state = baseline_state;
  assign event_active          = busy_q || interp_busy || psd_pending;
  assign accepting             = trigger_enable && !holdoff && baseline_ready &&
                                 !busy_q && !interp_busy && !psd_pending && !window_active;
  assign baseline_hold         = busy_q || interp_busy || psd_pending || holdoff;
endmodule
