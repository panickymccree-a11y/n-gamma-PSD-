`timescale 1ns/1ps

// Waveform capture engine.
//
// Geometry is fixed by docs/protocol.md:
//   * 14-bit offset-binary ADC samples, 8 ns apart, 12500 samples per record
//   * trigger point T is the first sample that satisfies the selected-polarity
//     threshold condition and lands at array index 3750
//   * the record covers [T-3750, T+8749], i.e. -30 us .. +69.992 us
//
// Storage is one 16384 x 16 bit simple dual-port block RAM.  The write port
// runs in the sampling domain (DCO125M) and the read port runs in the network
// domain (gmii_clk).  Each word is
//
//   bit13:0  ADC offset-binary code
//   bit14    per-sample overrange
//   bit15    always zero
//
// The memory is never reset word by word.  A valid-history counter guarantees
// that a trigger is only accepted once the whole record is backed by samples
// that were really written, so unwritten memory can never be published.
//
// Ownership of the buffer is handed to the network domain with one stable
// descriptor plus a request/acknowledge toggle pair, so multi-bit address,
// counter and metadata fields are never synchronised bit by bit.
//
// States reported to the control domain:
//   0 stopped, 1 filling pre-window, 2 waiting for trigger,
//   3 capturing post-window, 4 uploading, 5 single-shot complete
module mca_waveform #(
  parameter int ADC_WIDTH      = 14,
  parameter int PRE_SAMPLES    = 3750,
  parameter int TOTAL_SAMPLES  = 12500,
  parameter int MEM_DEPTH      = 16384,
  parameter int MEM_AW         = 14,
  parameter int FRAGMENT_MAX   = 512,
  parameter int FRAGMENT_COUNT = 25,
  parameter int BASELINE_INIT  = 8192
) (
  // ------------------------------------------------------------------
  // Sampling domain (DCO125M)
  // ------------------------------------------------------------------
  input  logic                    dco_clk,
  input  logic                    rst_dco_n,

  // Level controls.  `run_enable` starts and stops the engine, `holdoff`
  // (range change or manual pulse) invalidates the history, `abort` drops the
  // current capture without publishing anything.
  input  logic                    run_enable,
  input  logic                    holdoff,
  input  logic                    abort,

  // Atomically applied configuration
  input  logic                    config_apply,
  input  logic                    cfg_polarity_negative,
  input  logic [ADC_WIDTH-1:0]    cfg_threshold,
  input  logic [ADC_WIDTH-1:0]    cfg_return,
  input  logic [ADC_WIDTH-1:0]    cfg_baseline,
  input  logic                    cfg_baseline_ready,
  input  logic [31:0]             cfg_config_version,
  input  logic [31:0]             cfg_session_id,
  input  logic                    cfg_single_shot,

  // Sample stream
  input  logic [ADC_WIDTH-1:0]    adc_data,
  input  logic                    adc_overrange,

  // Sampling-domain observation
  output logic [47:0]             timestamp,
  output logic [13:0]             active_baseline,
  output logic [31:0]             capture_id,
  output logic [13:0]             active_threshold_out,
  output logic                    active_polarity_out,

  // ------------------------------------------------------------------
  // Network domain (gmii_clk)
  // ------------------------------------------------------------------
  input  logic                    gmii_clk,
  input  logic                    rst_net_n,

  // Consistent read-only status and capture-count snapshots.  Both are
  // published through a toggle pair so no multi-bit value is ever sampled
  // while it is changing.
  output logic [31:0]             status_snapshot,
  output logic [31:0]             capture_count_snapshot,
  output logic [31:0]             last_capture_snapshot,

  // Fragment interface.  `frag_valid` stays high with a stable descriptor and
  // frozen memory ownership until the transmitter pulses `frag_ack`.
  //
  // Sample read-out is a registered request/response: the transmitter holds
  // `frag_word_index` with `frag_word_req` until `frag_word_valid`; the BRAM
  // read and output register form a two-cycle latency path.
  output logic                    frag_valid,
  output logic [15:0]             frag_index,
  output logic [15:0]             frag_sample_count,
  output logic [15:0]             frag_sample_offset,
  output logic [15:0]             frag_fragment_count,
  output logic                    frag_last,
  output logic [31:0]             frag_capture_id,
  output logic [47:0]             frag_timestamp,
  output logic [13:0]             frag_baseline,
  output logic [13:0]             frag_threshold,
  output logic [13:0]             frag_return,
  output logic                    frag_negative,
  output logic [31:0]             frag_config_version,
  output logic [31:0]             frag_session_id,
  input  logic [15:0]             frag_word_index,
  input  logic                    frag_word_req,
  output logic [15:0]             frag_word,
  output logic                    frag_word_valid,
  input  logic                    frag_ack
);
  localparam int ABSW = mca_pkg::POLARITY_DIFF_WIDTH;

  localparam logic [31:0] ST_STOPPED   = mca_pkg::WAVE_STATE_STOPPED;
  localparam logic [31:0] ST_FILLING   = mca_pkg::WAVE_STATE_FILLING;
  localparam logic [31:0] ST_WAITING   = mca_pkg::WAVE_STATE_WAITING;
  localparam logic [31:0] ST_CAPTURING = mca_pkg::WAVE_STATE_CAPTURING;
  localparam logic [31:0] ST_UPLOADING = mca_pkg::WAVE_STATE_UPLOADING;
  localparam logic [31:0] ST_SINGLE    = mca_pkg::WAVE_STATE_SINGLE_DONE;

  localparam logic [MEM_AW-1:0] PRE_OFFSET = PRE_SAMPLES[MEM_AW-1:0];
  localparam logic [15:0] POST_COUNT_U =
      (TOTAL_SAMPLES - PRE_SAMPLES);
  localparam logic [15:0] FRAG_COUNT_U  = FRAGMENT_COUNT;
  localparam logic [15:0] FRAG_MAX_U    = FRAGMENT_MAX;
  localparam logic [15:0] TOTAL_U       = TOTAL_SAMPLES;
  localparam logic [15:0] LAST_SAMPLES_U =
      TOTAL_SAMPLES - (FRAGMENT_COUNT - 1) * FRAGMENT_MAX;

  // ==================================================================
  // Sampling domain
  // ==================================================================

  // ------------------------------------------------------------------
  // Buffered configuration (only replaced while the engine is idle)
  // ------------------------------------------------------------------
  logic [ADC_WIDTH-1:0] active_threshold_q;
  logic [ADC_WIDTH-1:0] active_return_q;
  logic [ADC_WIDTH-1:0] cfg_baseline_q;
  logic                 active_polarity_q;
  logic                 cfg_baseline_ready_q;
  logic [31:0]          active_version_q;
  logic [31:0]          active_session_q;
  logic                 active_single_q;

  // ------------------------------------------------------------------
  // Three-stage sample pipeline.  Every metadata field travels with the
  // sample so the trigger is identified by sample identity (address and
  // timestamp) and never by "the control cycle that noticed the crossing".
  // ------------------------------------------------------------------
  logic [ADC_WIDTH-1:0] p0_data;
  logic                 p0_or;
  logic                 p0_run;
  logic                 p0_hold;
  logic [MEM_AW-1:0]    p0_wptr;
  logic [47:0]          p0_ts;

  logic                 p1_negative;
  logic                 p1_or;
  logic                 p1_run;
  logic                 p1_hold;
  logic [MEM_AW-1:0]    p1_wptr;
  logic [47:0]          p1_ts;
  logic                 p1_threshold;
  logic                 p1_returned;

  logic                 p2_negative;
  logic                 p2_or;
  logic                 p2_run;
  logic                 p2_threshold;
  logic                 p2_returned;
  logic                 p2_threshold_q;
  logic                 p2_returned_q;
  logic [MEM_AW-1:0]    p2_wptr;
  logic [47:0]          p2_ts;

  wire signed [ABSW-1:0] p0_delta =
      mca_pkg::polarity_delta(p0_data, cfg_baseline_q, active_polarity_q);
  wire [ABSW-1:0] p0_magnitude = mca_pkg::polarity_magnitude(p0_delta);

  // ------------------------------------------------------------------
  // Ring buffer write port and valid-history bookkeeping
  // ------------------------------------------------------------------
  logic [MEM_AW-1:0] write_pointer;
  logic              mem_owned_net;                 // network domain owns it
  logic              mem_owned_ff1, mem_owned_ff2;  // two-flop synchroniser
  logic [31:0]       history_count;

  wire write_enable = !mem_owned_ff2;

  function automatic logic [15:0] pack_sample(input logic [ADC_WIDTH-1:0] code,
                                              input logic overrange);
    pack_sample = {1'b0, overrange, code};
  endfunction

  // Read-side interface of the record store.
  logic [MEM_AW-1:0] read_address;
  logic [15:0]       mem_dout;

`ifdef MCA_NO_XPM
  logic [15:0] wave_mem [0:MEM_DEPTH-1];
  always_ff @(posedge dco_clk) begin
    if (write_enable)
      wave_mem[write_pointer] <= pack_sample(p0_data, p0_or);
  end
  always_ff @(posedge gmii_clk) begin
    mem_dout <= wave_mem[read_address];
  end
`else
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(MEM_AW),
    .ADDR_WIDTH_B(MEM_AW),
    .AUTO_SLEEP_TIME(0),
    .BYTE_WRITE_WIDTH_A(16),
    .CLOCKING_MODE("independent_clock"),
    .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"),
    .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"),
    .MEMORY_PRIMITIVE("block"),
    .MEMORY_SIZE(MEM_DEPTH * 16),
    .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(16),
    .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"),
    .USE_EMBEDDED_CONSTRAINT(0),
    .USE_MEM_INIT(0),
    .WAKEUP_TIME("disable_sleep"),
    .WRITE_DATA_WIDTH_A(16),
    .WRITE_MODE_B("no_change")
  ) u_wave_bram (
    .dbiterrb(),
    .doutb(mem_dout),
    .sbiterrb(),
    .addra(write_pointer),
    .addrb(read_address),
    .clka(dco_clk),
    .clkb(gmii_clk),
    .dina(pack_sample(p0_data, p0_or)),
    .ena(write_enable),
    .enb(1'b1),
    .injectdbiterra(1'b0),
    .injectsbiterra(1'b0),
    .regceb(1'b1),
    .rstb(~rst_net_n),
    .sleep(1'b0),
    .wea(write_enable)
  );
`endif

  always_ff @(posedge dco_clk or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      mem_owned_ff1 <= 1'b0;
      mem_owned_ff2 <= 1'b0;
    end else begin
      mem_owned_ff1 <= mem_owned_net;
      mem_owned_ff2 <= mem_owned_ff1;
    end
  end

  // ------------------------------------------------------------------
  // Capture state machine
  // ------------------------------------------------------------------
  typedef enum logic [2:0] {
    W_IDLE,     // stopped
    W_FILL,     // accumulating valid history
    W_ARMED,    // history complete, waiting for a trigger
    W_POST,     // writing the post-trigger window
    W_HANDOFF,  // waiting for the network domain to take ownership
    W_UPLOAD,   // network domain streams fragments
    W_DONE      // single-shot capture finished
  } wave_state_t;

  wave_state_t wave_state;

  logic [MEM_AW-1:0] trigger_address;
  logic [15:0]       post_remaining;
  logic [47:0]       trigger_timestamp_q;
  logic [13:0]       trigger_baseline_q;
  logic [13:0]       trigger_threshold_q;
  logic [13:0]       trigger_return_q;
  logic              trigger_negative_q;
  logic [31:0]       trigger_version_q;
  logic [31:0]       capture_id_q;
  logic [31:0]       capture_count_q;
  logic [31:0]       fragment_drop_q;
  logic              armed_q;
  logic              rearmed_q;
  logic              threshold_hit_q;
  logic [47:0]       ts_counter;

  // Stable descriptor plus request/acknowledge toggle pair.
  logic [MEM_AW-1:0] desc_start_address;
  logic [31:0]       desc_capture_id;
  logic [47:0]       desc_timestamp;
  logic [13:0]       desc_baseline;
  logic [13:0]       desc_threshold;
  logic [13:0]       desc_return;
  logic              desc_negative;
  logic [31:0]       desc_config_version;
  logic [31:0]       desc_session_id;
  logic              desc_req_toggle;
  logic              desc_ack_toggle;
  (* ASYNC_REG = "TRUE" *) logic desc_ack_ff1, desc_ack_ff2;

  wire engine_running = run_enable && !holdoff && !abort;
  // The trigger is only allowed once the whole record is backed by samples
  // that were really written, the baseline estimator is ready and the
  // discriminator has re-armed.
  wire pre_window_ready = (history_count >= TOTAL_U);

  wire trigger_now = (wave_state == W_ARMED) && p2_run && armed_q &&
                     p2_threshold && !p2_threshold_q && rearmed_q &&
                     cfg_baseline_ready_q;
  wire pileup_now  = (wave_state == W_POST) && p2_run && rearmed_q &&
                     p2_threshold && !p2_threshold_q;

  always_ff @(posedge dco_clk or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      active_threshold_q  <= ADC_WIDTH'(64);
      active_return_q     <= ADC_WIDTH'(32);
      cfg_baseline_q      <= ADC_WIDTH'(BASELINE_INIT);
      active_polarity_q   <= 1'b0;
      cfg_baseline_ready_q<= 1'b0;
      active_version_q    <= 32'h0001_0000;
      active_session_q    <= 32'd0;
      active_single_q     <= 1'b0;

      p0_data <= '0;
      p0_or   <= 1'b0;
      p0_run  <= 1'b0;
      p0_hold <= 1'b0;
      p0_wptr <= '0;
      p0_ts   <= '0;
      p1_negative <= 1'b0;
      p1_or       <= 1'b0;
      p1_run      <= 1'b0;
      p1_hold     <= 1'b0;
      p1_wptr     <= '0;
      p1_ts       <= '0;
      p1_threshold<= 1'b0;
      p1_returned <= 1'b1;
      p2_negative <= 1'b0;
      p2_or       <= 1'b0;
      p2_run      <= 1'b0;
      p2_threshold<= 1'b0;
      p2_returned <= 1'b1;
      p2_threshold_q <= 1'b0;
      p2_returned_q  <= 1'b1;
      p2_wptr     <= '0;
      p2_ts       <= '0;

      write_pointer  <= '0;
      history_count  <= '0;
      wave_state     <= W_IDLE;
      trigger_address<= '0;
      post_remaining <= '0;
      trigger_timestamp_q <= '0;
      trigger_baseline_q  <= ADC_WIDTH'(BASELINE_INIT);
      trigger_threshold_q <= ADC_WIDTH'(64);
      trigger_return_q    <= ADC_WIDTH'(32);
      trigger_negative_q  <= 1'b0;
      trigger_version_q   <= 32'h0001_0000;
      capture_id_q        <= 32'd0;
      capture_count_q     <= 32'd0;
      fragment_drop_q     <= 32'd0;
      armed_q             <= 1'b1;
      rearmed_q           <= 1'b1;
      threshold_hit_q     <= 1'b0;
      ts_counter          <= '0;

      desc_start_address  <= '0;
      desc_capture_id     <= 32'd0;
      desc_timestamp      <= '0;
      desc_baseline       <= ADC_WIDTH'(BASELINE_INIT);
      desc_threshold      <= ADC_WIDTH'(64);
      desc_return         <= ADC_WIDTH'(32);
      desc_negative       <= 1'b0;
      desc_config_version <= 32'h0001_0000;
      desc_session_id     <= 32'd0;
      desc_req_toggle     <= 1'b0;
      desc_ack_ff1        <= 1'b0;
      desc_ack_ff2        <= 1'b0;
    end else begin
      // ---------------- sample pipeline ----------------
      p0_data <= adc_data;
      p0_or   <= adc_overrange;
      p0_run  <= engine_running;
      p0_hold <= holdoff || abort;
      p0_wptr <= write_pointer;
      p0_ts   <= ts_counter;

      p1_negative  <= active_polarity_q;
      p1_or        <= p0_or;
      p1_run       <= p0_run;
      p1_hold      <= p0_hold;
      p1_wptr      <= p0_wptr;
      p1_ts        <= p0_ts;
      p1_threshold <= !p0_delta[ABSW-1] &&
                      (p0_magnitude >= {1'b0, active_threshold_q});
      p1_returned  <= p0_delta[ABSW-1] ||
                      (p0_magnitude <= {1'b0, active_return_q});

      p2_negative  <= p1_negative;
      p2_or        <= p1_or;
      p2_run       <= p1_run;
      p2_threshold <= p1_threshold;
      p2_returned  <= p1_returned;
      p2_wptr      <= p1_wptr;
      p2_ts        <= p1_ts;
      p2_threshold_q <= p2_threshold;
      p2_returned_q  <= p2_returned;

      ts_counter <= ts_counter + 48'd1;

      // ---------------- memory write pointer ----------------
      if (write_enable) begin
        write_pointer <= write_pointer + 1'b1;
        if (history_count <= MEM_DEPTH[31:0])
          history_count <= history_count + 32'd1;
      end

      // ---------------- baseline and threshold freeze ----------------
      if (wave_state == W_FILL || wave_state == W_ARMED) begin
        cfg_baseline_q       <= cfg_baseline;
        cfg_baseline_ready_q <= cfg_baseline_ready;
      end

      // ---------------- atomic configuration ----------------
      // The waveform configuration is only replaced while the engine is idle,
      // and the top level only asserts `config_apply` when acquisition has
      // been stopped, so a running capture can never be retroactively changed.
      if (config_apply && (wave_state == W_IDLE || wave_state == W_DONE)) begin
        active_threshold_q <= (cfg_threshold == 0) ? ADC_WIDTH'(1)
                                                   : cfg_threshold;
        active_return_q    <= cfg_return;
        active_polarity_q  <= cfg_polarity_negative;
        active_version_q   <= cfg_config_version;
        active_session_q   <= cfg_session_id;
        active_single_q    <= cfg_single_shot;
        // A new configuration always restarts the pre-window so that no
        // sample of an old session can leak into a new one.
        history_count      <= '0;
        cfg_baseline_q     <= cfg_baseline;
        cfg_baseline_ready_q <= cfg_baseline_ready;
        armed_q            <= 1'b1;
        rearmed_q          <= 1'b1;
        threshold_hit_q    <= 1'b0;
      end

      // ---------------- holdoff, abort and run control ----------------
      if (holdoff || abort) begin
        // A range change, a manual pulse or an explicit abort invalidates the
        // history: the new-range plateau must not be mistaken for a pulse, and
        // an unfinished capture must never be published.
        history_count   <= '0;
        armed_q         <= 1'b1;
        rearmed_q       <= 1'b1;
        threshold_hit_q <= 1'b0;
        if (wave_state == W_POST || wave_state == W_ARMED ||
            wave_state == W_FILL || wave_state == W_DONE)
          wave_state <= run_enable ? W_FILL : W_IDLE;
      end else begin
        unique case (wave_state)
          W_IDLE: begin
            if (run_enable) begin
              history_count <= '0;
              wave_state    <= W_FILL;
            end
          end

          W_FILL: begin
            if (!run_enable)
              wave_state <= W_IDLE;
            else if (pre_window_ready && cfg_baseline_ready_q)
              wave_state <= W_ARMED;
          end

          W_ARMED: begin
            if (!run_enable) begin
              wave_state <= W_IDLE;
            end else if (trigger_now) begin
              // p0_data is written by the BRAM port on the following clock,
              // when write_pointer has advanced once beyond p0_wptr.
              // Store the physical RAM address so T lands exactly at the
              // documented PRE_SAMPLES index during read-out.
              trigger_address     <= p2_wptr + 1'b1;
              trigger_timestamp_q <= p2_ts;
              trigger_baseline_q  <= cfg_baseline_q;
              trigger_threshold_q <= active_threshold_q;
              trigger_return_q    <= active_return_q;
              trigger_negative_q  <= p2_negative;
              trigger_version_q   <= active_version_q;
              capture_id_q        <= capture_id_q + 32'd1;
              post_remaining      <= POST_COUNT_U - 16'd1;
              armed_q             <= 1'b0;
              rearmed_q           <= 1'b0;
              // The post window is always written out in full even if the
              // host stops in the meantime: an accepted capture is finished
              // and uploaded, and stopping only prevents new triggers.
              wave_state          <= W_POST;
            end
          end

          W_POST: begin
            // Pulses that arrive during the post window are kept as part of
            // the raw record, but they are not queued and they are counted
            // separately from the spectrum counters.
            if (pileup_now)
              fragment_drop_q <= fragment_drop_q + 32'd1;
            if (post_remaining == 16'd0) begin
              desc_start_address  <= trigger_address - PRE_OFFSET;
              desc_capture_id     <= capture_id_q;
              desc_timestamp      <= trigger_timestamp_q;
              desc_baseline       <= trigger_baseline_q;
              desc_threshold      <= trigger_threshold_q;
              desc_return         <= trigger_return_q;
              desc_negative       <= trigger_negative_q;
              desc_config_version <= trigger_version_q;
              desc_session_id     <= active_session_q;
              desc_req_toggle     <= ~desc_req_toggle;
              wave_state          <= W_HANDOFF;
            end else begin
              post_remaining <= post_remaining - 16'd1;
            end
          end

          W_HANDOFF: begin
            // Nothing else changes while the handshake is open: the
            // descriptor bus stays frozen for the whole exchange.
          end

          W_UPLOAD: begin
            // Write side is parked because the network domain owns the RAM.
          end

          W_DONE: begin
            if (!run_enable)
              wave_state <= W_IDLE;
          end

          default: wave_state <= W_IDLE;
        endcase
      end

      // ---------------- ownership release ----------------
      // The acknowledgement toggle equals the request toggle only after the
      // network side has transmitted (or cancelled) the final fragment.  The
      // old mismatch test released ownership immediately after the request
      // and then could never leave W_UPLOAD when the real acknowledgement
      // arrived.
      desc_ack_ff1 <= desc_ack_toggle;
      desc_ack_ff2 <= desc_ack_ff1;
      if ((wave_state == W_HANDOFF || wave_state == W_UPLOAD) &&
          (desc_ack_ff2 == desc_req_toggle)) begin
        capture_count_q <= capture_count_q + 32'd1;
        wave_state      <= active_single_q ? W_DONE : W_FILL;
        // Continuous mode restarts the pre-window after every upload.
        if (!active_single_q)
          history_count <= '0;
      end

      // ---------------- return-to-threshold re-arm ----------------
      if (p2_returned)
        armed_q <= 1'b1;
      else if (trigger_now)
        armed_q <= 1'b0;

      if (p2_returned)
        rearmed_q <= 1'b1;
      else if (p2_threshold)
        rearmed_q <= 1'b0;

      threshold_hit_q <= p2_threshold;
    end
  end

  assign timestamp           = ts_counter;
  assign active_baseline     = cfg_baseline_q;
  assign capture_id          = capture_id_q;
  assign active_threshold_out= active_threshold_q;
  assign active_polarity_out = active_polarity_q;

  // ==================================================================
  // Network domain
  // ==================================================================

  // Descriptor captured in the network domain.
  logic [MEM_AW-1:0] nd_start_address;
  logic [31:0]       nd_capture_id;
  logic [47:0]       nd_timestamp;
  logic [13:0]       nd_baseline;
  logic [13:0]       nd_threshold;
  logic [13:0]       nd_return;
  logic              nd_negative;
  logic [31:0]       nd_config_version;
  logic [31:0]       nd_session_id;

  (* ASYNC_REG = "TRUE" *) logic desc_req_ff1, desc_req_ff2;
  logic desc_req_seen;

  // Status publication from the sampling domain.
  logic [31:0] status_value_dco, status_value_q;
  logic        status_toggle_dco;
  (* ASYNC_REG = "TRUE" *) logic status_toggle_ff1, status_toggle_ff2;
  logic        status_toggle_seen;

  // ------------------------------------------------------------------
  // Status publication.  The state code is the only multi-bit value the
  // network domain consumes; the capture counters stay in the sampling domain
  // and are published to the control domain by the top-level snapshot.
  // ------------------------------------------------------------------
  logic [31:0] status_code;
  always_comb begin
    unique case (wave_state)
      W_IDLE:    status_code = ST_STOPPED;
      W_FILL:    status_code = ST_FILLING;
      W_ARMED:   status_code = ST_WAITING;
      W_POST:    status_code = ST_CAPTURING;
      W_HANDOFF: status_code = ST_UPLOADING;
      W_UPLOAD:  status_code = ST_UPLOADING;
      W_DONE:    status_code = ST_SINGLE;
      default:   status_code = ST_STOPPED;
    endcase
  end

  logic [15:0] status_refresh;
  always_ff @(posedge dco_clk or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      status_toggle_dco <= 1'b0;
      status_value_dco  <= ST_STOPPED;
      status_refresh    <= '0;
    end else if ((status_code != status_value_dco) ||
                 (status_refresh == 16'hFFFF)) begin
      // Publish on every change and refresh a static state periodically, so a
      // lost toggle can never leave the network domain with a stale value.
      status_toggle_dco <= ~status_toggle_dco;
      status_value_dco  <= status_code;
      status_refresh    <= '0;
    end else begin
      status_refresh <= status_refresh + 1'b1;
    end
  end

  always_ff @(posedge gmii_clk or negedge rst_net_n) begin
    if (!rst_net_n) begin
      status_toggle_ff1 <= 1'b0;
      status_toggle_ff2 <= 1'b0;
      status_toggle_seen<= 1'b0;
      status_value_q    <= ST_STOPPED;
    end else begin
      status_toggle_ff1 <= status_toggle_dco;
      status_toggle_ff2 <= status_toggle_ff1;
      if (status_toggle_ff2 != status_toggle_seen) begin
        status_toggle_seen <= status_toggle_ff2;
        status_value_q     <= status_value_dco;
      end
    end
  end

  // ------------------------------------------------------------------
  // Descriptor handshake and fragment read-out
  // ------------------------------------------------------------------
  typedef enum logic [1:0] { R_IDLE, R_LOAD, R_READY } read_state_t;
  read_state_t read_state;

  logic [15:0] frag_index_q;
  logic        frag_last_q;
  logic [MEM_AW-1:0] frag_base_address;
  logic        desc_accepted;

  function automatic logic [15:0] fragment_samples(input logic [15:0] index);
    fragment_samples = (index == (FRAG_COUNT_U - 16'd1)) ? LAST_SAMPLES_U
                                                         : FRAG_MAX_U;
  endfunction

  always_ff @(posedge gmii_clk or negedge rst_net_n) begin
    if (!rst_net_n) begin
      desc_req_ff1 <= 1'b0;
      desc_req_ff2 <= 1'b0;
      desc_req_seen<= 1'b0;
      desc_ack_toggle <= 1'b0;
      read_state   <= R_IDLE;
      frag_index_q <= 16'd0;
      frag_last_q  <= 1'b0;
      frag_base_address <= '0;
      nd_start_address  <= '0;
      nd_capture_id     <= 32'd0;
      nd_timestamp      <= '0;
      nd_baseline       <= ADC_WIDTH'(BASELINE_INIT);
      nd_threshold      <= ADC_WIDTH'(64);
      nd_return         <= ADC_WIDTH'(32);
      nd_negative       <= 1'b0;
      nd_config_version <= 32'h0001_0000;
      nd_session_id     <= 32'd0;
      mem_owned_net     <= 1'b0;
      desc_accepted     <= 1'b0;
    end else begin
      desc_req_ff1 <= desc_req_toggle;
      desc_req_ff2 <= desc_req_ff1;
      desc_accepted <= 1'b0;

      if (desc_req_ff2 != desc_req_seen) begin
        // The descriptor bus has been stable since the request toggle moved,
        // so the whole record can be captured in a single cycle.
        desc_req_seen     <= desc_req_ff2;
        desc_accepted     <= 1'b1;
        nd_start_address  <= desc_start_address;
        nd_capture_id     <= desc_capture_id;
        nd_timestamp      <= desc_timestamp;
        nd_baseline       <= desc_baseline;
        nd_threshold      <= desc_threshold;
        nd_return         <= desc_return;
        nd_negative       <= desc_negative;
        nd_config_version <= desc_config_version;
        nd_session_id     <= desc_session_id;
        frag_index_q      <= 16'd0;
        frag_last_q       <= (FRAG_COUNT_U == 16'd1);
        frag_base_address <= desc_start_address;
        mem_owned_net     <= 1'b1;
        read_state        <= R_READY;
      end

      // `desc_accepted` excludes the cycle in which a new descriptor was
      // captured, so a fresh handshake can never be advanced by a stale
      // acknowledgement.
      if (read_state == R_READY && frag_ack && !desc_accepted) begin
        if (frag_last_q) begin
          read_state      <= R_IDLE;
          mem_owned_net   <= 1'b0;
          desc_ack_toggle <= ~desc_ack_toggle;
        end else begin
          frag_index_q      <= frag_index_q + 16'd1;
          frag_last_q       <= (frag_index_q + 16'd1) == (FRAG_COUNT_U - 16'd1);
          frag_base_address <= nd_start_address +
              ((frag_index_q + 16'd1) * FRAG_MAX_U);
          // Drop frag_valid for one cycle between descriptors.  The consumer
          // holds its acknowledgement until valid is released, so keeping
          // R_READY asserted here deadlocked the stream after fragment zero.
          read_state          <= R_LOAD;
        end
      end

      if (read_state == R_LOAD)
        read_state <= R_READY;

      // Do not clear ownership merely because the *old* state was R_IDLE.
      // On the descriptor-accept cycle nonblocking assignments still expose
      // R_IDLE here; the former unconditional clear overrode the assertion
      // above and let the DCO writer overwrite the record during upload.
      // Ownership is released only by the final-fragment acknowledgement.
    end
  end

  assign read_address = frag_base_address + frag_word_index;

  typedef enum logic [1:0] { FR_IDLE, FR_WAIT, FR_RELEASE } frag_read_state_t;
  frag_read_state_t frag_read_state;
  always_ff @(posedge gmii_clk or negedge rst_net_n) begin
    if (!rst_net_n) begin
      frag_word       <= 16'd0;
      frag_word_valid <= 1'b0;
      frag_read_state <= FR_IDLE;
    end else begin
      frag_word_valid <= 1'b0;
      unique case (frag_read_state)
        FR_IDLE: begin
          if (frag_word_req)
            // The RAM address is already driven by frag_word_index.  Wait a
            // full clock for its registered read port before sampling data.
            frag_read_state <= FR_WAIT;
        end
        FR_WAIT: begin
          frag_word       <= mem_dout;
          frag_word_valid <= 1'b1;
          frag_read_state <= FR_RELEASE;
        end
        FR_RELEASE: begin
          // The requester holds req until valid.  Do not interpret that held
          // level as the next word request; re-arm only after it releases.
          if (!frag_word_req)
            frag_read_state <= FR_IDLE;
        end
        default: frag_read_state <= FR_IDLE;
      endcase
    end
  end

  assign frag_valid          = (read_state == R_READY);
  assign frag_index          = frag_index_q;
  assign frag_sample_count   = fragment_samples(frag_index_q);
  assign frag_sample_offset  = frag_index_q * FRAG_MAX_U;
  assign frag_fragment_count = FRAG_COUNT_U;
  assign frag_last           = frag_last_q;
  assign frag_capture_id     = nd_capture_id;
  assign frag_timestamp      = nd_timestamp;
  assign frag_baseline       = nd_baseline;
  assign frag_threshold      = nd_threshold;
  assign frag_return         = nd_return;
  assign frag_negative       = nd_negative;
  assign frag_config_version = nd_config_version;
  assign frag_session_id     = nd_session_id;

  assign status_snapshot    = status_value_q;
  assign capture_count_snapshot = capture_count_q;
  assign last_capture_snapshot  = nd_capture_id;
endmodule
