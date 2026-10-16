`timescale 1ns/1ps

// Independent baseline estimator for one 14-bit, 125 MHz ADC channel.
//
// Automatic acquisition uses a fixed coarse histogram. The 14-bit sample is
// divided into 1024 bins of 16 codes, 4096 sample-valid instants are counted,
// and the centre of the most populated bin becomes the new baseline. Normal
// tracking then uses the original Q24 pulse-protected IIR. Histogram training
// is independent of acquisition busy so an already-running pulse stream
// cannot starve a manual-to-auto transition.
module mca_baseline #(
  parameter int ADC_WIDTH      = 14,
  parameter int BASELINE_WIDTH = 14,
  parameter int FRACTION_BITS  = 24
) (
  input  logic                        clk,
  input  logic                        rst_n,
  input  logic                        sample_valid,
  input  logic [ADC_WIDTH-1:0]        sample,
  input  logic                        sample_overrange,
  input  logic                        auto_mode,
  input  logic [BASELINE_WIDTH-1:0]   manual_baseline,
  input  logic [BASELINE_WIDTH-1:0]   guard,
  input  logic [BASELINE_WIDTH-1:0]   pulse_threshold,
  input  logic [5:0]                  track_shift,
  input  logic [5:0]                  recovery_shift,
  input  logic [5:0]                  fast_shift,
  input  logic [15:0]                 fast_samples,
  input  logic [15:0]                 recovery_samples,
  input  logic [13:0]                 recovery_tolerance_lsb,
  input  logic [15:0]                 offset_qualify,
  input  logic [15:0]                 pulse_hold_samples,
  // `hold` freezes only the TRACK IIR. Event busy must not starve histogram
  // acquisition. `train_hold` is reserved for a real front-end pause.
  input  logic                        hold,
  input  logic                        train_hold,
  input  logic                        restart,
  // A manual-to-auto transition has a valid manual seed and may continue
  // acquiring while the histogram runs. Range/filter restarts drive zero.
  input  logic                        restart_keep_valid,
  output logic [BASELINE_WIDTH-1:0]   baseline,
  output logic [1:0]                  state,
  output logic                        ready,
  output logic                        update_enable
);
  localparam logic [1:0] STATE_LEARN   = 2'd0;
  localparam logic [1:0] STATE_TRACK   = 2'd1;
  localparam logic [1:0] STATE_RELEARN = 2'd2;
  localparam logic [1:0] STATE_ERROR   = 2'd3;

  localparam int QW = ADC_WIDTH + FRACTION_BITS;
  localparam int HIST_AW = 10;
  localparam int HIST_BINS = 1 << HIST_AW;
  localparam int HIST_CW = 13;
  localparam logic [12:0] MIN_VALID_SAMPLES = 13'd3072;

  typedef enum logic [2:0] {
    CORE_CLEAR,
    CORE_COLLECT,
    CORE_DRAIN,
    CORE_SCAN,
    CORE_COMMIT,
    CORE_TRACK,
    CORE_ERROR
  } core_state_t;

  core_state_t core_state_q;
  logic training_is_recovery_q;
  logic ready_q;

  logic signed [QW:0] baseline_q;
  // Round the positive Q24 estimate to the nearest ADC code. Arithmetic
  // right shifts of a negative correction round down; truncating the final
  // accumulator would otherwise leave a settled baseline one code low.
  wire signed [QW:0] baseline_rounded_q = baseline_q +
      $signed({1'b0, {ADC_WIDTH{1'b0}}, 1'b1,
               {(FRACTION_BITS-1){1'b0}}});
  wire signed [QW:0] sample_q =
      $signed({1'b0, sample, {FRACTION_BITS{1'b0}}});
  wire signed [QW+1:0] error_now_q = sample_q - baseline_q;
  wire signed [ADC_WIDTH:0] base_int =
      baseline_rounded_q[FRACTION_BITS +: ADC_WIDTH + 1];
  wire signed [ADC_WIDTH:0] error_now_int =
      $signed({1'b0, sample}) - base_int;
  wire [ADC_WIDTH:0] error_now_abs =
      error_now_int[ADC_WIDTH] ? -error_now_int : error_now_int;

  wire [5:0] track_shift_eff = (track_shift < 6'd1) ? 6'd1 :
                               (track_shift > 6'd28) ? 6'd28 : track_shift;
  wire [5:0] recovery_shift_eff = (recovery_shift < 6'd1) ? 6'd1 :
                                  (recovery_shift > 6'd28) ? 6'd28 : recovery_shift;
  wire [15:0] offset_qualify_eff =
      (offset_qualify < 16'd16) ? 16'd16 : offset_qualify;
  wire [15:0] pulse_hold_eff =
      (pulse_hold_samples == 0) ? 16'd1 : pulse_hold_samples;

  // Legacy configuration ports remain readable/writeable but modal training
  // is deliberately fixed-size and does not consume them.
  wire legacy_cfg_unused = ^{fast_shift, fast_samples, recovery_samples,
                             recovery_tolerance_lsb};

  // ------------------------------------------------------------------
  // Histogram memory: one 1024 x 13 simple dual-port RAM (one RAMB18).
  // ------------------------------------------------------------------
  logic [HIST_AW-1:0] hist_raddr;
  logic [HIST_CW-1:0] hist_rdata;
  logic               hist_we;
  logic [HIST_AW-1:0] hist_waddr;
  logic [HIST_CW-1:0] hist_wdata;

`ifdef MCA_NO_XPM
  (* ram_style = "block" *) logic [HIST_CW-1:0] hist_mem [0:HIST_BINS-1];
  always_ff @(posedge clk) begin
    if (hist_we)
      hist_mem[hist_waddr] <= hist_wdata;
    hist_rdata <= hist_mem[hist_raddr];
  end
`else
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(HIST_AW), .ADDR_WIDTH_B(HIST_AW),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(HIST_CW),
    .CLOCKING_MODE("common_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("block"),
    .MEMORY_SIZE(HIST_BINS * HIST_CW), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(HIST_CW), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .USE_EMBEDDED_CONSTRAINT(0),
    .USE_MEM_INIT(0), .WAKEUP_TIME("disable_sleep"),
    .WRITE_DATA_WIDTH_A(HIST_CW), .WRITE_MODE_B("no_change")
  ) u_baseline_histogram (
    .dbiterrb(), .doutb(hist_rdata), .sbiterrb(),
    .addra(hist_waddr), .addrb(hist_raddr),
    .clka(clk), .clkb(clk), .dina(hist_wdata),
    .ena(1'b1), .enb(1'b1),
    .injectdbiterra(1'b0), .injectsbiterra(1'b0),
    // Histogram contents and the read pipeline are invalidated by the core
    // state machine; do not add a RAM reset control to the BRAM primitive.
    .regceb(1'b1), .rstb(1'b0), .sleep(1'b0), .wea(hist_we)
  );
`endif

  logic [HIST_AW-1:0] clear_addr_q;
  logic [11:0] collect_count_q;
  logic [12:0] valid_sample_count_q;
  logic [1:0] drain_count_q;

  // One read and one write can retire every clock. Two forwarding records
  // cover the pending write and a same-address read/write collision.
  logic               hist_read_valid_q;
  logic [HIST_AW-1:0] hist_read_addr_q;
  logic               hist_pending_valid_q;
  logic [HIST_AW-1:0] hist_pending_addr_q;
  logic [HIST_CW-1:0] hist_pending_data_q;
  logic               hist_committed_valid_q;
  logic [HIST_AW-1:0] hist_committed_addr_q;
  logic [HIST_CW-1:0] hist_committed_data_q;

  wire pending_hit = hist_pending_valid_q &&
                     (hist_read_addr_q == hist_pending_addr_q);
  wire committed_hit = hist_committed_valid_q &&
                       (hist_read_addr_q == hist_committed_addr_q);
  wire [HIST_CW-1:0] hist_forwarded_count =
      pending_hit ? hist_pending_data_q :
      committed_hit ? hist_committed_data_q : hist_rdata;

  logic [HIST_AW-1:0] scan_issue_addr_q;
  logic               scan_data_valid_q;
  logic [HIST_AW-1:0] scan_data_addr_q;
  logic [HIST_CW-1:0] mode_count_q;
  logic [HIST_AW-1:0] mode_bin_q;

  always_comb begin
    hist_raddr = (core_state_q == CORE_SCAN) ? scan_issue_addr_q
                                             : sample[13:4];
    hist_we    = 1'b0;
    hist_waddr = hist_pending_addr_q;
    hist_wdata = hist_pending_data_q;
    if (!train_hold) begin
      if (core_state_q == CORE_CLEAR) begin
        hist_we    = 1'b1;
        hist_waddr = clear_addr_q;
        hist_wdata = '0;
      end else if ((core_state_q == CORE_COLLECT) ||
                   (core_state_q == CORE_DRAIN)) begin
        hist_we = hist_pending_valid_q;
      end
    end
  end

  // ------------------------------------------------------------------
  // TRACK classifier and pipelined Q24 IIR.
  // ------------------------------------------------------------------
  logic                 class_valid_q;
  logic                 class_overrange_q;
  logic signed [QW+1:0] class_error_q;
  logic [ADC_WIDTH:0]   class_error_abs_q;
  logic                 class_pulse_hit_q;
  logic [5:0]           class_track_shift_q;
  logic [5:0]           class_recovery_shift_q;
  logic [15:0]          class_offset_qualify_q;
  logic [15:0]          class_pulse_hold_q;

  logic signed [QW+1:0] update_error_q;
  logic [5:0]           update_shift_q;
  logic                 update_valid_q;
  wire signed [QW+1:0] update_delta_q =
      update_error_q >>> update_shift_q;
  wire signed [QW+1:0] baseline_next_q = baseline_q + update_delta_q;

  logic [15:0] qualify_count_q;
  logic [15:0] hold_count_q;

  always_comb begin
    if (!auto_mode) begin
      baseline      = manual_baseline;
      state         = STATE_TRACK;
      ready         = 1'b1;
      update_enable = 1'b0;
    end else begin
      baseline = baseline_rounded_q[FRACTION_BITS +: BASELINE_WIDTH];
      ready    = ready_q;
      case (core_state_q)
        CORE_TRACK: state = STATE_TRACK;
        CORE_ERROR: state = STATE_ERROR;
        default:    state = training_is_recovery_q ? STATE_RELEARN
                                                   : STATE_LEARN;
      endcase
      update_enable = (core_state_q == CORE_TRACK) && !hold;
    end
  end

  // Control and RAM-address registers use synchronous reset. The memory
  // contents are initialized only by CORE_CLEAR.
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      core_state_q            <= CORE_CLEAR;
      training_is_recovery_q  <= 1'b0;
      ready_q                 <= 1'b0;
      baseline_q              <= $signed({1'b0, 14'd8192,
                                           {FRACTION_BITS{1'b0}}});
      clear_addr_q            <= '0;
      collect_count_q         <= '0;
      valid_sample_count_q    <= '0;
      drain_count_q           <= '0;
      hist_read_valid_q       <= 1'b0;
      hist_read_addr_q        <= '0;
      hist_pending_valid_q    <= 1'b0;
      hist_pending_addr_q     <= '0;
      hist_pending_data_q     <= '0;
      hist_committed_valid_q  <= 1'b0;
      hist_committed_addr_q   <= '0;
      hist_committed_data_q   <= '0;
      scan_issue_addr_q       <= '0;
      scan_data_valid_q       <= 1'b0;
      scan_data_addr_q        <= '0;
      mode_count_q            <= '0;
      mode_bin_q              <= '0;
      class_valid_q           <= 1'b0;
      class_overrange_q       <= 1'b0;
      class_error_q           <= '0;
      class_error_abs_q       <= '0;
      class_pulse_hit_q       <= 1'b0;
      class_track_shift_q     <= 6'd1;
      class_recovery_shift_q  <= 6'd1;
      class_offset_qualify_q  <= 16'd16;
      class_pulse_hold_q      <= 16'd1;
      update_error_q          <= '0;
      update_shift_q          <= 6'd1;
      update_valid_q          <= 1'b0;
      qualify_count_q         <= '0;
      hold_count_q            <= '0;
    end else if (!auto_mode) begin
      core_state_q            <= CORE_TRACK;
      training_is_recovery_q  <= 1'b0;
      ready_q                 <= 1'b1;
      baseline_q              <= $signed({1'b0, manual_baseline,
                                           {FRACTION_BITS{1'b0}}});
      clear_addr_q            <= '0;
      collect_count_q         <= '0;
      valid_sample_count_q    <= '0;
      drain_count_q           <= '0;
      hist_read_valid_q       <= 1'b0;
      hist_pending_valid_q    <= 1'b0;
      hist_committed_valid_q  <= 1'b0;
      scan_data_valid_q       <= 1'b0;
      class_valid_q           <= 1'b0;
      update_valid_q          <= 1'b0;
      qualify_count_q         <= '0;
      hold_count_q            <= '0;
    end else if (restart) begin
      core_state_q            <= CORE_CLEAR;
      training_is_recovery_q  <= 1'b0;
      ready_q                 <= restart_keep_valid && ready_q;
      clear_addr_q            <= '0;
      collect_count_q         <= '0;
      valid_sample_count_q    <= '0;
      drain_count_q           <= '0;
      hist_read_valid_q       <= 1'b0;
      hist_pending_valid_q    <= 1'b0;
      hist_committed_valid_q  <= 1'b0;
      scan_data_valid_q       <= 1'b0;
      mode_count_q            <= '0;
      mode_bin_q              <= '0;
      class_valid_q           <= 1'b0;
      update_valid_q          <= 1'b0;
      qualify_count_q         <= '0;
      hold_count_q            <= '0;
    end else begin
      scan_data_valid_q <= 1'b0;

      // Histogram RMW pipeline is independent of acquisition `hold`.
      if (!train_hold) begin
        hist_committed_valid_q <= hist_pending_valid_q &&
                                  ((core_state_q == CORE_COLLECT) ||
                                   (core_state_q == CORE_DRAIN));
        if (hist_pending_valid_q) begin
          hist_committed_addr_q <= hist_pending_addr_q;
          hist_committed_data_q <= hist_pending_data_q;
        end
        hist_pending_valid_q <= hist_read_valid_q;
        if (hist_read_valid_q) begin
          hist_pending_addr_q <= hist_read_addr_q;
          hist_pending_data_q <= hist_forwarded_count + 13'd1;
        end
        hist_read_valid_q <= (core_state_q == CORE_COLLECT) &&
                             sample_valid && !sample_overrange;
        if ((core_state_q == CORE_COLLECT) && sample_valid &&
            !sample_overrange)
          hist_read_addr_q <= sample[13:4];
      end

      case (core_state_q)
        CORE_CLEAR: if (!train_hold) begin
          hist_read_valid_q      <= 1'b0;
          hist_pending_valid_q   <= 1'b0;
          hist_committed_valid_q <= 1'b0;
          if (clear_addr_q == HIST_BINS-1) begin
            clear_addr_q         <= '0;
            collect_count_q      <= '0;
            valid_sample_count_q <= '0;
            core_state_q         <= CORE_COLLECT;
          end else begin
            clear_addr_q <= clear_addr_q + 1'b1;
          end
        end

        CORE_COLLECT: if (!train_hold && sample_valid) begin
          if (!sample_overrange)
            valid_sample_count_q <= valid_sample_count_q + 13'd1;
          if (collect_count_q == 12'd4095) begin
            collect_count_q <= '0;
            drain_count_q   <= '0;
            core_state_q    <= CORE_DRAIN;
          end else begin
            collect_count_q <= collect_count_q + 12'd1;
          end
        end

        CORE_DRAIN: if (!train_hold) begin
          hist_read_valid_q <= 1'b0;
          if (drain_count_q == 2'd1) begin
            drain_count_q <= '0;
            if (valid_sample_count_q < MIN_VALID_SAMPLES) begin
              core_state_q <= CORE_ERROR;
            end else begin
              scan_issue_addr_q <= '0;
              scan_data_valid_q <= 1'b0;
              mode_count_q      <= '0;
              mode_bin_q        <= '0;
              core_state_q      <= CORE_SCAN;
            end
          end else begin
            drain_count_q <= drain_count_q + 2'd1;
          end
        end

        CORE_SCAN: if (!train_hold) begin
          scan_data_valid_q <= 1'b1;
          scan_data_addr_q  <= scan_issue_addr_q;
          if (scan_issue_addr_q != HIST_BINS-1)
            scan_issue_addr_q <= scan_issue_addr_q + 1'b1;
          if (scan_data_valid_q) begin
            // Strict greater-than keeps the lower bin on an exact tie.
            if (hist_rdata > mode_count_q) begin
              mode_count_q <= hist_rdata;
              mode_bin_q   <= scan_data_addr_q;
            end
            if (scan_data_addr_q == HIST_BINS-1) begin
              scan_data_valid_q <= 1'b0;
              core_state_q      <= CORE_COMMIT;
            end
          end
        end

        CORE_COMMIT: begin
          baseline_q <= $signed({1'b0, mode_bin_q, 4'b1000,
                                 {FRACTION_BITS{1'b0}}});
          ready_q                <= 1'b1;
          training_is_recovery_q <= 1'b0;
          qualify_count_q        <= '0;
          hold_count_q           <= '0;
          class_valid_q          <= 1'b0;
          update_valid_q         <= 1'b0;
          core_state_q           <= CORE_TRACK;
        end

        CORE_TRACK: begin
          hist_read_valid_q      <= 1'b0;
          hist_pending_valid_q   <= 1'b0;
          hist_committed_valid_q <= 1'b0;
          if (hold) begin
            class_valid_q  <= 1'b0;
            update_valid_q <= 1'b0;
          end else begin
            if (update_valid_q)
              baseline_q <= baseline_next_q[QW:0];
            update_valid_q <= 1'b0;

            if (class_valid_q && !class_overrange_q) begin
              if (class_pulse_hit_q) begin
                hold_count_q <= class_pulse_hold_q;
                if (qualify_count_q + 16'd1 >=
                    class_offset_qualify_q) begin
                  core_state_q            <= CORE_CLEAR;
                  training_is_recovery_q  <= 1'b1;
                  ready_q                 <= 1'b0;
                  clear_addr_q            <= '0;
                  collect_count_q         <= '0;
                  valid_sample_count_q    <= '0;
                  qualify_count_q         <= '0;
                  hold_count_q            <= '0;
                  hist_read_valid_q       <= 1'b0;
                  hist_pending_valid_q    <= 1'b0;
                  hist_committed_valid_q  <= 1'b0;
                end else begin
                  qualify_count_q <= qualify_count_q + 16'd1;
                end
              end else begin
                qualify_count_q <= '0;
                if (hold_count_q != 0)
                  hold_count_q <= hold_count_q - 16'd1;
                else if (class_error_abs_q <= {1'b0, pulse_threshold}) begin
                  update_error_q <= class_error_q;
                  update_shift_q <=
                      (class_error_abs_q <= {1'b0, guard})
                        ? class_track_shift_q : class_recovery_shift_q;
                  update_valid_q <= 1'b1;
                end
              end
            end

            class_valid_q          <= sample_valid;
            class_overrange_q      <= sample_overrange;
            class_error_q          <= error_now_q;
            class_error_abs_q      <= error_now_abs;
            class_pulse_hit_q      <= error_now_abs > {1'b0, pulse_threshold};
            class_track_shift_q    <= track_shift_eff;
            class_recovery_shift_q <= recovery_shift_eff;
            class_offset_qualify_q <= offset_qualify_eff;
            class_pulse_hold_q     <= pulse_hold_eff;
          end
        end

        CORE_ERROR: begin
          hist_read_valid_q      <= 1'b0;
          hist_pending_valid_q   <= 1'b0;
          hist_committed_valid_q <= 1'b0;
          class_valid_q          <= 1'b0;
          update_valid_q         <= 1'b0;
        end

        default: core_state_q <= CORE_CLEAR;
      endcase
    end
  end

  wire _unused_ok = legacy_cfg_unused & 1'b0;
endmodule
