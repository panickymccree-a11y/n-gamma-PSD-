`timescale 1ns/1ps

// Single-channel AD9246 MCA board top.
//
// Clock domains
//   clk_125m_sys : clock wizard output, control and configuration
//   DCO125M      : ADC output clock, one sample per cycle (8 ns)
//   gmii_clk     : RGMII receive clock, Ethernet/UDP and packet assembly
//
// Reset
//   There is no physical reset pin.  The clock wizard lock is the internal
//   reset source and every domain releases through its own synchronizer.
module single_channel_mca #(
  parameter logic [47:0] LOCAL_MAC = 48'h02_12_34_56_78_9A,
  parameter logic [47:0] HOST_MAC  = 48'h02_00_00_00_00_01,
  parameter logic [31:0] LOCAL_IP  = 32'hC0A8_010A,
  parameter logic [31:0] HOST_IP   = 32'hC0A8_0164,
  parameter int UDP_DATA_PORT      = 9000,   // events, status and responses
  parameter int UDP_CONTROL_PORT   = 9001,   // MCF2 requests
  parameter int PHY_RESET_CYCLES   = 1_250_000,
  parameter int PULSE_CYCLES       = 2_500_000,
  parameter int STATUS_INTERVAL_CYCLES = 12_500_000, // 100 ms at 125 MHz
  parameter int EVENT_BATCH_MAX    = 64,
  parameter int UART_SPECTRUM_BINS = 4096
) (
  input  logic         clk_125m,
  input  logic [13:0]  adc_data,
  input  logic         DCO125M,
  input  logic         adc_or,
  output logic         adc_sclk,
  inout  wire          adc_sdio,
  output logic         adc_csb,
  input  logic         rgmii_rxc,
  input  logic         rgmii_rx_ctl,
  input  logic [3:0]   rgmii_rxd,
  output logic         rgmii_txc,
  output logic         rgmii_tx_ctl,
  output logic [3:0]   rgmii_txd,
  output logic         phy_rst_n,
  output logic         phy_mdc,
  inout  wire          phy_mdio,
  input  logic         ex_tri,
  input  logic         uart_rxd,
  output logic         uart_txd,
  output logic         Q1,
  output logic         Q2
);
  localparam logic [15:0] LEN_BADREQ = 13;
  localparam int STATUS_BYTES = 134;
  localparam int STATUS_W = STATUS_BYTES * 8;
  // Recovery qualification is fixed in the sampling datapath. Keep the
  // legacy configuration field on the control bus for compatibility, but do
  // not let it feed the 125 MHz baseline arithmetic.
  localparam logic [15:0] BASELINE_RECOVERY_SAMPLES_FIXED = 16'd1024;

  logic clk_125m_sys;
  logic clk_200m;
  logic clk_locked;
  logic rst_n;
  logic phy_reset_done;
  logic rst_clk_n, rst_dco_n, rst_net_n;
  logic gmii_clk;

  logic        frontend_filter_enabled_clk;
  logic        frontend_pipeline_valid_clk;
  logic        frontend_pipeline_valid;
  logic [13:0] frontend_sample;
  logic        frontend_overrange;
  logic [47:0] frontend_timestamp;
  // One local DCO register separates the front-end output mux from every
  // sample consumer.  Valid/overrange travel with the sample so the added
  // cycle cannot create a data/handshake skew.
  logic        frontend_pipeline_valid_dco_q;
  logic [13:0] frontend_sample_dco_q;
  logic        frontend_overrange_dco_q;
  logic [47:0] frontend_timestamp_dco_q;
  logic [47:0] adc_timestamp_dco;
  logic        init_done;

  mca_clk_wiz u_clocking (
    .clk_out1 (clk_125m_sys),
    .clk_out2 (clk_200m),
    .locked   (clk_locked),
    .clk_in1  (clk_125m)
  );
  assign rst_n = clk_locked;

  mca_reset_sync u_rst_clk (.clk(clk_125m_sys), .arst_n(rst_n), .srst_n(rst_clk_n));
  mca_reset_sync u_rst_dco (.clk(DCO125M),     .arst_n(rst_n), .srst_n(rst_dco_n));
  mca_reset_sync u_rst_net (.clk(gmii_clk),    .arst_n(rst_n), .srst_n(rst_net_n));

  mca_phy_reset #(.RESET_CYCLES(PHY_RESET_CYCLES)) u_phy_reset (
    .clk(clk_125m_sys), .rst_n(rst_clk_n), .phy_rst_n(phy_rst_n),
    .done(phy_reset_done)
  );

  logic [4:0] mdc_count;
  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      mdc_count <= '0;
      phy_mdc   <= 1'b0;
    end else if (mdc_count == 5'd24) begin
      mdc_count <= '0;
      phy_mdc   <= ~phy_mdc;
    end else begin
      mdc_count <= mdc_count + 1'b1;
    end
  end
  assign phy_mdio = 1'bz;
  // ADC SPI is intentionally not implemented in this phase. The board pins
  // remain present and are held inactive so the Excel constraints are valid.
  always_comb begin
    adc_sclk = 1'b0;
    adc_csb  = 1'b1;
  end
  assign adc_sdio = 1'bz;

  // -----------------------------------------------------------------
  // Manual pulse and range control (125 MHz domain)
  // -----------------------------------------------------------------
  logic manual_q1, manual_q2;
  logic range_pulse_q1, range_pulse_q2;
  logic [1:0] ctrl_range_value;
  logic ctrl_range_strobe;
  logic [1:0] range_request_value;
  logic range_request_strobe;
  logic [1:0] range_pending_value;
  logic [1:0] range_applied_value_clk;
  logic range_pending, range_seen_busy, range_busy;
  logic run_enable_dco;
  (* ASYNC_REG = "TRUE" *) logic run_control_ff1, run_control_ff2;
  // Range/control decisions must use a control-domain level, not raw DCO state.
  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      run_control_ff1<=1'b0;run_control_ff2<=1'b0;
    end else begin
      run_control_ff1<=run_enable_dco;run_control_ff2<=run_control_ff1;
    end
  end
  logic init_done_prev;
  wire system_ready = clk_locked & phy_reset_done;

  // EXTRI is intentionally separate from the range request path; it is
  // consumed only by mca_external_trigger in the external-trigger mode.
  assign range_request_value  = ctrl_range_value;
  assign range_request_strobe = ctrl_range_strobe;
  assign range_pulse_q1 = range_request_strobe && !range_busy &&
                          (range_request_value == 2'b01);
  assign range_pulse_q2 = range_request_strobe && !range_busy &&
                          (range_request_value == 2'b10);
  assign manual_q1 = range_pulse_q1;
  assign manual_q2 = range_pulse_q2;

  logic pulse_busy;
  logic pulse_pause_request;
  (* ASYNC_REG = "TRUE" *) logic dco_pause_req_ff1, dco_pause_req_ff2;
  logic dco_event_active;
  logic event_active_ctrl_dco;
  logic dco_pause_ack;
  (* ASYNC_REG = "TRUE" *) logic clk_pause_ack_ff1, clk_pause_ack_ff2;

  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      dco_pause_req_ff1 <= 1'b0;
      dco_pause_req_ff2 <= 1'b0;
      dco_pause_ack     <= 1'b0;
    end else begin
      dco_pause_req_ff1 <= pulse_pause_request;
      dco_pause_req_ff2 <= dco_pause_req_ff1;
      // Keep the pause handshake on the current acquisition-idle indication;
      // unlike configuration commit, the startup range pulse relies on this
      // edge-accurate acknowledgement.
      dco_pause_ack     <= dco_pause_req_ff2 && !dco_event_active;
    end
  end
  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      clk_pause_ack_ff1 <= 1'b0;
      clk_pause_ack_ff2 <= 1'b0;
    end else begin
      clk_pause_ack_ff1 <= dco_pause_ack;
      clk_pause_ack_ff2 <= clk_pause_ack_ff1;
    end
  end

  mca_pulse_controller #(.PULSE_CYCLES(PULSE_CYCLES)) u_pulse (
    .clk(clk_125m_sys), .rst_n(rst_clk_n),
    .system_ready(system_ready),
    .run_request(1'b0), .manual_q1(manual_q1), .manual_q2(manual_q2),
    .dco_pause_ack(clk_pause_ack_ff2),
    .q1(Q1), .q2(Q2), .busy(pulse_busy), .pause_request(pulse_pause_request),
    .init_done(init_done), .actual_q1(), .actual_q2()
  );

  assign range_busy = !system_ready | pulse_busy | run_control_ff2;

  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      range_pending_value     <= 2'b00;
      range_applied_value_clk <= 2'b00;
      range_pending           <= 1'b0;
      range_seen_busy         <= 1'b0;
      init_done_prev          <= 1'b0;
    end else begin
      init_done_prev <= init_done;
      if (init_done && !init_done_prev)
        range_applied_value_clk <= 2'b10;
      if (range_request_strobe && !range_busy &&
          ((range_request_value == 2'b01) ||
           (range_request_value == 2'b10))) begin
        range_pending_value <= range_request_value;
        range_pending       <= 1'b1;
        range_seen_busy     <= 1'b0;
      end
      if (range_pending && pulse_busy)
        range_seen_busy <= 1'b1;
      if (range_pending && range_seen_busy && !pulse_busy) begin
        range_applied_value_clk <= range_pending_value;
        range_pending           <= 1'b0;
        range_seen_busy         <= 1'b0;
      end
    end
  end

  // The range pulse pauses the sampling domain.  When the pause releases the
  // baseline estimator restarts so the new range is measured from scratch.
  logic pause_dco_prev;
  wire  range_change_restart = pause_dco_prev & ~dco_pause_req_ff2;

  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      pause_dco_prev <= 1'b0;
    end else begin
      pause_dco_prev <= dco_pause_req_ff2;
    end
  end

  // -----------------------------------------------------------------
  // Command and response FIFOs
  // -----------------------------------------------------------------
  logic cmd_wr_net;
  logic [511:0] cmd_payload_net;
  logic [7:0] cmd_len_net;
  logic cmd_fifo_full, cmd_fifo_empty, cmd_fifo_rd_en;
  logic [519:0] cmd_fifo_dout;
  logic [4:0] cmd_wr_count, cmd_rd_count;
  logic cmd_wr_busy, cmd_rd_busy;
  mca_async_fifo #(.WIDTH(520), .ADDR_WIDTH(4)) u_cmd_fifo (
    .rst_n(rst_n), .wr_clk(gmii_clk), .wr_en(cmd_wr_net),
    .din({cmd_len_net, cmd_payload_net}), .full(cmd_fifo_full),
    .wr_data_count(cmd_wr_count), .wr_rst_busy(cmd_wr_busy),
    .rd_clk(clk_125m_sys), .rd_en(cmd_fifo_rd_en), .dout(cmd_fifo_dout),
    .empty(cmd_fifo_empty), .rd_data_count(cmd_rd_count), .rd_rst_busy(cmd_rd_busy)
  );

  logic resp_fifo_full, resp_fifo_empty, resp_fifo_wr_en, resp_fifo_rd_en;
  logic [519:0] resp_fifo_din, resp_fifo_dout;
  logic [4:0] resp_wr_count, resp_rd_count;
  logic resp_wr_busy, resp_rd_busy;
  mca_async_fifo #(.WIDTH(520), .ADDR_WIDTH(4)) u_resp_fifo (
    .rst_n(rst_n), .wr_clk(clk_125m_sys), .wr_en(resp_fifo_wr_en),
    .din(resp_fifo_din), .full(resp_fifo_full), .wr_data_count(resp_wr_count),
    .wr_rst_busy(resp_wr_busy), .rd_clk(gmii_clk), .rd_en(resp_fifo_rd_en),
    .dout(resp_fifo_dout), .empty(resp_fifo_empty),
    .rd_data_count(resp_rd_count), .rd_rst_busy(resp_rd_busy)
  );

  // -----------------------------------------------------------------
  // MCF2 control
  // -----------------------------------------------------------------
  logic ctrl_cmd_valid, ctrl_cmd_ready;
  logic net_cmd_valid;
  logic uart_cmd_valid, uart_cmd_ready;
  logic [511:0] uart_cmd_payload;
  logic [7:0] uart_cmd_len;
  logic ctrl_cmd_source_uart;
  logic inflight_source_uart;
  assign cmd_fifo_rd_en = !cmd_fifo_empty && !cmd_rd_busy &&
                          !net_cmd_valid && !uart_cmd_valid && ctrl_cmd_ready;
  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      net_cmd_valid       <= 1'b0;
      inflight_source_uart<= 1'b0;
    end else begin
      if (cmd_fifo_rd_en)
        net_cmd_valid <= 1'b1;
      else if (net_cmd_valid && ctrl_cmd_ready && !uart_cmd_valid)
        net_cmd_valid <= 1'b0;
      if (ctrl_cmd_valid && ctrl_cmd_ready)
        inflight_source_uart <= ctrl_cmd_source_uart;
    end
  end
  assign ctrl_cmd_valid = uart_cmd_valid | net_cmd_valid;
  assign ctrl_cmd_source_uart = uart_cmd_valid;
  assign uart_cmd_ready = ctrl_cmd_ready;

  logic ctrl_resp_valid, ctrl_resp_ready;
  logic [519:0] ctrl_resp_payload;
  logic [7:0] ctrl_resp_len;
  logic [275:0] cfg_bus_clk;
  logic cfg_toggle_clk;
  logic ctrl_commit_wait;
  logic [31:0] snap_config_version_dco;

  // Snapshot values returned by the sampling domain.  Every multi-bit field
  // is only sampled while its toggle is stable.
  logic [13:0] snap_baseline_clk;
  logic [1:0]  snap_state_clk;
  logic        snap_ready_clk;
  logic        snap_auto_clk;
  logic [13:0] snap_manual_clk;
  logic [31:0] snap_config_version_clk;
  logic [31:0] snap_event_count_clk;
  logic [31:0] snap_rejected_count_clk;
  logic [31:0] snap_dropped_count_clk;
  logic [47:0] snap_timestamp_clk;
  logic        snap_event_active_clk;
  logic        snap_external_armed_clk;
  logic        snap_external_window_clk;
  logic [31:0] snap_external_count_clk;
  logic [31:0] snap_wave_status_clk;
  logic [31:0] snap_wave_capture_clk;
  logic [31:0] snap_wave_count_clk;
  logic        snap_running_clk;
  logic [1:0]  snap_actual_range_clk;
  logic        snap_range_busy_clk;
  logic        snap_filter_enabled_clk;
  logic        snap_peak_interpolation_clk;
  logic        snap_frontend_init_clk;
  logic [13:0] snap_recovery_tolerance_clk;
  logic [2:0]  snap_mode_clk;
  logic        cfg_ack_clk, cfg_ack_seen;
  logic        cfg_ack_toggle_ff1, cfg_ack_toggle_ff2;
  logic        cfg_apply_pending;
  logic        config_apply_dco;
  logic        window_open;
  logic        ctrl_abort_toggle, ctrl_clear_toggle;

  logic uart_resp_ready;
  assign ctrl_resp_ready = inflight_source_uart ? uart_resp_ready :
                           (!resp_fifo_full && !resp_wr_busy);
  assign resp_fifo_wr_en = ctrl_resp_valid && ctrl_resp_ready &&
                           !inflight_source_uart;
  assign resp_fifo_din   = {ctrl_resp_len, ctrl_resp_payload};

  mca_control #(
    .FIRMWARE_VERSION (32'h0002_0700),
    .SAMPLE_RATE_HZ   (125_000_000),
    .SAMPLE_NS        (8),
    .WINDOW_MIN       (6),
    .WINDOW_MAX       (2500)
  ) u_control (
    .clk(clk_125m_sys), .rst_n(rst_clk_n),
    .cmd_valid(ctrl_cmd_valid),
    .cmd_source(ctrl_cmd_source_uart),
    .cmd_payload(ctrl_cmd_source_uart ? uart_cmd_payload : cmd_fifo_dout[511:0]),
    .cmd_len(ctrl_cmd_source_uart ? uart_cmd_len : cmd_fifo_dout[519:512]),
    .cmd_ready(ctrl_cmd_ready),
    .busy(ctrl_commit_wait),
    .resp_valid(ctrl_resp_valid), .resp_payload(ctrl_resp_payload),
    .resp_len(ctrl_resp_len), .resp_ready(ctrl_resp_ready),
    .cfg_bus(cfg_bus_clk), .cfg_toggle(cfg_toggle_clk), .cfg_ack(cfg_ack_clk),
    .commit_ack(cfg_ack_clk),
    .applied_config_version(snap_config_version_clk),
    .applied_baseline(snap_baseline_clk),
    .applied_baseline_state(snap_state_clk),
    .applied_baseline_ready(snap_ready_clk),
    .applied_baseline_auto(snap_auto_clk),
    .applied_manual_baseline(snap_manual_clk),
    .run_enable(run_control_ff2),
    .range_applied_value(range_applied_value_clk),
    .range_busy(range_busy),
     .phy_ready(system_ready),
     .init_done(init_done),
     .q_busy(pulse_busy),
     .filter_enabled(frontend_filter_enabled_clk),
     .peak_interpolation_enabled(snap_peak_interpolation_clk),
     .frontend_ready(frontend_pipeline_valid_clk),
    .event_count(snap_event_count_clk),
    .rejected_count(snap_rejected_count_clk),
    .dropped_count(snap_dropped_count_clk),
    .timestamp(snap_timestamp_clk),
    .event_active(snap_event_active_clk),
    .baseline_ready_in(snap_ready_clk),
    .external_armed(snap_external_armed_clk),
    .external_window_active(snap_external_window_clk),
    .external_trigger_count(snap_external_count_clk),
    .wave_status(snap_wave_status_clk),
    .wave_last_capture(snap_wave_capture_clk),
    .wave_capture_count(snap_wave_count_clk),
    .range_request_value(ctrl_range_value),
    .range_request_strobe(ctrl_range_strobe),
    .abort_toggle(ctrl_abort_toggle),
    .clear_counts_toggle(ctrl_clear_toggle)
  );

  // -----------------------------------------------------------------
  // Configuration apply in the sampling domain
  // -----------------------------------------------------------------
  logic        cfg_toggle_ff1, cfg_toggle_ff2, cfg_toggle_seen;
  logic        cfg_ack_dco, cfg_ack_toggle_dco;
  logic [31:0] config_version_dco;
  logic [13:0] cfg_threshold_dco;
  logic [13:0] cfg_return_dco;
  logic [11:0] cfg_window_total_dco;
  logic [11:0] cfg_window_pre_dco;
  logic cfg_psd_dco;
  logic [11:0] cfg_psd_start_dco,cfg_psd_end_dco;
  logic [13:0] cfg_guard_dco;
  logic [13:0] cfg_pulse_threshold_dco;
  logic [5:0]  cfg_fast_shift_dco;
  logic [5:0]  cfg_track_shift_dco;
  logic [5:0]  cfg_recovery_shift_dco;
  logic [15:0] cfg_fast_samples_dco;
  logic [15:0] cfg_pulse_hold_dco;
  logic [15:0] cfg_offset_qualify_dco;
  logic [13:0] cfg_recovery_tolerance_dco;
  logic        cfg_filter_enable_dco;
  logic        cfg_peak_interpolation_dco;
  logic        cfg_polarity_negative_dco;
  logic        cfg_wave_mode_dco;
  logic        cfg_external_mode_dco;
  logic [19:0] cfg_external_duration_us_dco;
  logic        cfg_wave_single_dco;
  logic [31:0] cfg_wave_session_dco;
  logic        wave_config_apply;
  logic        abort_dco, clear_dco;
  logic        abort_toggle_dco_ff1, abort_toggle_dco_ff2, abort_toggle_seen;
  logic        clear_toggle_dco_ff1, clear_toggle_dco_ff2, clear_toggle_seen;

  logic [1:0]  baseline_state_dco;
  logic [13:0] baseline_dco;
  logic        baseline_ready_dco;
  // Local DCO copies keep baseline status/control consumers from sharing the
  // estimator's raw output nets. Acquisition uses a delayed ready flag to
  // assert a conservative enable, while the current state still invalidates
  // event acceptance immediately on entry to recovery.
  logic [13:0] baseline_local_dco;
  logic [1:0]  baseline_state_local_dco;
  logic        baseline_ready_local_dco;
  logic        baseline_auto_active_dco;
  logic [13:0] manual_baseline_active_dco;
  logic        baseline_apply_restart;
  logic        baseline_restart_keep_valid;
  logic        external_window_active_dco;
  logic [31:0] external_trigger_count_dco;

  wire external_armed_dco = run_enable_dco &&
                             cfg_external_mode_dco && !cfg_wave_mode_dco;
  // The raw spectrum engine remains clocked for the whole run so an event
  // already in flight can drain after a window expires.  Only trigger
  // qualification is gated by EXTRI.
  wire spectrum_run_dco = run_enable_dco && !cfg_wave_mode_dco;
  wire spectrum_trigger_enable_dco = spectrum_run_dco &&
                                     (!cfg_external_mode_dco ||
                                      external_window_active_dco);

  wire baseline_ready_for_events = baseline_ready_local_dco &&
                                   (baseline_state_dco == 2'd1);

  (* ASYNC_REG = "TRUE" *) logic filter_enable_ff1, filter_enable_ff2;
  (* ASYNC_REG = "TRUE" *) logic frontend_valid_ff1, frontend_valid_ff2;
  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      filter_enable_ff1 <= 1'b0;
      filter_enable_ff2 <= 1'b0;
      frontend_valid_ff1 <= 1'b0;
      frontend_valid_ff2 <= 1'b0;
    end else begin
      filter_enable_ff1 <= cfg_filter_enable_dco;
      filter_enable_ff2 <= filter_enable_ff1;
      // Report readiness only after the local sample/metadata register, so
      // the control-domain handshake describes the data actually consumed by
      // baseline, acquisition and waveform logic.
      frontend_valid_ff1 <= frontend_pipeline_valid_dco_q;
      frontend_valid_ff2 <= frontend_valid_ff1;
    end
  end
  assign frontend_filter_enabled_clk = filter_enable_ff2;
  assign frontend_pipeline_valid_clk = frontend_valid_ff2;

  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      cfg_toggle_ff1              <= 1'b0;
      cfg_toggle_ff2              <= 1'b0;
      cfg_toggle_seen             <= 1'b0;
      cfg_threshold_dco           <= 14'd64;
      cfg_return_dco              <= 14'd32;
      cfg_window_total_dco        <= 12'd32;
      cfg_window_pre_dco          <= 12'd0;
      cfg_psd_dco<=0; cfg_psd_start_dco<=7; cfg_psd_end_dco<=150;
      cfg_guard_dco               <= 14'd16;
      cfg_pulse_threshold_dco     <= 14'd32;
      cfg_fast_shift_dco          <= 6'd4;
      cfg_track_shift_dco         <= 6'd8;
      cfg_recovery_shift_dco      <= 6'd8;
      cfg_fast_samples_dco        <= 16'd1024;
      cfg_pulse_hold_dco          <= 16'd64;
      cfg_offset_qualify_dco      <= 16'd4096;
      cfg_recovery_tolerance_dco  <= 14'd32;
      cfg_filter_enable_dco       <= 1'b0;
      cfg_peak_interpolation_dco  <= 1'b0;
      cfg_polarity_negative_dco   <= 1'b0;
      cfg_wave_mode_dco           <= 1'b0;
      cfg_external_mode_dco       <= 1'b0;
      cfg_external_duration_us_dco<= 20'd1000;
      cfg_wave_single_dco         <= 1'b0;
      cfg_wave_session_dco        <= 32'd0;
      run_enable_dco              <= 1'b0;
      abort_dco                   <= 1'b0;
      clear_dco                   <= 1'b0;
      abort_toggle_dco_ff1        <= 1'b0;
      abort_toggle_dco_ff2        <= 1'b0;
      abort_toggle_seen           <= 1'b0;
      clear_toggle_dco_ff1        <= 1'b0;
      clear_toggle_dco_ff2        <= 1'b0;
      clear_toggle_seen           <= 1'b0;
      baseline_auto_active_dco    <= 1'b0;
      manual_baseline_active_dco  <= 14'd8192;
      baseline_apply_restart      <= 1'b0;
      baseline_restart_keep_valid <= 1'b0;
      cfg_apply_pending           <= 1'b0;
      config_apply_dco            <= 1'b0;
      cfg_ack_dco                 <= 1'b0;
      cfg_ack_toggle_dco          <= 1'b0;
      config_version_dco          <= 32'h0001_0000;
    end else begin
      cfg_toggle_ff1       <= cfg_toggle_clk;
      cfg_toggle_ff2       <= cfg_toggle_ff1;
      abort_toggle_dco_ff1 <= ctrl_abort_toggle;
      abort_toggle_dco_ff2 <= abort_toggle_dco_ff1;
      clear_toggle_dco_ff1 <= ctrl_clear_toggle;
      clear_toggle_dco_ff2 <= clear_toggle_dco_ff1;
      abort_dco            <= 1'b0;
      clear_dco            <= 1'b0;
      baseline_apply_restart <= 1'b0;
      baseline_restart_keep_valid <= 1'b0;
      config_apply_dco     <= 1'b0;
      cfg_ack_dco          <= 1'b0;

      if (abort_toggle_dco_ff2 != abort_toggle_seen) begin
        abort_toggle_seen <= abort_toggle_dco_ff2;
        abort_dco         <= 1'b1;
      end
      if (clear_toggle_dco_ff2 != clear_toggle_seen) begin
        clear_toggle_seen <= clear_toggle_dco_ff2;
        clear_dco         <= 1'b1;
      end

      if ((cfg_toggle_ff2 != cfg_toggle_seen) && !cfg_apply_pending) begin
        cfg_apply_pending <= 1'b1;
        // A configuration transaction must not wait indefinitely for a gap in
        // a high-rate event stream.  Abort only the in-flight record; the new
        // atomic configuration is applied on the following idle boundary.
        abort_dco         <= 1'b1;
      end

      // Apply the complete stable bus atomically only when the acquisition
      // core is idle.  The acknowledgement toggle is generated on the same
      // edge and reaches the control domain after the new values are live.
      // Do not apply on the same cycle that a new toggle is first observed.
      // That cycle raises abort_dco to retire an in-flight record.  Applying
      // simultaneously made mca_acquisition take its abort branch and skip
      // config_apply even though the control side received an acknowledgement.
      if (cfg_apply_pending && !event_active_ctrl_dco && !window_open) begin
        cfg_toggle_seen           <= cfg_toggle_ff2;
        cfg_apply_pending         <= 1'b0;
        cfg_threshold_dco         <= cfg_bus_clk[13:0];
        cfg_return_dco            <= cfg_bus_clk[27:14];
        cfg_window_total_dco      <= cfg_bus_clk[39:28];
        cfg_window_pre_dco        <= cfg_bus_clk[51:40];
        cfg_psd_dco<=cfg_bus_clk[251]; cfg_psd_start_dco<=cfg_bus_clk[263:252]; cfg_psd_end_dco<=cfg_bus_clk[275:264];
        cfg_guard_dco             <= cfg_bus_clk[65:52];
        cfg_pulse_threshold_dco   <= cfg_bus_clk[79:66];
        cfg_fast_shift_dco        <= cfg_bus_clk[85:80];
        cfg_track_shift_dco       <= cfg_bus_clk[91:86];
        cfg_recovery_shift_dco    <= cfg_bus_clk[97:92];
        cfg_fast_samples_dco      <= cfg_bus_clk[113:98];
        cfg_pulse_hold_dco        <= cfg_bus_clk[197:182];
        cfg_offset_qualify_dco    <= cfg_bus_clk[213:198];
        cfg_filter_enable_dco     <= cfg_bus_clk[214];
        cfg_peak_interpolation_dco<= cfg_bus_clk[250];
        cfg_recovery_tolerance_dco<= cfg_bus_clk[228:215];
        manual_baseline_active_dco<= cfg_bus_clk[143:128];
        baseline_auto_active_dco  <= cfg_bus_clk[144];
        run_enable_dco            <= cfg_bus_clk[145];
        cfg_polarity_negative_dco <= cfg_bus_clk[146];
        cfg_wave_mode_dco         <= cfg_bus_clk[147];
        cfg_external_mode_dco     <= cfg_bus_clk[229];
        cfg_external_duration_us_dco <= cfg_bus_clk[249:230];
        cfg_wave_single_dco       <= cfg_bus_clk[148];
        cfg_wave_session_dco      <= cfg_bus_clk[181:150];
        // Starting/stopping or switching spectrum/waveform mode must not kick
        // a converged estimator back into FAST.  Only a real auto/manual mode
        // transition restarts the estimator; range changes are handled below.
        baseline_apply_restart    <=
            (baseline_auto_active_dco != cfg_bus_clk[144]) ||
            (cfg_filter_enable_dco != cfg_bus_clk[214]);
        // A manual baseline is a valid seed for a manual-to-auto change.
        // A simultaneous filter change invalidates that seed.
        baseline_restart_keep_valid <=
            !baseline_auto_active_dco && cfg_bus_clk[144] &&
            (cfg_filter_enable_dco == cfg_bus_clk[214]);
        config_apply_dco          <= 1'b1;
        cfg_ack_dco               <= 1'b1;
        cfg_ack_toggle_dco        <= ~cfg_ack_toggle_dco;
        config_version_dco        <= config_version_dco + 1'b1;
      end

      // A range change restarts the estimator so the new range is measured
      // from a clean baseline.
      if (range_change_restart) begin
        baseline_apply_restart <= 1'b1;
        baseline_restart_keep_valid <= 1'b0;
      end
    end
  end

  mca_external_trigger u_external_trigger (
    .clk(DCO125M), .rst_n(rst_dco_n),
    .ex_tri_async(ex_tri),
    .enable(external_armed_dco),
    .abort(abort_dco),
    .clear_count(clear_dco),
    .duration_us(cfg_external_duration_us_dco),
    .window_active(external_window_active_dco),
    .trigger_count(external_trigger_count_dco)
  );

  // -----------------------------------------------------------------
  // ADC front end.  The raw/bypass path has exactly the same fixed latency as
  // the two-stage RC path.  The raw overrange bit and timestamp therefore
  // cannot be hidden by a smoothed code.
  // -----------------------------------------------------------------
  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n)
      adc_timestamp_dco <= '0;
    else
      adc_timestamp_dco <= adc_timestamp_dco + 48'd1;
  end

  mca_frontend_filter #(.ADC_WIDTH(14)) u_frontend_filter (
    .clk(DCO125M), .rst_n(rst_dco_n),
    .filter_enable(cfg_filter_enable_dco),
    .clear_history(config_apply_dco | range_change_restart),
    .sample_valid(1'b1), .sample(adc_data), .raw_overrange(adc_or),
    .timestamp(adc_timestamp_dco),
    .output_valid(frontend_pipeline_valid),
    .output_sample(frontend_sample),
    .output_overrange(frontend_overrange),
    .output_timestamp(frontend_timestamp)
  );

  // Register the complete front-end output bundle before the baseline,
  // acquisition and waveform engines.  The timing report showed the raw
  // front-end sample feeding the baseline state decision in one DCO cycle.
  // This boundary removes that long front-end-to-FSM path while preserving
  // sample identity across all three consumers.
  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      frontend_pipeline_valid_dco_q <= 1'b0;
      frontend_sample_dco_q          <= 14'd0;
      frontend_overrange_dco_q       <= 1'b0;
      frontend_timestamp_dco_q       <= '0;
    end else if (config_apply_dco || range_change_restart) begin
      // The front-end history is cleared on the same control pulse.  Drop
      // the local copy as well so no pre-change sample can cross the restart
      // boundary into a new baseline or capture.
      frontend_pipeline_valid_dco_q <= 1'b0;
      frontend_sample_dco_q          <= 14'd0;
      frontend_overrange_dco_q       <= 1'b0;
      frontend_timestamp_dco_q       <= '0;
    end else begin
      frontend_pipeline_valid_dco_q <= frontend_pipeline_valid;
      frontend_sample_dco_q          <= frontend_sample;
      frontend_overrange_dco_q       <= frontend_overrange;
      frontend_timestamp_dco_q       <= frontend_timestamp;
    end
  end

  // -----------------------------------------------------------------
  // Baseline estimator
  // -----------------------------------------------------------------
  logic baseline_hold_dco;

  mca_baseline #(
    .ADC_WIDTH      (14),
    .BASELINE_WIDTH (14),
    .FRACTION_BITS  (24)
  ) u_baseline (
    .clk(DCO125M), .rst_n(rst_dco_n),
     .sample_valid(frontend_pipeline_valid_dco_q),
     .sample(frontend_sample_dco_q),
     .sample_overrange(frontend_overrange_dco_q),
    .auto_mode(baseline_auto_active_dco),
    .manual_baseline(manual_baseline_active_dco),
    .guard(cfg_guard_dco),
    .pulse_threshold(cfg_pulse_threshold_dco),
     .track_shift(cfg_track_shift_dco),
     .recovery_shift(cfg_recovery_shift_dco),
     .fast_shift(cfg_fast_shift_dco),
     .fast_samples(cfg_fast_samples_dco),
     .recovery_samples(BASELINE_RECOVERY_SAMPLES_FIXED),
     .recovery_tolerance_lsb(cfg_recovery_tolerance_dco),
    .offset_qualify(cfg_offset_qualify_dco),
    .pulse_hold_samples(cfg_pulse_hold_dco),
    .hold(baseline_hold_dco),
    .train_hold(dco_pause_req_ff2),
    .restart(baseline_apply_restart | range_change_restart),
    .restart_keep_valid(baseline_restart_keep_valid),
    .baseline(baseline_dco),
    .state(baseline_state_dco),
    .ready(baseline_ready_dco),
    .update_enable()
  );

  // -----------------------------------------------------------------
  // Acquisition
  // -----------------------------------------------------------------
  logic        event_wr_dco;
  logic [95:0] event_data_dco;
  logic [231:0] tagged_event_data_dco;
  logic        event_fifo_full, event_fifo_empty, event_fifo_rd_en;
  logic [231:0] event_fifo_dout;
  logic [8:0]  event_wr_count, event_rd_count;
  logic        event_wr_busy, event_rd_busy;
  logic [47:0] timestamp_dco;
  logic [31:0] event_count_dco, rejected_count_dco, dropped_count_dco;
  logic        event_active_dco;
  logic        histogram_event_valid_dco;
  logic [13:0] histogram_peak_dco;
  logic [23:0] histogram_integral_dco;
  logic        event_active_snapshot_dco;

  // Register the low-bandwidth control/status fanout at the DCO boundary.
  // The raw baseline value remains connected to acquisition so its sample and
  // baseline timing stay aligned; only control/status consumers use the local
  // copies. The configuration path uses a conservative local event copy;
  // the pause handshake retains its edge-accurate current-idle check.
  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      baseline_local_dco        <= 14'd8192;
      baseline_state_local_dco  <= 2'd0;
      baseline_ready_local_dco  <= 1'b0;
      event_active_ctrl_dco     <= 1'b0;
      event_active_snapshot_dco <= 1'b0;
    end else begin
      baseline_local_dco        <= baseline_dco;
      baseline_state_local_dco  <= baseline_state_dco;
      baseline_ready_local_dco  <= baseline_ready_dco;
      event_active_ctrl_dco     <= event_active_dco;
      event_active_snapshot_dco <= event_active_dco;
    end
  end

  mca_acquisition #(
    .ADC_WIDTH     (14),
    .MAX_WINDOW    (2500),
    .MIN_WINDOW    (6),
    .BASELINE_INIT (8192),
    .INTEGRAL_BITS (28)
  ) u_acquisition (
    .dco_clk(DCO125M), .rst_n(rst_dco_n),
    // Spectrum and waveform capture are mutually exclusive.  Leaving the
    // spectrum engine active in waveform mode filled the event FIFO, held the
    // baseline during ordinary events and starved waveform uploads.
    .run_enable(spectrum_run_dco),
    .trigger_enable(spectrum_trigger_enable_dco),
    .holdoff(dco_pause_req_ff2),
    .abort_event(abort_dco),
    .clear_counts(clear_dco),
    .config_apply(config_apply_dco),
    .cfg_threshold(cfg_threshold_dco),
    .cfg_return(cfg_return_dco),
    .cfg_window_total(cfg_window_total_dco),
    .cfg_window_pre(cfg_window_pre_dco),
    .psd_enable(cfg_psd_dco), .psd_tail_start(cfg_psd_start_dco), .psd_tail_end(cfg_psd_end_dco),
    .config_version(config_version_dco), .tagged_event_data(tagged_event_data_dco),
    .cfg_polarity_negative(cfg_polarity_negative_dco),
    .peak_interpolation_enable(cfg_peak_interpolation_dco),
    .baseline_hold(baseline_hold_dco),
    .baseline(baseline_dco),
    .baseline_state(baseline_state_local_dco),
    .baseline_ready(baseline_ready_for_events),
     .adc_data(frontend_sample_dco_q),
     .adc_overrange(frontend_overrange_dco_q),
    .event_wr(event_wr_dco), .event_data(event_data_dco),
    .event_full(event_fifo_full),
    .histogram_event_valid(histogram_event_valid_dco),
    .histogram_peak(histogram_peak_dco),
    .histogram_integral(histogram_integral_dco),
    .timestamp(timestamp_dco),
    .active_baseline(),
    .active_baseline_state(),
    .event_count(event_count_dco),
    .rejected_count(rejected_count_dco),
    .dropped_count(dropped_count_dco),
    .event_active(event_active_dco),
    .accepting(),
    .window_open(window_open)
  );

  // -----------------------------------------------------------------
  // Local UART spectra.  The histogram owns both RAM write ports; the
  // serial side only reads the stable 64-bin chunk while it awaits ACK.  The
  // shorter wire frame stays below the sustained receive burst that caused the
  // board CH340C/Windows path to drop bytes from long sustained frames.  The
  // production UART also runs at 500 kbaud for receive-throughput margin.
  // -----------------------------------------------------------------
  logic hist_ready, hist_chunk_toggle, hist_chunk_ack_toggle;
  logic [31:0] hist_session, hist_epoch, hist_config_version, hist_interval_events;
  logic hist_chunk_kind;
  logic [7:0] hist_chunk_flags, hist_bin_shift, hist_chunk_rd_index;
  logic [15:0] hist_total_bins, hist_chunk_start, hist_chunk_count;
  logic [15:0] hist_chunk_index, hist_chunk_total;
  logic [47:0] hist_interval_ticks;
  logic [31:0] hist_overflow, hist_chunk_rd_data;

  mca_spectrum_histogram #(
    .BIN_COUNT(UART_SPECTRUM_BINS), .CHUNK_BINS(64), .CLOCK_HZ(125_000_000)
  ) u_uart_histogram (
    .dco_clk(DCO125M), .host_clk(clk_125m_sys), .rst_n(rst_dco_n),
    .run_enable(spectrum_run_dco), .clear(clear_dco),
    .event_valid(histogram_event_valid_dco),
    .event_peak(histogram_peak_dco),
    .event_integral(histogram_integral_dco),
    .config_version(config_version_dco),
    .ready(hist_ready), .chunk_toggle(hist_chunk_toggle),
    .chunk_ack_toggle(hist_chunk_ack_toggle),
    .session_id(hist_session), .epoch(hist_epoch),
    .chunk_config_version(hist_config_version),
    .chunk_kind(hist_chunk_kind), .chunk_flags(hist_chunk_flags),
    .bin_shift(hist_bin_shift), .total_bins(hist_total_bins),
    .chunk_start(hist_chunk_start), .chunk_count(hist_chunk_count),
    .chunk_index(hist_chunk_index), .chunk_total(hist_chunk_total),
    .interval_ticks(hist_interval_ticks),
    .interval_events(hist_interval_events),
    .histogram_overflow(hist_overflow),
    .chunk_rd_index(hist_chunk_rd_index),
    .chunk_rd_data(hist_chunk_rd_data)
  );

  logic [31:0] uart_crc_errors, uart_frame_errors;
  mca_uart_protocol #(
    .CLOCK_HZ(125_000_000), .BAUD_HZ(500_000),
    .BIN_COUNT(UART_SPECTRUM_BINS), .CHUNK_BINS(64),
    .FIRMWARE_VERSION(32'h0002_0700)
  ) u_uart_protocol (
    .clk(clk_125m_sys), .rst_n(rst_clk_n),
    .uart_rxd(uart_rxd), .uart_txd(uart_txd),
    .cmd_valid(uart_cmd_valid), .cmd_payload(uart_cmd_payload),
    .cmd_len(uart_cmd_len), .cmd_ready(uart_cmd_ready),
    .resp_valid(ctrl_resp_valid && inflight_source_uart),
    .resp_payload(ctrl_resp_payload), .resp_len(ctrl_resp_len),
    .resp_ready(uart_resp_ready),
    .chunk_toggle(hist_chunk_toggle),
    .chunk_ack_toggle(hist_chunk_ack_toggle),
    .chunk_session(hist_session), .chunk_epoch(hist_epoch),
    .chunk_config_version(hist_config_version),
    .chunk_kind(hist_chunk_kind), .chunk_flags(hist_chunk_flags),
    .chunk_shift(hist_bin_shift), .chunk_total_bins(hist_total_bins),
    .chunk_start(hist_chunk_start), .chunk_count(hist_chunk_count),
    .chunk_index(hist_chunk_index), .chunk_total(hist_chunk_total),
    .chunk_ticks(hist_interval_ticks), .chunk_events(hist_interval_events),
    .chunk_dropped(snap_dropped_count_clk),
    .status_running(snap_running_clk),
    .status_baseline_state(snap_state_clk),
    .status_baseline_ready(snap_ready_clk),
    .status_baseline_auto(snap_auto_clk),
    .status_baseline(snap_baseline_clk),
    .status_manual_baseline(snap_manual_clk),
    .status_recovery_tolerance(snap_recovery_tolerance_clk),
    .status_frontend_init(snap_frontend_init_clk),
    .status_range_busy(snap_range_busy_clk),
    .status_filter_enabled(snap_filter_enabled_clk),
    .status_peak_interpolation(snap_peak_interpolation_clk),
    .status_actual_range(snap_actual_range_clk),
    .status_mode(snap_mode_clk),
    .status_external_armed(snap_external_armed_clk),
    .status_external_window(snap_external_window_clk),
    .status_external_count(snap_external_count_clk),
    .status_timestamp(snap_timestamp_clk),
    .status_config_version(snap_config_version_clk),
    .status_event_count(snap_event_count_clk),
    .status_rejected_count(snap_rejected_count_clk),
    .status_dropped_count(snap_dropped_count_clk),
    .chunk_rd_index(hist_chunk_rd_index),
    .chunk_rd_data(hist_chunk_rd_data),
    .crc_error_count(uart_crc_errors),
    .frame_error_count(uart_frame_errors)
  );

  assign dco_event_active = event_active_dco;

  mca_async_fifo #(.WIDTH(232), .ADDR_WIDTH(8)) u_event_fifo (
    .rst_n(rst_n), .wr_clk(DCO125M), .wr_en(event_wr_dco),
    .din(tagged_event_data_dco), .full(event_fifo_full),
    .wr_data_count(event_wr_count), .wr_rst_busy(event_wr_busy),
    .rd_clk(gmii_clk), .rd_en(event_fifo_rd_en), .dout(event_fifo_dout),
    .empty(event_fifo_empty), .rd_data_count(event_rd_count),
    .rd_rst_busy(event_rd_busy)
  );

  // -----------------------------------------------------------------
  // Status snapshot and snapshot handshake
  // -----------------------------------------------------------------
  logic        status_wr_dco;
  logic [STATUS_W-1:0] status_payload_dco;
  logic        status_fifo_full, status_fifo_empty, status_fifo_rd_en;
  logic [STATUS_W-1:0] status_fifo_dout;
  logic [4:0]  status_wr_count, status_rd_count;
  logic        status_wr_busy, status_rd_busy;

  // Waveform status snapshots are produced in the DCO domain and consumed by
  // both the control-domain snapshot bridge and the network-domain sender.
  logic [31:0] wave_status_bus, wave_snap_status, wave_snap_capture;
  logic [31:0] wave_snap_count;

  logic snap_toggle_dco, snap_toggle_ff1, snap_toggle_ff2, snap_toggle_seen;
  logic [31:0] snap_period_count;

  mca_status_snapshot #(
    .INTERVAL_CYCLES (STATUS_INTERVAL_CYCLES),
    .PAYLOAD_BYTES   (STATUS_BYTES)
  ) u_status_snapshot (
    .dco_clk(DCO125M), .rst_n(rst_dco_n),
    .force_refresh(cfg_ack_dco),
    .config_version(config_version_dco),
    .timestamp(timestamp_dco),
    .event_count(event_count_dco),
    .rejected_count(rejected_count_dco),
    .dropped_count(dropped_count_dco),
    .baseline(baseline_local_dco),
    .baseline_state(baseline_state_local_dco),
    .baseline_ready(baseline_ready_local_dco),
    .baseline_auto(baseline_auto_active_dco),
    .manual_baseline(manual_baseline_active_dco),
    .run_enable(run_enable_dco),
    .mode(cfg_psd_dco ? 3'd5 : cfg_external_mode_dco ? 3'd4 :
         (cfg_wave_mode_dco ? 3'd3 : 3'd0)),
    .event_active(event_active_snapshot_dco),
    .event_fifo_level(event_wr_count[7:0]),
    .adc_aligned(1'b1),
    .range_actual(range_applied_value_clk),
    .range_busy(range_busy),
    .filter_enabled(cfg_filter_enable_dco),
    .peak_interpolation_enabled(cfg_peak_interpolation_dco),
    .frontend_init_done(init_done),
    .recovery_tolerance(cfg_recovery_tolerance_dco),
    .wr_en(status_wr_dco),
    .payload(status_payload_dco),
    .fifo_full(status_fifo_full)
  );

  // Continuous readback path: the sampling domain keeps a stable copy of the
  // values the control domain reads, and toggles once per status interval.
  logic [13:0] snap_baseline_dco;
  logic [1:0]  snap_state_dco;
  logic        snap_ready_dco;
  logic        snap_auto_dco;
  logic [13:0] snap_manual_dco;
  logic [31:0] snap_event_count_dco, snap_rejected_count_dco;
  logic [31:0] snap_dropped_count_dco;
  logic [47:0] snap_timestamp_dco;
  logic        snap_event_active_dco;
  logic        snap_external_armed_dco;
  logic        snap_external_window_dco;
  logic [31:0] snap_external_count_dco;
  logic [31:0] snap_wave_status_dco;
  logic [31:0] snap_wave_capture_dco;
  logic [31:0] snap_wave_count_dco;
  logic        snap_running_dco;
  logic [1:0]  snap_actual_range_dco;
  logic        snap_range_busy_dco;
  logic        snap_filter_enabled_dco;
  logic        snap_peak_interpolation_dco;
  logic        snap_frontend_init_dco;
  logic [13:0] snap_recovery_tolerance_dco;
  logic [2:0]  snap_mode_dco;

  // Range/front-end controls originate in the system clock domain.  Bring
  // them into the sampling domain before including them in the status
  // snapshot so the UART never samples an asynchronous multi-bit value.
  logic [1:0] range_applied_dco_ff1, range_applied_dco_ff2;
  logic range_busy_dco_ff1, range_busy_dco_ff2;
  logic init_done_dco_ff1, init_done_dco_ff2;
  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      range_applied_dco_ff1 <= 2'b00;
      range_applied_dco_ff2 <= 2'b00;
      range_busy_dco_ff1    <= 1'b1;
      range_busy_dco_ff2    <= 1'b1;
      init_done_dco_ff1     <= 1'b0;
      init_done_dco_ff2     <= 1'b0;
    end else begin
      range_applied_dco_ff1 <= range_applied_value_clk;
      range_applied_dco_ff2 <= range_applied_dco_ff1;
      range_busy_dco_ff1    <= range_busy;
      range_busy_dco_ff2    <= range_busy_dco_ff1;
      init_done_dco_ff1     <= init_done;
      init_done_dco_ff2     <= init_done_dco_ff1;
    end
  end

  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      snap_toggle_dco     <= 1'b0;
      snap_period_count   <= '0;
      snap_baseline_dco   <= 14'd8192;
      snap_state_dco      <= 2'd0;
      snap_ready_dco      <= 1'b0;
      snap_auto_dco       <= 1'b0;
      snap_manual_dco     <= 14'd8192;
      snap_event_count_dco<= '0;
      snap_rejected_count_dco <= '0;
      snap_dropped_count_dco  <= '0;
      snap_timestamp_dco  <= '0;
      snap_event_active_dco <= 1'b0;
      snap_external_armed_dco <= 1'b0;
      snap_external_window_dco <= 1'b0;
      snap_external_count_dco <= '0;
      snap_wave_status_dco  <= mca_pkg::WAVE_STATE_STOPPED;
      snap_wave_capture_dco <= '0;
      snap_wave_count_dco   <= '0;
      snap_running_dco      <= 1'b0;
      snap_actual_range_dco <= 2'b00;
      snap_range_busy_dco   <= 1'b1;
      snap_filter_enabled_dco <= 1'b0;
      snap_frontend_init_dco <= 1'b0;
      snap_recovery_tolerance_dco <= 14'd32;
      snap_mode_dco         <= 3'd0;
    end else if ((snap_period_count == STATUS_INTERVAL_CYCLES-1) ||
                 (baseline_ready_local_dco != snap_ready_dco) ||
                 (baseline_state_local_dco != snap_state_dco) ||
                 (wave_status_bus != snap_wave_status_dco) ||
                 (run_enable_dco != snap_running_dco) ||
                 (range_applied_dco_ff2 != snap_actual_range_dco) ||
                 (range_busy_dco_ff2 != snap_range_busy_dco) ||
                 (cfg_filter_enable_dco != snap_filter_enabled_dco) ||
                 (cfg_peak_interpolation_dco != snap_peak_interpolation_dco) ||
                 (init_done_dco_ff2 != snap_frontend_init_dco) ||
                 (cfg_recovery_tolerance_dco != snap_recovery_tolerance_dco) ||
                 ((cfg_psd_dco ? 3'd5 : cfg_external_mode_dco ? 3'd4 :
                   (cfg_wave_mode_dco ? 3'd3 : 3'd0)) != snap_mode_dco) ||
                 (external_armed_dco != snap_external_armed_dco) ||
                 (external_window_active_dco != snap_external_window_dco) ||
                 (external_trigger_count_dco != snap_external_count_dco)) begin
      // Publish state transitions immediately.  Waiting for the 100 ms
      // periodic snapshot made START see a stale "training" baseline and
      // made waveform-status polling lag far behind the capture engine.
      snap_period_count   <= '0;
      snap_toggle_dco     <= ~snap_toggle_dco;
      snap_baseline_dco   <= baseline_local_dco;
      snap_state_dco      <= baseline_state_local_dco;
      snap_ready_dco      <= baseline_ready_local_dco;
      snap_auto_dco       <= baseline_auto_active_dco;
      snap_manual_dco     <= manual_baseline_active_dco;
      snap_event_count_dco<= event_count_dco;
      snap_rejected_count_dco <= rejected_count_dco;
      snap_dropped_count_dco  <= dropped_count_dco;
      snap_timestamp_dco  <= timestamp_dco;
      snap_event_active_dco <= event_active_snapshot_dco;
      snap_external_armed_dco <= external_armed_dco;
      snap_external_window_dco <= external_window_active_dco;
      snap_external_count_dco <= external_trigger_count_dco;
      snap_wave_status_dco  <= wave_status_bus;
      snap_wave_capture_dco <= wave_snap_capture;
      snap_wave_count_dco   <= wave_snap_count;
      snap_running_dco      <= run_enable_dco;
      snap_actual_range_dco <= range_applied_dco_ff2;
      snap_range_busy_dco   <= range_busy_dco_ff2;
      snap_filter_enabled_dco <= cfg_filter_enable_dco;
      snap_peak_interpolation_dco <= cfg_peak_interpolation_dco;
      snap_frontend_init_dco <= init_done_dco_ff2;
      snap_recovery_tolerance_dco <= cfg_recovery_tolerance_dco;
      snap_mode_dco         <= cfg_psd_dco ? 3'd5 : cfg_external_mode_dco ? 3'd4 :
                                (cfg_wave_mode_dco ? 3'd3 : 3'd0);
    end else begin
      snap_period_count <= snap_period_count + 1'b1;
    end
  end

  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      snap_toggle_ff1 <= 1'b0;
      snap_toggle_ff2 <= 1'b0;
      snap_toggle_seen<= 1'b0;
      snap_baseline_clk <= 14'd8192;
      snap_state_clk  <= 2'd0;
      snap_ready_clk  <= 1'b0;
      snap_auto_clk   <= 1'b0;
      snap_manual_clk <= 14'd8192;
      snap_config_version_clk <= 32'h0001_0000;
      snap_event_count_clk <= '0;
      snap_rejected_count_clk <= '0;
      snap_dropped_count_clk  <= '0;
      snap_timestamp_clk <= '0;
      snap_event_active_clk <= 1'b0;
      snap_external_armed_clk <= 1'b0;
      snap_external_window_clk <= 1'b0;
      snap_external_count_clk <= '0;
      snap_wave_status_clk  <= mca_pkg::WAVE_STATE_STOPPED;
      snap_wave_capture_clk <= '0;
      snap_wave_count_clk   <= '0;
      snap_running_clk      <= 1'b0;
      snap_actual_range_clk <= 2'b00;
      snap_range_busy_clk   <= 1'b1;
      snap_filter_enabled_clk <= 1'b0;
      snap_peak_interpolation_clk <= 1'b0;
      snap_frontend_init_clk <= 1'b0;
      snap_recovery_tolerance_clk <= 14'd32;
      snap_mode_clk         <= 3'd0;
      cfg_ack_clk    <= 1'b0;
      cfg_ack_seen   <= 1'b0;
      cfg_ack_toggle_ff1 <= 1'b0;
      cfg_ack_toggle_ff2 <= 1'b0;
    end else begin
      snap_toggle_ff1 <= snap_toggle_dco;
      snap_toggle_ff2 <= snap_toggle_ff1;
      if (snap_toggle_ff2 != snap_toggle_seen) begin
        snap_toggle_seen  <= snap_toggle_ff2;
        snap_baseline_clk <= snap_baseline_dco;
        snap_state_clk    <= snap_state_dco;
        snap_ready_clk    <= snap_ready_dco;
        snap_auto_clk     <= snap_auto_dco;
        snap_manual_clk   <= snap_manual_dco;
        snap_event_count_clk <= snap_event_count_dco;
        snap_rejected_count_clk <= snap_rejected_count_dco;
        snap_dropped_count_clk  <= snap_dropped_count_dco;
        snap_timestamp_clk <= snap_timestamp_dco;
        snap_event_active_clk <= snap_event_active_dco;
        snap_external_armed_clk <= snap_external_armed_dco;
        snap_external_window_clk <= snap_external_window_dco;
        snap_external_count_clk <= snap_external_count_dco;
        snap_wave_status_clk  <= snap_wave_status_dco;
        snap_wave_capture_clk <= snap_wave_capture_dco;
        snap_wave_count_clk   <= snap_wave_count_dco;
        snap_running_clk      <= snap_running_dco;
        snap_actual_range_clk <= snap_actual_range_dco;
        snap_range_busy_clk   <= snap_range_busy_dco;
        snap_filter_enabled_clk <= snap_filter_enabled_dco;
        snap_peak_interpolation_clk <= snap_peak_interpolation_dco;
        snap_frontend_init_clk <= snap_frontend_init_dco;
        snap_recovery_tolerance_clk <= snap_recovery_tolerance_dco;
        snap_mode_clk         <= snap_mode_dco;
      end
      cfg_ack_toggle_ff1 <= cfg_ack_toggle_dco;
      cfg_ack_toggle_ff2 <= cfg_ack_toggle_ff1;
      cfg_ack_clk <= 1'b0;
      if (cfg_ack_toggle_ff2 != cfg_ack_seen) begin
        cfg_ack_seen <= cfg_ack_toggle_ff2;
        cfg_ack_clk  <= 1'b1;
      end
    end
  end

  // The sampling-domain configuration version crosses on its own toggle so a
  // control read always sees a version that has really been applied.
  logic        ver_toggle_dco, ver_toggle_ff1, ver_toggle_ff2, ver_toggle_seen;
  logic [31:0] ver_value_dco;
  always_ff @(posedge DCO125M or negedge rst_dco_n) begin
    if (!rst_dco_n) begin
      ver_toggle_dco <= 1'b0;
      ver_value_dco  <= 32'h0001_0000;
    end else if (cfg_ack_dco) begin
      ver_toggle_dco <= ~ver_toggle_dco;
      ver_value_dco  <= config_version_dco + 1'b1;
    end
  end
  always_ff @(posedge clk_125m_sys or negedge rst_clk_n) begin
    if (!rst_clk_n) begin
      ver_toggle_ff1 <= 1'b0;
      ver_toggle_ff2 <= 1'b0;
      ver_toggle_seen<= 1'b0;
      snap_config_version_dco <= 32'h0001_0000;
    end else begin
      ver_toggle_ff1 <= ver_toggle_dco;
      ver_toggle_ff2 <= ver_toggle_ff1;
      if (ver_toggle_ff2 != ver_toggle_seen) begin
        ver_toggle_seen <= ver_toggle_ff2;
        snap_config_version_dco <= ver_value_dco;
      end
    end
  end

  // -----------------------------------------------------------------
  // Waveform capture engine
  //
  // The engine shares the threshold, polarity and baseline configuration with
  // the spectrum path, and only accepts a trigger after the complete record
  // is backed by written samples.
  // -----------------------------------------------------------------
  logic        wave_frag_valid, wave_frag_ack;
  logic [15:0] wave_frag_index, wave_frag_samples, wave_frag_offset;
  logic [15:0] wave_frag_fragment_count;
  logic        wave_frag_last;
  logic [31:0] wave_frag_capture_id;
  logic [47:0] wave_frag_timestamp;
  logic [13:0] wave_frag_baseline, wave_frag_threshold, wave_frag_return;
  logic        wave_frag_negative;
  logic [31:0] wave_frag_version, wave_frag_session;
  logic [15:0] wave_word_index, wave_word;
  logic        wave_word_req, wave_word_valid;
  logic        wave_word_read;

  wire wave_cancel = !run_enable_dco || !cfg_wave_mode_dco;

  mca_waveform #(
    .ADC_WIDTH(14),
    .PRE_SAMPLES(mca_pkg::WAVE_PRE_SAMPLES),
    .TOTAL_SAMPLES(mca_pkg::WAVE_TOTAL_SAMPLES),
    .MEM_DEPTH(mca_pkg::WAVE_MEM_DEPTH),
    .MEM_AW(mca_pkg::WAVE_MEM_AW),
    .FRAGMENT_MAX(mca_pkg::WAVE_FRAGMENT_MAX),
    .FRAGMENT_COUNT(mca_pkg::WAVE_FRAGMENT_COUNT),
    .BASELINE_INIT(8192)
  ) u_waveform (
    .dco_clk(DCO125M), .rst_dco_n(rst_dco_n),
    .run_enable(run_enable_dco && cfg_wave_mode_dco),
    .holdoff(dco_pause_req_ff2),
    .abort(abort_dco),
    .config_apply(config_apply_dco),
    .cfg_polarity_negative(cfg_polarity_negative_dco),
    .cfg_threshold(cfg_threshold_dco),
    .cfg_return(cfg_return_dco),
    .cfg_baseline(baseline_dco),
    .cfg_baseline_ready(baseline_ready_for_events),
    .cfg_config_version(config_version_dco + 32'd1),
    .cfg_session_id(cfg_wave_session_dco),
    .cfg_single_shot(cfg_wave_single_dco),
     .adc_data(frontend_sample_dco_q),
     .adc_overrange(frontend_overrange_dco_q),
    .timestamp(),
    .active_baseline(),
    .capture_id(),
    .active_threshold_out(),
    .active_polarity_out(),
    .gmii_clk(gmii_clk), .rst_net_n(rst_net_n),
    .status_snapshot(wave_status_bus),
    .capture_count_snapshot(wave_snap_count),
    .last_capture_snapshot(wave_snap_capture),
    .frag_valid(wave_frag_valid), .frag_ack(wave_frag_ack),
    .frag_index(wave_frag_index),
    .frag_sample_count(wave_frag_samples),
    .frag_sample_offset(wave_frag_offset),
    .frag_fragment_count(wave_frag_fragment_count),
    .frag_last(wave_frag_last),
    .frag_capture_id(wave_frag_capture_id),
    .frag_timestamp(wave_frag_timestamp),
    .frag_baseline(wave_frag_baseline),
    .frag_threshold(wave_frag_threshold),
    .frag_return(wave_frag_return),
    .frag_negative(wave_frag_negative),
    .frag_config_version(wave_frag_version),
    .frag_session_id(wave_frag_session),
    .frag_word_index(wave_word_index),
    .frag_word_req(wave_word_read),
    .frag_word(wave_word),
    .frag_word_valid(wave_word_valid)
  );

  assign wave_word_read = wave_word_req & wave_frag_valid;

  // -----------------------------------------------------------------
  // Response/status/event transmit path
  // -----------------------------------------------------------------
  logic        eth_tx_start, eth_tx_req, eth_tx_done;
  logic [15:0] eth_tx_len, eth_rec_len;
  logic [7:0]  eth_tx_data, eth_rec_data;
  logic        eth_rec_en, eth_rec_done;

  mca_net_tx #(
    .LOCAL_MAC(LOCAL_MAC), .HOST_MAC(HOST_MAC),
    .LOCAL_IP(LOCAL_IP), .HOST_IP(HOST_IP),
    .UDP_SRC_PORT(UDP_DATA_PORT), .UDP_DST_PORT(UDP_CONTROL_PORT),
     .MAX_BATCH(EVENT_BATCH_MAX),
     .PSD_MAX_BATCH(40),
     .STATUS_BYTES(STATUS_BYTES),
     .STATUS_FLAGS(8'hC0),
    .STAGE_BYTES(mca_pkg::WAVE_FRAGMENT_HEADER_BYTES
                 + mca_pkg::WAVE_FRAGMENT_MAX * 2 + 20)
  ) u_net_tx (
    .clk(gmii_clk), .rst_n(rst_net_n), .device_id(8'd0),
    .event_empty(event_fifo_empty | event_rd_busy),
    .event_rd_en(event_fifo_rd_en), .event_dout(event_fifo_dout),
    .event_level(event_rd_count),
    .resp_empty(resp_fifo_empty | resp_rd_busy),
    .resp_rd_en(resp_fifo_rd_en), .resp_dout(resp_fifo_dout),
    .status_empty(status_fifo_empty | status_rd_busy),
    .status_rd_en(status_fifo_rd_en), .status_dout(status_fifo_dout),
    .wave_valid(wave_frag_valid), .wave_ack(wave_frag_ack),
    .wave_cancel(wave_cancel),
    .wave_index(wave_frag_index),
    .wave_sample_count(wave_frag_samples),
    .wave_sample_offset(wave_frag_offset),
    .wave_fragment_count(wave_frag_fragment_count),
    .wave_last(wave_frag_last),
    .wave_capture_id(wave_frag_capture_id),
    .wave_timestamp(wave_frag_timestamp),
    .wave_baseline(wave_frag_baseline),
    .wave_threshold(wave_frag_threshold),
    .wave_return(wave_frag_return),
    .wave_negative(wave_frag_negative),
    .wave_config_version(wave_frag_version),
    .wave_session_id(wave_frag_session),
    .wave_word_index(wave_word_index),
    .wave_word_req(wave_word_req),
    .wave_word(wave_word),
    .wave_word_valid(wave_word_valid),
    .cfg_version(snap_config_version_dco),
    .tx_start_en(eth_tx_start), .tx_byte_num(eth_tx_len),
    .tx_data(eth_tx_data), .tx_data_req(eth_tx_req), .tx_done(eth_tx_done)
  );

  mca_async_fifo #(.WIDTH(STATUS_W), .ADDR_WIDTH(4)) u_status_fifo (
    .rst_n(rst_n), .wr_clk(DCO125M), .wr_en(status_wr_dco),
    .din(status_payload_dco), .full(status_fifo_full),
    .wr_data_count(status_wr_count), .wr_rst_busy(status_wr_busy),
    .rd_clk(gmii_clk), .rd_en(status_fifo_rd_en), .dout(status_fifo_dout),
    .empty(status_fifo_empty), .rd_data_count(status_rd_count),
    .rd_rst_busy(status_rd_busy)
  );

  // -----------------------------------------------------------------
  // Ethernet / UDP
  // -----------------------------------------------------------------
  logic eth_idelay_ready;
  logic idelay_rst_n;
  (* ASYNC_REG = "TRUE" *) logic [1:0] phy_done_200_sync;
  always_ff @(posedge clk_200m or negedge rst_n) begin
    if (!rst_n)
      phy_done_200_sync <= 2'b00;
    else
      phy_done_200_sync <= {phy_done_200_sync[0], phy_reset_done};
  end
  assign idelay_rst_n = phy_done_200_sync[1] & clk_locked;

  mca_eth_udp #(
    .BOARD_MAC(LOCAL_MAC), .BOARD_IP(LOCAL_IP),
    .DES_MAC(HOST_MAC), .DES_IP(HOST_IP),
    .DATA_PORT(UDP_DATA_PORT[15:0]),
    .CONTROL_PORT(UDP_CONTROL_PORT[15:0]),
    .IDELAY_VALUE(0)
  ) u_eth (
    .idelay_clk(clk_200m), .idelay_rst_n(idelay_rst_n),
    .phy_rst_n(phy_rst_n), .rst_n(rst_net_n),
    .eth_rxc(rgmii_rxc), .eth_rx_ctl(rgmii_rx_ctl), .eth_rxd(rgmii_rxd),
    .eth_txc(rgmii_txc), .eth_tx_ctl(rgmii_tx_ctl), .eth_txd(rgmii_txd),
    .eth_rst_n(), .idelay_ready(eth_idelay_ready), .gmii_clk(gmii_clk),
    .tx_start_en(eth_tx_start), .tx_byte_num(eth_tx_len),
    .tx_data(eth_tx_data), .tx_data_req(eth_tx_req), .tx_done(eth_tx_done),
    .rec_en(eth_rec_en), .rec_data(eth_rec_data),
    .rec_pkt_done(eth_rec_done), .rec_byte_num(eth_rec_len)

  );

  // -----------------------------------------------------------------
  // MCF2 request receiver (gmii domain)
  // -----------------------------------------------------------------
  logic [7:0]  rx_index;
  logic        rx_finish_pending;
  logic [15:0] rx_finished_len;

  always_ff @(posedge gmii_clk) begin
    if (!rst_net_n) begin
      rx_index          <= '0;
      rx_finish_pending <= 1'b0;
      rx_finished_len   <= '0;
      cmd_wr_net        <= 1'b0;
      cmd_payload_net   <= '0;
      cmd_len_net       <= '0;
    end else begin
      cmd_wr_net <= 1'b0;
      if (eth_rec_en) begin
        if (rx_index < 8'd64)
          cmd_payload_net[rx_index*8 +: 8] <= eth_rec_data;
        if (rx_index != 8'hff)
          rx_index <= rx_index + 1'b1;
      end
      if (eth_rec_done) begin
        rx_finish_pending <= 1'b1;
        rx_finished_len   <= eth_rec_len;
      end
      if (rx_finish_pending) begin
        rx_finish_pending <= 1'b0;
        if ((rx_finished_len <= 16'd64) && !cmd_fifo_full) begin
          cmd_len_net <= rx_finished_len[7:0];
          cmd_wr_net  <= 1'b1;
        end
        rx_index <= '0;
      end
    end
  end

endmodule
