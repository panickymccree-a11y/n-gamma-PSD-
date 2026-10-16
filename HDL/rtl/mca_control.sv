`timescale 1ns/1ps

// MCF2 control endpoint for the single-channel MCA.
//
// Wire format (identical to the 32-channel host):
//
//   request  (20 bytes)
//     0..3   magic "MCF2" = 0x4D434632
//     4      version = 2
//     5      opcode
//     6      target
//     7      target id
//     8..9   register address (big endian)
//     10..11 flags
//     12..15 transaction id (big endian)
//     16..19 value (big endian)
//
//   response (16 bytes)
//     0..3   transaction id
//     4      opcode
//     5      target
//     6      target id
//     7      status
//     8..9   register address
//     10..11 reserved
//     12..15 value
//
// Behaviour required by the single-channel plan:
//   * A repeated transaction id replays the cached response instead of
//     re-applying the command, so host retries stay idempotent.
//   * Commit is only acknowledged after the sampling domain reports that the
//     new configuration is live, and the acknowledgement carries the new
//     configuration version.
//   * Configuration is staged in a shadow bank and becomes active only on
//     commit; reads report the active and the staged values separately.
//   * Errors are reported explicitly; nothing is silently accepted.
module mca_control #(
  parameter logic [31:0] FIRMWARE_VERSION = 32'h0002_0600,
  parameter int          SAMPLE_RATE_HZ   = 125_000_000,
  parameter int          SAMPLE_NS        = 8,
  parameter int          WINDOW_MIN       = 6,
  parameter int          WINDOW_MAX       = 2500
) (
  input  logic         clk,
  input  logic         rst_n,

  // Command interface (MCF2 request)
  input  logic         cmd_valid,
  input  logic         cmd_source, // 0: UDP, 1: UART
  input  logic [511:0] cmd_payload,
  input  logic [7:0]   cmd_len,
  output logic         cmd_ready,
  output logic         busy,

  // Response interface (MCF2 response or legacy format)
  output logic         resp_valid,
  output logic [519:0] resp_payload,
  output logic [7:0]   resp_len,
  input  logic         resp_ready,

  // Configuration bus.  Held stable while `apply_toggle` differs from
  // `apply_ack`.
  output logic [275:0] cfg_bus,
  output logic         cfg_toggle,
  input  logic         cfg_ack,

  // Commit acknowledgement from the sampling domain
  input  logic         commit_ack,
  input  logic [31:0]  applied_config_version,
  input  logic [13:0]  applied_baseline,
  input  logic [1:0]   applied_baseline_state,
  input  logic         applied_baseline_ready,
  input  logic         applied_baseline_auto,
  input  logic [13:0]  applied_manual_baseline,

  // Live status
  input  logic         run_enable,
  input  logic [1:0]   range_applied_value,
  input  logic         range_busy,
  input  logic         phy_ready,
  input  logic         init_done,
  input  logic         q_busy,
  input  logic         filter_enabled,
  input  logic         peak_interpolation_enabled,
  input  logic         frontend_ready,
  input  logic [31:0]  event_count,
  input  logic [31:0]  rejected_count,
  input  logic [31:0]  dropped_count,
  input  logic [47:0]  timestamp,
  input  logic         event_active,
  input  logic         baseline_ready_in,

  // External-trigger status, synchronized into the control clock domain.
  input  logic         external_armed,
  input  logic         external_window_active,
  input  logic [31:0]  external_trigger_count,

  // Waveform status snapshots (already synchronised into this domain)
  input  logic [31:0]  wave_status,
  input  logic [31:0]  wave_last_capture,
  input  logic [31:0]  wave_capture_count,

  // Range request handshake
  output logic [1:0]   range_request_value,
  output logic         range_request_strobe,

  // Level and toggle control to the sampling domain
  output logic         abort_toggle,
  output logic         clear_counts_toggle
);
  localparam int  CFG_W_MAX = $clog2(WINDOW_MAX + 1);
  localparam logic [31:0] CONFIG_VERSION_BASE = 32'h0001_0000;

  // Canonical opcodes after the legacy translation stage.
  localparam logic [7:0] CMD_NONE          = 8'h00;
  localparam logic [7:0] CMD_WRITE_SHADOW  = 8'h01;
  localparam logic [7:0] CMD_READ          = 8'h02;
  localparam logic [7:0] CMD_COMMIT        = 8'h03;
  localparam logic [7:0] CMD_START         = 8'h04;
  localparam logic [7:0] CMD_STOP          = 8'h05;
  localparam logic [7:0] CMD_ABORT         = 8'h06;
  localparam logic [7:0] CMD_CLEAR         = 8'h07;
  localparam logic [7:0] CMD_SET_RANGE     = 8'h09;
  localparam logic [7:0] CMD_READ_LEGACY   = 8'h21;
  localparam logic [7:0] CMD_STATUS_LEGACY = 8'h28;

  // ---------------------------------------------------------------
  // Shadow configuration bank
  // ---------------------------------------------------------------
  logic [13:0] sh_threshold, sh_return, sh_guard, sh_pulse_threshold;
  logic [11:0] sh_window_total, sh_window_pre;
  logic [5:0]  sh_fast_shift, sh_track_shift, sh_recovery_shift;
  logic [15:0] sh_fast_samples, sh_recovery_samples, sh_offset_qualify;
  logic [15:0] sh_pulse_hold;
  logic [13:0] sh_recovery_tolerance;
  logic [13:0] sh_manual_baseline;
  logic        sh_baseline_auto;
  logic        sh_run_enable;
  logic        sh_filter_enable;
  logic        sh_peak_interpolation;
  logic        sh_polarity_negative;
  logic [31:0] sh_mode;
  logic signed [15:0] sh_psd_start, sh_psd_end;
  logic [31:0] sh_wave_single;
  logic [31:0] sh_wave_session;
  logic [19:0] sh_ext_duration_us;

  // Snapshot of what the sampling domain has actually applied.
  logic [31:0] act_config_version;
  logic [13:0] act_threshold, act_return, act_guard, act_pulse_threshold;
  logic [11:0] act_window_total, act_window_pre;

  logic [31:0] config_version;
  logic [1:0]  pending_range;
  logic        range_pending;
  logic        range_seen_busy;

  // ---------------------------------------------------------------
  // Command decode (combinational)
  // ---------------------------------------------------------------
  // The command FIFO is implemented in block RAM.  Capture its output before
  // decoding so the RAMB36 -> protocol validation -> shadow-register path is
  // not required to settle in one 125 MHz cycle.
  logic [511:0] cmd_payload_q;
  logic [7:0]   cmd_len_q;
  logic         cmd_source_q;
  logic         cmd_pending;
  logic         dec_pending;
  wire          cmd_capture = cmd_valid && cmd_ready;
  wire          cmd_decode  = cmd_pending;
  wire          cmd_execute = dec_pending;

  logic [7:0]  frame_opcode;
  logic [7:0]  frame_target;
  logic [7:0]  frame_target_id;
  logic [15:0] frame_reg;
  logic [31:0] frame_transaction;
  logic [31:0] frame_value;
  logic [7:0]  dec_opcode;
  logic [31:0] dec_value;
  logic [15:0] dec_reg;
  logic [7:0]  dec_target;
  logic [7:0]  dec_target_id;
  logic [7:0]  cmd_len_exec;
  logic        frame_magic_ok;
  logic        frame_version_ok;
  logic        dec_source;

  logic [7:0]  frame_opcode_d;
  logic [7:0]  frame_target_d;
  logic [7:0]  frame_target_id_d;
  logic [15:0] frame_reg_d;
  logic [31:0] frame_transaction_d;
  logic [31:0] frame_value_d;
  logic [7:0]  dec_opcode_d;
  logic [31:0] dec_value_d;
  logic [15:0] dec_reg_d;
  logic [7:0]  dec_target_d;
  logic [7:0]  dec_target_id_d;

  always_comb begin
    frame_opcode_d      = cmd_payload_q[47:40];
    frame_target_d      = cmd_payload_q[55:48];
    frame_target_id_d   = cmd_payload_q[63:56];
    frame_reg_d         = {cmd_payload_q[71:64], cmd_payload_q[79:72]};
    frame_transaction_d = {cmd_payload_q[103:96], cmd_payload_q[111:104],
                           cmd_payload_q[119:112], cmd_payload_q[127:120]};
    frame_value_d       = {cmd_payload_q[135:128], cmd_payload_q[143:136],
                           cmd_payload_q[151:144], cmd_payload_q[159:152]};

    dec_opcode_d    = CMD_NONE;
    dec_value_d     = frame_value_d;
    dec_reg_d       = frame_reg_d;
    dec_target_d    = (cmd_len_q == 8'd20) ? frame_target_d : 8'h00;
    dec_target_id_d = (cmd_len_q == 8'd20) ? frame_target_id_d : 8'h00;

    if (cmd_len_q == 8'd20) begin
      unique case (frame_opcode_d)
        8'h01: dec_opcode_d = CMD_WRITE_SHADOW;
        8'h02: dec_opcode_d = CMD_READ;
        8'h03: dec_opcode_d = CMD_COMMIT;
        8'h04: dec_opcode_d = CMD_START;
        8'h05: dec_opcode_d = CMD_STOP;
        8'h06: dec_opcode_d = CMD_ABORT;
        8'h07: dec_opcode_d = CMD_CLEAR;
        default: dec_opcode_d = CMD_NONE;
      endcase
    end else begin
      // Legacy single-channel frames: 6 bytes for a plain command and
      // 10 bytes for the legacy WRITE_CONFIG payload.
      case (frame_opcode_d)
        8'h01: dec_opcode_d = CMD_READ_LEGACY;
        8'h02: begin
          dec_opcode_d = CMD_WRITE_SHADOW;
          dec_target_d = 8'h02;
          dec_reg_d    = 16'h0010; // 14-bit threshold register
          dec_value_d  = {16'd0, cmd_payload_q[63:48]};
        end
        8'h03: dec_opcode_d = CMD_COMMIT;
        8'h04: dec_opcode_d = CMD_START;
        8'h05: dec_opcode_d = CMD_STOP;
        8'h06: dec_opcode_d = CMD_ABORT;
        8'h07: dec_opcode_d = CMD_CLEAR;
        8'h08: dec_opcode_d = CMD_STATUS_LEGACY;
        8'h09: begin
          dec_opcode_d = CMD_SET_RANGE;
          dec_value_d  = {30'd0, cmd_payload_q[49:48]};
        end
        default: dec_opcode_d = CMD_NONE;
      endcase
      if (frame_opcode_d == 8'h02 && cmd_len_q == 8'd10) begin
        dec_value_d = {18'd0, cmd_payload_q[61:48]} |
                      ({16'd0, cmd_payload_q[63:56]} << 16);
      end
    end
  end

  // ---------------------------------------------------------------
  // Register writes
  // ---------------------------------------------------------------
  logic        write_hit;
  logic [7:0]  write_status;
  logic        write_valid;

  always_comb begin
    write_hit    = 1'b0;
    write_status = mca_pkg::RESP_OK;
    if (dec_opcode == CMD_WRITE_SHADOW) begin
      write_hit = 1'b1;
      if (dec_target == mca_pkg::TARGET_GLOBAL) begin
        case (dec_reg)
          mca_pkg::REG_GLOBAL_MODE: begin
            // The single-channel device implements the spectrum mode and the
            // waveform mode.  The historical multi-channel numbers stay
            // rejected so they can never be confused with a new mode.
            if ((dec_value != mca_pkg::MODE_SPECTRUM) &&
                (dec_value != mca_pkg::MODE_WAVEFORM) &&
                (dec_value != mca_pkg::MODE_EXTERNAL) &&
                (dec_value != mca_pkg::MODE_PSD)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end else if ((dec_value == mca_pkg::MODE_PSD) && dec_source) begin
              write_status = mca_pkg::RESP_BAD_VALUE; write_hit=1'b0;
            end else if ((dec_value != sh_mode) && sh_run_enable) begin
              // Mode switching is only allowed while stopped.
              write_status = mca_pkg::RESP_BUSY;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_GLOBAL_WAVE_TRIGGER_MODE:
            if (dec_value > 32'd1) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          // Opaque host-generated tag echoed in waveform fragments.
          mca_pkg::REG_GLOBAL_WAVE_SESSION: ;
          mca_pkg::REG_GLOBAL_EXT_DURATION_US: begin
            if ((dec_value == 0) || (dec_value > 32'd1_000_000)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_GLOBAL_BANDWIDTH_ENABLE: begin
            if (dec_value > 32'd1) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end else if (sh_run_enable || q_busy || range_busy || event_active) begin
              write_status = mca_pkg::RESP_BUSY;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_GLOBAL_PEAK_INTERPOLATION: begin
            if (dec_value > 32'd1) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end else if (sh_run_enable || q_busy || range_busy || event_active) begin
              write_status = mca_pkg::RESP_BUSY;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_GLOBAL_BASELINE_TRACKING:
            if (dec_value > 32'd1) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_GLOBAL_INTEGRATION_NS:
            if (dec_value == 0) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_GLOBAL_MANUAL_BASELINE:
            if (dec_value > 32'd16383) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_GLOBAL_MANUAL_BASELINE14:
            if (dec_value > 32'd16383) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_GLOBAL_RANGE: begin
            if ((dec_value != 32'd1) && (dec_value != 32'd2)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_GLOBAL_ABORT:
            if (dec_value > 32'd1) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_GLOBAL_BASELINE_AUTO:
            if (dec_value > 32'd1) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          default: begin
            write_status = mca_pkg::RESP_BAD_VALUE;
            write_hit    = 1'b0;
          end
        endcase
      end else if (dec_target == mca_pkg::TARGET_CHANNEL) begin
        if (dec_target_id > 8'd0) begin
          write_status = mca_pkg::RESP_BAD_TARGET;
          write_hit    = 1'b0;
        end else case (dec_reg)
          mca_pkg::REG_CH_CONTROL: begin
            if (dec_value > 32'd3) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_CH_THRESHOLD, mca_pkg::REG_CH_THRESHOLD14: begin
            if ((dec_value == 0) || (dec_value > 32'd16383)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_CH_RETURN, mca_pkg::REG_CH_RETURN14:
            if (dec_value > 32'd16383) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_GUARD, mca_pkg::REG_CH_GUARD14,
          mca_pkg::REG_CH_PULSE_THR14:
            if ((dec_value == 0) || (dec_value > 32'd16383)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_BASE_SHIFT, mca_pkg::REG_CH_FAST_SHIFT:
            if ((dec_value == 0) || (dec_value > 32'd28)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_PSD_START, mca_pkg::REG_CH_PSD_END:
            if (($signed(dec_value) < -32'sd1250) || ($signed(dec_value) > 32'sd1250)) begin
              write_status=mca_pkg::RESP_BAD_VALUE; write_hit=1'b0;
            end
          mca_pkg::REG_CH_WINDOW_TOTAL:
            if ((dec_value < WINDOW_MIN) || (dec_value > WINDOW_MAX)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_WINDOW_PRE:
            if (dec_value > (WINDOW_MAX - 1)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_WINDOW: begin
            if ((dec_value[15:0] == 0) || (dec_value[15:0] > WINDOW_MAX) ||
                (dec_value[31:16] > (WINDOW_MAX - 1))) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_CH_FAST_SAMPLES, mca_pkg::REG_CH_RECOVERY_SAMPLES,
          mca_pkg::REG_CH_OFFSET_QUALIFY: begin
            if ((dec_value == 0) || (dec_value > 32'd65535)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          end
          mca_pkg::REG_CH_BASE_PULSE_THR:
            if ((dec_value == 0) || (dec_value > 32'd4095)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_BASE_HOLD:
            if (dec_value > 32'd4096) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          mca_pkg::REG_CH_RECOVERY_TOLERANCE:
            if ((dec_value == 0) || (dec_value > 32'd16383)) begin
              write_status = mca_pkg::RESP_BAD_VALUE;
              write_hit    = 1'b0;
            end
          default: begin
            write_status = mca_pkg::RESP_BAD_VALUE;
            write_hit    = 1'b0;
          end
        endcase
      end else begin
        // ADC/module/pair configuration does not exist on this device.
        write_status = mca_pkg::RESP_BAD_TARGET;
        write_hit    = 1'b0;
      end
    end
    if (dec_opcode==CMD_WRITE_SHADOW && sh_run_enable && sh_mode==mca_pkg::MODE_PSD &&
        ((dec_target==mca_pkg::TARGET_CHANNEL &&
          (dec_reg==mca_pkg::REG_CH_WINDOW_TOTAL || dec_reg==mca_pkg::REG_CH_WINDOW_PRE ||
           dec_reg==mca_pkg::REG_CH_PSD_START || dec_reg==mca_pkg::REG_CH_PSD_END || dec_reg==mca_pkg::REG_CH_WINDOW)) ||
         (dec_target==mca_pkg::TARGET_GLOBAL && dec_reg==mca_pkg::REG_GLOBAL_INTEGRATION_NS))) begin
      write_hit=0;write_status=mca_pkg::RESP_BUSY;
    end
  end

  assign write_valid = write_hit;

  // ---------------------------------------------------------------
  // Register reads
  // ---------------------------------------------------------------
  logic        read_hit;
  logic [31:0] read_value;
  logic [7:0]  read_status;

  always_comb begin
    read_hit    = 1'b0;
    read_value  = 32'd0;
    read_status = mca_pkg::RESP_OK;
    if ((dec_opcode == CMD_READ) || (dec_opcode == CMD_READ_LEGACY)) begin
      if (dec_opcode == CMD_READ_LEGACY) begin
        read_hit   = 1'b1;
        read_value = {18'd0, act_threshold};
      end else if (dec_target == mca_pkg::TARGET_GLOBAL) begin
        read_hit = 1'b1;
        case (dec_reg)
          mca_pkg::REG_GLOBAL_MODE:            read_value = sh_mode;
          mca_pkg::REG_GLOBAL_BASELINE_TRACKING:
            read_value = {31'd0, sh_baseline_auto};
          mca_pkg::REG_GLOBAL_INTEGRATION_NS:
            read_value = {20'd0, sh_window_total} * SAMPLE_NS;
          mca_pkg::REG_GLOBAL_MANUAL_BASELINE:
            read_value = {18'd0, sh_manual_baseline};
          mca_pkg::REG_GLOBAL_MANUAL_BASELINE14:
            read_value = {18'd0, sh_manual_baseline};
          mca_pkg::REG_GLOBAL_RANGE:
            read_value = {30'd0, range_applied_value};
          mca_pkg::REG_GLOBAL_ABORT:
            read_value = {31'd0, q_busy};
          mca_pkg::REG_GLOBAL_BASELINE_AUTO:
            read_value = {31'd0, sh_baseline_auto};
          mca_pkg::REG_GLOBAL_WAVE_TRIGGER_MODE:
            read_value = sh_wave_single;
          mca_pkg::REG_GLOBAL_WAVE_SESSION:
            read_value = sh_wave_session;
          mca_pkg::REG_GLOBAL_EXT_DURATION_US:
            read_value = {12'd0, sh_ext_duration_us};
          mca_pkg::REG_GLOBAL_BANDWIDTH_ENABLE:
            read_value = {31'd0, sh_filter_enable};
          mca_pkg::REG_GLOBAL_PEAK_INTERPOLATION:
            read_value = {31'd0, sh_peak_interpolation};
          mca_pkg::REG_FRONTEND_STATUS:
            read_value = {26'd0, peak_interpolation_enabled, init_done, filter_enabled,
                          range_busy, range_applied_value};
          mca_pkg::REG_WAVE_STATUS:
            read_value = wave_status;
          mca_pkg::REG_WAVE_LAST_CAPTURE:
            read_value = wave_last_capture;
          mca_pkg::REG_WAVE_CAPTURE_COUNT:
            read_value = wave_capture_count;
          mca_pkg::REG_EXT_STATUS:
            read_value = {30'd0, external_window_active, external_armed};
          mca_pkg::REG_EXT_TRIGGER_COUNT:
            read_value = external_trigger_count;

          mca_pkg::REG_CAP_ADC_COUNT:      read_value = 32'd1;
          mca_pkg::REG_CAP_MAX_ADC_COUNT:  read_value = 32'd1;
          mca_pkg::REG_CAP_CH_COUNT:       read_value = 32'd1;
          mca_pkg::REG_CAP_MAX_CH_COUNT:   read_value = 32'd1;
          mca_pkg::REG_CAP_MODULE_COUNT:   read_value = 32'd0;
          mca_pkg::REG_CAP_MAX_MODULE:     read_value = 32'd0;
          mca_pkg::REG_CAP_PAIR_COUNT:     read_value = 32'd0;
          mca_pkg::REG_CAP_SAMPLE_RATE:    read_value = SAMPLE_RATE_HZ;
          mca_pkg::REG_CAP_FIRMWARE:       read_value = FIRMWARE_VERSION;
          mca_pkg::REG_CAP_CONFIG_VERSION: read_value = config_version;
          mca_pkg::REG_CAP_FORMAT_FLAGS:
            read_value = mca_pkg::FMT_FLAG_EXTENDED_QUERY |
                         mca_pkg::FMT_FLAG_MANUAL_BASELINE |
                          mca_pkg::FMT_FLAG_EXTENDED_STATUS |
                          mca_pkg::FMT_FLAG_PEAK_ABSOLUTE |
                         mca_pkg::FMT_FLAG_BASELINE_TLV16 |
                         mca_pkg::FMT_FLAG_WAVEFORM |
                         mca_pkg::FMT_FLAG_FRONTEND_STATE |
                         mca_pkg::FMT_FLAG_PEAK_INTERPOLATION |
                         mca_pkg::FMT_FLAG_EXTERNAL_TRIGGER | mca_pkg::FMT_FLAG_PSD;
          mca_pkg::REG_CAP_ADC_BITS:      read_value = 32'd14;
          mca_pkg::REG_CAP_PEAK_Q_BITS:   read_value = 32'd2;
          mca_pkg::REG_CAP_INTEGRAL_BITS: read_value = 32'd24;
          mca_pkg::REG_CAP_INTEGRAL_WINDOW_MIN_NS:
            read_value = WINDOW_MIN * SAMPLE_NS;
          mca_pkg::REG_CAP_INTEGRAL_WINDOW_MAX_NS:
            read_value = WINDOW_MAX * SAMPLE_NS;
          mca_pkg::REG_CAP_PSD_MAX_NS: read_value = 32'd10000;
          mca_pkg::REG_CAP_TIMEBASE_PS:   read_value = SAMPLE_NS * 1000;
          default: begin
            read_status = mca_pkg::RESP_BAD_VALUE;
            read_hit    = 1'b0;
          end
        endcase
      end else if (dec_target == mca_pkg::TARGET_CHANNEL) begin
        if (dec_target_id > 8'd0) begin
          read_status = mca_pkg::RESP_BAD_TARGET;
        end else begin
          read_hit = 1'b1;
          case (dec_reg)
            mca_pkg::REG_CH_CONTROL:        read_value = {31'd0, sh_run_enable};
            mca_pkg::REG_CH_THRESHOLD,
            mca_pkg::REG_CH_THRESHOLD14:    read_value = {18'd0, sh_threshold};
            mca_pkg::REG_CH_RETURN,
            mca_pkg::REG_CH_RETURN14:       read_value = {18'd0, sh_return};
            mca_pkg::REG_CH_GUARD,
            mca_pkg::REG_CH_GUARD14:        read_value = {18'd0, sh_guard};
            mca_pkg::REG_CH_PULSE_THR14:    read_value = {18'd0, sh_pulse_threshold};
            mca_pkg::REG_CH_BASE_PULSE_THR: read_value = {18'd0, sh_pulse_threshold};
            mca_pkg::REG_CH_BASE_SHIFT:     read_value = {26'd0, sh_track_shift};
            mca_pkg::REG_CH_FAST_SHIFT:     read_value = {26'd0, sh_fast_shift};
            mca_pkg::REG_CH_WINDOW:         read_value = {16'd0, sh_window_pre} |
                                                        ({16'd0, sh_window_total} << 16);
            mca_pkg::REG_CH_PSD_START: read_value = {{16{sh_psd_start[15]}},sh_psd_start};
            mca_pkg::REG_CH_PSD_END: read_value = {{16{sh_psd_end[15]}},sh_psd_end};
            mca_pkg::REG_CH_WINDOW_TOTAL:   read_value = {20'd0, sh_window_total};
            mca_pkg::REG_CH_WINDOW_PRE:     read_value = {20'd0, sh_window_pre};
            mca_pkg::REG_CH_FAST_SAMPLES:   read_value = {16'd0, sh_fast_samples};
            mca_pkg::REG_CH_RECOVERY_SAMPLES:
              read_value = {16'd0, sh_recovery_samples};
            mca_pkg::REG_CH_OFFSET_QUALIFY: read_value = {16'd0, sh_offset_qualify};
            mca_pkg::REG_CH_BASE_HOLD:      read_value = {16'd0, sh_pulse_hold};
            mca_pkg::REG_CH_RECOVERY_TOLERANCE:
              read_value = {18'd0, sh_recovery_tolerance};
            // Active (applied) mirrors, useful for commit verification.
            16'h0020: read_value = {18'd0, act_threshold};
            16'h0021: read_value = {18'd0, act_return};
            16'h0022: read_value = {18'd0, act_guard};
            16'h0023: read_value = {18'd0, act_pulse_threshold};
            16'h0024: read_value = {20'd0, act_window_total};
            16'h0025: read_value = {20'd0, act_window_pre};
            default: begin
              read_status = mca_pkg::RESP_BAD_VALUE;
              read_hit    = 1'b0;
            end
          endcase
        end
      end else if (dec_target == mca_pkg::TARGET_ADC) begin
        if (dec_target_id > 8'd0) begin
          read_status = mca_pkg::RESP_BAD_TARGET;
        end else begin
          read_hit   = 1'b1;
          read_value = {18'd0, act_threshold};
        end
      end else begin
        read_status = mca_pkg::RESP_BAD_TARGET;
      end
    end
  end

  // ---------------------------------------------------------------
  // Commit validation
  // ---------------------------------------------------------------
  logic        commit_accept;
  logic [7:0]  commit_status;
  logic [31:0] commit_version_value;

  // Keep arithmetic out of the command-response enable cone. Stage 1 forms
  // bounds and captures the same shadow snapshot; stage 2 compares only flops.
  // Frame intake -> decode -> command execution and response retirement ensure
  // at least two clocks after a shadow write before the next COMMIT/START.
  logic signed [16:0] psd_min_start_q, psd_post_limit_q;
  logic signed [15:0] psd_check_start_q, psd_check_end_q;
  logic [11:0] psd_check_total_q;
  logic psd_check_peak_q, psd_bounds_bad_q;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      psd_min_start_q<=0; psd_post_limit_q<=17'sd32;
      psd_check_start_q<=16'sd7; psd_check_end_q<=16'sd150;
      psd_check_total_q<=12'd32; psd_check_peak_q<=0;
      psd_bounds_bad_q<=1'b1;
    end else begin
      psd_min_start_q <= -$signed({1'b0,sh_window_pre});
      psd_post_limit_q <= $signed({1'b0,sh_window_total})-
                          $signed({1'b0,sh_window_pre});
      psd_check_start_q<=sh_psd_start; psd_check_end_q<=sh_psd_end;
      psd_check_total_q<=sh_window_total; psd_check_peak_q<=sh_peak_interpolation;
      psd_bounds_bad_q <= (psd_check_total_q>12'd1250) || psd_check_peak_q ||
          ($signed(psd_check_start_q)<psd_min_start_q) ||
          ($signed(psd_check_end_q)>psd_post_limit_q) ||
          (psd_check_start_q>=psd_check_end_q);
    end
  end

  always_comb begin
    commit_accept        = 1'b1;
    commit_status        = mca_pkg::RESP_OK;
    commit_version_value = config_version;
    // Guard < pulse threshold < trigger threshold keeps the estimator from
    // treating the trigger band as a persistent offset.
    if (sh_baseline_auto && (sh_pulse_threshold < 14'd16)) begin
      commit_accept = 1'b0;
      commit_status = mca_pkg::RESP_VALIDATION_FAILED;
    end else if ((sh_guard >= sh_pulse_threshold) ||
        (sh_pulse_threshold >= sh_threshold)) begin
      commit_accept = 1'b0;
      commit_status = mca_pkg::RESP_VALIDATION_FAILED;
    end else if ((sh_filter_enable != filter_enabled) &&
                 (sh_run_enable || q_busy || range_busy)) begin
      commit_accept = 1'b0;
      commit_status = mca_pkg::RESP_BUSY;
    end else if ((sh_mode == mca_pkg::MODE_PSD) && psd_bounds_bad_q) begin
      commit_accept=1'b0; commit_status=mca_pkg::RESP_VALIDATION_FAILED;
    end else if (sh_window_pre >= sh_window_total) begin
      commit_accept = 1'b0;
      commit_status = mca_pkg::RESP_VALIDATION_FAILED;
    end
  end

  // ---------------------------------------------------------------
  // Command execution
  // ---------------------------------------------------------------
  logic        commit_wait;
  logic [7:0]  latched_transaction_opcode;
  logic [7:0]  latched_target, latched_target_id;
  logic [15:0] latched_reg;
  logic [31:0] latched_transaction;
  logic [31:0] latched_value;
  logic [7:0]  latched_status;
  logic [7:0]  latched_len;
  logic        latched_source;

  logic        cache_valid;
  logic [31:0] cache_transaction;
  logic [7:0]  cache_opcode;
  logic        cache_source;
  logic [519:0] cache_response;
  logic [7:0]  cache_len;

  // A modifying transport owns the single configuration bank until STOP.
  // Reads remain available to both transports.  While stopped, five seconds
  // without traffic from the owner also releases it.
  localparam int OWNER_TIMEOUT_CYCLES = SAMPLE_RATE_HZ * 5;
  localparam int OWNER_TIMER_W = $clog2(OWNER_TIMEOUT_CYCLES + 1);
  logic [1:0] control_owner; // 0 none, 1 UDP, 2 UART
  logic [OWNER_TIMER_W-1:0] owner_idle;
  wire dec_modifying = (dec_opcode != CMD_READ) &&
                       (dec_opcode != CMD_READ_LEGACY) &&
                       (dec_opcode != CMD_STATUS_LEGACY);
  wire [1:0] dec_owner = dec_source ? 2'd2 : 2'd1;
  wire owner_conflict = dec_modifying && (control_owner != 2'd0) &&
                        (control_owner != dec_owner);

  function automatic logic [519:0] build_response(
      input logic [31:0] transaction,
      input logic [7:0]  opcode,
      input logic [7:0]  target,
      input logic [7:0]  target_id,
      input logic [7:0]  status,
      input logic [15:0] reg_addr,
      input logic [31:0] value);
    logic [519:0] resp_bits;
    begin
      resp_bits = '0;
      resp_bits[7:0]      = transaction[31:24];
      resp_bits[15:8]     = transaction[23:16];
      resp_bits[23:16]    = transaction[15:8];
      resp_bits[31:24]    = transaction[7:0];
      resp_bits[39:32]    = opcode;
      resp_bits[47:40]    = target;
      resp_bits[55:48]    = target_id;
      resp_bits[63:56]    = status;
      resp_bits[71:64]    = reg_addr[15:8];
      resp_bits[79:72]    = reg_addr[7:0];
      resp_bits[103:96]   = value[31:24];
      resp_bits[111:104]  = value[23:16];
      resp_bits[119:112]  = value[15:8];
      resp_bits[127:120]  = value[7:0];
      build_response = resp_bits;
    end
  endfunction

  // ---------------------------------------------------------------
  // Configuration bus assembly
  // ---------------------------------------------------------------
  always_comb begin
    cfg_bus = '0;
    cfg_bus[13:0]    = sh_threshold;
    cfg_bus[27:14]   = sh_return;
    cfg_bus[39:28]   = sh_window_total;
    cfg_bus[51:40]   = sh_window_pre;
    cfg_bus[65:52]   = sh_guard;
    cfg_bus[79:66]   = sh_pulse_threshold;
    cfg_bus[85:80]   = sh_fast_shift;
    cfg_bus[91:86]   = sh_track_shift;
    cfg_bus[97:92]   = sh_recovery_shift;
    cfg_bus[113:98]  = sh_fast_samples;
    cfg_bus[127:114] = sh_recovery_samples;
    cfg_bus[143:128] = sh_manual_baseline;
    cfg_bus[144]     = sh_baseline_auto;
    cfg_bus[145]     = sh_run_enable;
    // Shared polarity: the discriminator of both the spectrum and the
    // waveform path reads this one bit.
    cfg_bus[146]     = sh_polarity_negative;
    cfg_bus[147]     = (sh_mode == mca_pkg::MODE_WAVEFORM);
    cfg_bus[148]     = sh_wave_single[0];
    cfg_bus[181:150] = sh_wave_session[31:0];
    cfg_bus[197:182] = sh_pulse_hold;
    cfg_bus[213:198] = sh_offset_qualify;
    cfg_bus[214]     = sh_filter_enable;
    cfg_bus[228:215] = sh_recovery_tolerance;
    cfg_bus[229]     = (sh_mode == mca_pkg::MODE_EXTERNAL);
    cfg_bus[249:230] = sh_ext_duration_us;
    cfg_bus[250]     = sh_peak_interpolation;
    cfg_bus[251] = (sh_mode == mca_pkg::MODE_PSD);
    cfg_bus[263:252] = sh_window_pre + sh_psd_start;
    cfg_bus[275:264] = sh_window_pre + sh_psd_end;
  end

  assign range_request_value  = pending_range;
  assign range_request_strobe = range_pending && !range_busy && !range_seen_busy;

  // ---------------------------------------------------------------
  // Sequencer
  // ---------------------------------------------------------------
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      sh_threshold         <= 14'd64;
      sh_return            <= 14'd32;
      sh_guard             <= 14'd16;
      sh_pulse_threshold   <= 14'd32;
      sh_window_total      <= 12'd32;
      sh_window_pre        <= 12'd0;
      sh_fast_shift        <= 6'd4;
      sh_track_shift       <= 6'd8;
      // The slow rate carries 24 fractional bits, so a 12-bit shift still
      // resolves a one-code error: the accumulator never truncates the
      // update to zero the way a 4-bit shift would.
      sh_recovery_shift    <= 6'd8;
      sh_fast_samples      <= 16'd1024;
      sh_recovery_samples  <= 16'd1024;
      sh_offset_qualify    <= 16'd4096;
      sh_pulse_hold        <= 16'd64;
      sh_recovery_tolerance<= 14'd32;
      sh_manual_baseline   <= 14'd8192;
      sh_baseline_auto     <= 1'b0;
      sh_run_enable        <= 1'b0;
      sh_filter_enable     <= 1'b0;
      sh_peak_interpolation<= 1'b0;
      sh_polarity_negative <= 1'b0;
      sh_mode              <= mca_pkg::MODE_SPECTRUM;
      sh_psd_start<=16'sd7; sh_psd_end<=16'sd150;
      sh_wave_single       <= 32'd0;
      sh_wave_session      <= 32'd0;
      sh_ext_duration_us   <= 20'd1000;
      act_config_version   <= CONFIG_VERSION_BASE;
      act_threshold        <= 14'd64;
      act_return           <= 14'd32;
      act_guard            <= 14'd16;
      act_pulse_threshold  <= 14'd32;
      act_window_total     <= 12'd32;
      act_window_pre       <= 12'd0;
      config_version       <= CONFIG_VERSION_BASE;
      pending_range        <= 2'b00;
      range_pending        <= 1'b0;
      range_seen_busy      <= 1'b0;
      cfg_toggle           <= 1'b0;
      commit_wait          <= 1'b0;
      abort_toggle         <= 1'b0;
      clear_counts_toggle  <= 1'b0;
      resp_valid           <= 1'b0;
      resp_payload         <= '0;
      resp_len             <= '0;
      cache_valid          <= 1'b0;
      cache_transaction    <= '0;
      cache_opcode         <= '0;
      cache_source         <= 1'b0;
      cache_response       <= '0;
      cache_len            <= '0;
      cmd_payload_q        <= '0;
      cmd_len_q            <= '0;
      cmd_source_q         <= 1'b0;
      cmd_pending          <= 1'b0;
      dec_pending          <= 1'b0;
      frame_opcode         <= '0;
      frame_target         <= '0;
      frame_target_id      <= '0;
      frame_reg            <= '0;
      frame_transaction    <= '0;
      frame_value          <= '0;
      dec_opcode           <= CMD_NONE;
      dec_value            <= '0;
      dec_reg              <= '0;
      dec_target           <= '0;
      dec_target_id        <= '0;
      cmd_len_exec         <= '0;
      frame_magic_ok       <= 1'b0;
      frame_version_ok     <= 1'b0;
      dec_source           <= 1'b0;
      latched_transaction_opcode <= '0;
      latched_target       <= '0;
      latched_target_id    <= '0;
      latched_reg          <= '0;
      latched_transaction  <= '0;
      latched_value        <= '0;
      latched_status       <= '0;
      latched_len          <= '0;
      latched_source       <= 1'b0;
      control_owner        <= 2'd0;
      owner_idle           <= '0;
    end else begin
      if (control_owner != 2'd0 && !sh_run_enable) begin
        if (owner_idle == OWNER_TIMEOUT_CYCLES - 1) begin
          control_owner <= 2'd0;
          owner_idle    <= '0;
        end else begin
          owner_idle <= owner_idle + 1'b1;
        end
      end else begin
        owner_idle <= '0;
      end
      if (resp_valid && resp_ready)
        resp_valid <= 1'b0;

      if (range_pending && range_busy)
        range_seen_busy <= 1'b1;
      if (range_pending && range_seen_busy && !range_busy)
        range_pending <= 1'b0;

      // Commit apply handshake.
      if (commit_wait && cfg_ack) begin
        commit_wait        <= 1'b0;
        config_version     <= applied_config_version;
        act_config_version <= applied_config_version;
        act_threshold      <= sh_threshold;
        act_return         <= sh_return;
        act_guard          <= sh_guard;
        act_pulse_threshold<= sh_pulse_threshold;
        act_window_total   <= sh_window_total;
        act_window_pre     <= sh_window_pre;
        resp_payload       <= build_response(latched_transaction,
                                             latched_transaction_opcode,
                                             latched_target,
                                             latched_target_id,
                                             mca_pkg::RESP_OK,
                                             latched_reg,
                                             applied_config_version);
        resp_len           <= 8'd16;
        resp_valid         <= 1'b1;
        cache_valid        <= 1'b1;
        cache_transaction  <= latched_transaction;
        cache_opcode       <= latched_transaction_opcode;
        cache_source       <= latched_source;
        cache_response     <= build_response(latched_transaction,
                                             latched_transaction_opcode,
                                             latched_target,
                                             latched_target_id,
                                             mca_pkg::RESP_OK,
                                             latched_reg,
                                             applied_config_version);
        cache_len          <= 8'd16;
        if (latched_transaction_opcode == CMD_STOP)
          control_owner <= 2'd0;
      end

      if (cmd_capture) begin
        cmd_payload_q <= cmd_payload;
        cmd_len_q     <= cmd_len;
        cmd_source_q  <= cmd_source;
        cmd_pending   <= 1'b1;
      end

      if (cmd_decode) begin
        cmd_pending       <= 1'b0;
        dec_pending       <= 1'b1;
        frame_opcode      <= frame_opcode_d;
        frame_target      <= frame_target_d;
        frame_target_id   <= frame_target_id_d;
        frame_reg         <= frame_reg_d;
        frame_transaction <= frame_transaction_d;
        frame_value       <= frame_value_d;
        dec_opcode        <= dec_opcode_d;
        dec_value         <= dec_value_d;
        dec_reg           <= dec_reg_d;
        dec_target        <= dec_target_d;
        dec_target_id     <= dec_target_id_d;
        cmd_len_exec      <= cmd_len_q;
        frame_magic_ok    <= ({cmd_payload_q[7:0], cmd_payload_q[15:8],
                               cmd_payload_q[23:16], cmd_payload_q[31:24]} ==
                              mca_pkg::MCF2_MAGIC32);
        frame_version_ok  <= (cmd_payload_q[39:32] == 8'd2);
        dec_source        <= cmd_source_q;
      end

      if (cmd_execute) begin
        dec_pending                <= 1'b0;
        latched_transaction       <= frame_transaction;
        latched_transaction_opcode<= (cmd_len_exec == 8'd20) ? frame_opcode : dec_opcode;
        latched_target            <= dec_target;
        latched_target_id         <= dec_target_id;
        latched_reg               <= dec_reg;
        latched_value             <= dec_value;
        latched_source            <= dec_source;

        if (control_owner == dec_owner)
          owner_idle <= '0;

        if (cache_valid && (cache_source == dec_source) &&
            (cache_transaction == frame_transaction) &&
            (cache_opcode == ((cmd_len_exec == 8'd20) ? frame_opcode : dec_opcode))) begin
          // Idempotent replay: the host retried the same transaction.
          resp_payload <= cache_response;
          resp_len     <= cache_len;
          resp_valid   <= 1'b1;
        end else if (cmd_len_exec == 8'd20 && !frame_magic_ok) begin
          resp_payload <= build_response(frame_transaction, frame_opcode,
                                         frame_target, frame_target_id,
                                         mca_pkg::RESP_BAD_FRAME, frame_reg, 32'd0);
          resp_len     <= 8'd16;
          resp_valid   <= 1'b1;
          cache_valid  <= 1'b1;
          cache_transaction <= frame_transaction;
          cache_opcode <= frame_opcode;
          cache_source <= dec_source;
          cache_response <= build_response(frame_transaction, frame_opcode,
                                           frame_target, frame_target_id,
                                           mca_pkg::RESP_BAD_FRAME, frame_reg, 32'd0);
          cache_len    <= 8'd16;
        end else if (cmd_len_exec == 8'd20 && !frame_version_ok) begin
          resp_payload <= build_response(frame_transaction, frame_opcode,
                                         frame_target, frame_target_id,
                                         mca_pkg::RESP_BAD_FRAME, frame_reg, 32'd0);
          resp_len     <= 8'd16;
          resp_valid   <= 1'b1;
        end else if (cmd_len_exec != 8'd20 && cmd_len_exec != 8'd6 && cmd_len_exec != 8'd10) begin
          resp_payload <= build_response(frame_transaction, frame_opcode,
                                         frame_target, frame_target_id,
                                         mca_pkg::RESP_BAD_FRAME, frame_reg, 32'd0);
          resp_len     <= 8'd16;
          resp_valid   <= 1'b1;
        end else if (owner_conflict) begin
          resp_payload <= build_response(frame_transaction, frame_opcode,
                                         dec_target, dec_target_id,
                                         mca_pkg::RESP_BUSY, dec_reg, 32'd0);
          resp_len     <= 8'd16;
          resp_valid   <= 1'b1;
        end else begin
          if (dec_modifying && control_owner == 2'd0)
            control_owner <= dec_owner;
          // Global registers whose effect is an event rather than a stored
          // value are handled before the per-opcode dispatch.
          if (dec_opcode == CMD_WRITE_SHADOW &&
              (dec_target == mca_pkg::TARGET_GLOBAL)) begin
            if (dec_reg == mca_pkg::REG_GLOBAL_ABORT)
              abort_toggle <= ~abort_toggle;
            else if (dec_reg == mca_pkg::REG_GLOBAL_RANGE) begin
              pending_range   <= dec_value[1:0];
              range_pending   <= 1'b1;
              range_seen_busy <= 1'b0;
            end
          end

          unique case (dec_opcode)
            CMD_WRITE_SHADOW: begin
              if (commit_wait) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_BUSY, dec_reg, 32'd0);
                resp_len   <= 8'd16;
                resp_valid <= 1'b1;
              end else if (!write_hit) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               write_status, dec_reg, 32'd0);
                resp_len   <= 8'd16;
                resp_valid <= 1'b1;
              end else begin
                case (dec_target)
                  mca_pkg::TARGET_GLOBAL: begin
                    case (dec_reg)
                      mca_pkg::REG_GLOBAL_BASELINE_TRACKING:
                        sh_baseline_auto <= dec_value[0];
                      mca_pkg::REG_GLOBAL_BASELINE_AUTO:
                        sh_baseline_auto <= dec_value[0];
                      mca_pkg::REG_GLOBAL_MANUAL_BASELINE:
                        sh_manual_baseline <= dec_value[13:0];
                      mca_pkg::REG_GLOBAL_MANUAL_BASELINE14:
                        sh_manual_baseline <= dec_value[13:0];
                      mca_pkg::REG_GLOBAL_MODE:
                        sh_mode <= dec_value;
                      mca_pkg::REG_GLOBAL_WAVE_TRIGGER_MODE:
                        sh_wave_single <= {31'd0, dec_value[0]};
                      mca_pkg::REG_GLOBAL_WAVE_SESSION:
                        sh_wave_session <= dec_value;
                      mca_pkg::REG_GLOBAL_EXT_DURATION_US:
                        sh_ext_duration_us <= dec_value[19:0];
                      mca_pkg::REG_GLOBAL_BANDWIDTH_ENABLE:
                        sh_filter_enable <= dec_value[0];
                      mca_pkg::REG_GLOBAL_PEAK_INTERPOLATION:
                        sh_peak_interpolation <= dec_value[0];
                      mca_pkg::REG_GLOBAL_INTEGRATION_NS: begin
                        logic [31:0] samples;
                        samples = dec_value / SAMPLE_NS;
                        if (samples < WINDOW_MIN) samples = WINDOW_MIN;
                        if (samples > WINDOW_MAX) samples = WINDOW_MAX;
                        sh_window_total <= samples[11:0];
                        if (sh_window_pre >= samples[11:0])
                          sh_window_pre <= samples[11:0] - 1'b1;
                      end
                      default: ;
                    endcase
                  end
                  mca_pkg::TARGET_CHANNEL: begin
                    case (dec_reg)
                      mca_pkg::REG_CH_CONTROL:
                        // bit0 enables the channel, bit1 selects the negative
                        // polarity.  Both bits take effect on commit.
                        sh_polarity_negative <= dec_value[1];
                      mca_pkg::REG_CH_THRESHOLD,
                      mca_pkg::REG_CH_THRESHOLD14:
                        sh_threshold <= dec_value[13:0];
                      mca_pkg::REG_CH_RETURN,
                      mca_pkg::REG_CH_RETURN14:
                        sh_return <= dec_value[13:0];
                      mca_pkg::REG_CH_GUARD,
                      mca_pkg::REG_CH_GUARD14:
                        sh_guard <= dec_value[13:0];
                      mca_pkg::REG_CH_PULSE_THR14,
                      mca_pkg::REG_CH_BASE_PULSE_THR:
                        sh_pulse_threshold <= dec_value[13:0];
                      mca_pkg::REG_CH_FAST_SHIFT:
                        sh_fast_shift <= dec_value[5:0];
                      mca_pkg::REG_CH_BASE_SHIFT: begin
                        sh_track_shift    <= dec_value[5:0];
                        sh_recovery_shift <= dec_value[5:0];
                      end
                      mca_pkg::REG_CH_WINDOW: begin
                        sh_window_pre   <= dec_value[27:16];
                        sh_window_total <= dec_value[11:0];
                      end
                      mca_pkg::REG_CH_WINDOW_TOTAL: begin
                        sh_window_total <= dec_value[11:0];
                        if (sh_window_pre >= dec_value[11:0])
                          sh_window_pre <= dec_value[11:0] - 1'b1;
                      end
                      mca_pkg::REG_CH_PSD_START: sh_psd_start<=dec_value[15:0];
                      mca_pkg::REG_CH_PSD_END: sh_psd_end<=dec_value[15:0];
                      mca_pkg::REG_CH_WINDOW_PRE:
                        sh_window_pre <= dec_value[11:0];
                      mca_pkg::REG_CH_FAST_SAMPLES:
                        sh_fast_samples <= dec_value[15:0];
                      mca_pkg::REG_CH_RECOVERY_SAMPLES:
                        sh_recovery_samples <= dec_value[15:0];
                      mca_pkg::REG_CH_OFFSET_QUALIFY:
                        sh_offset_qualify <= dec_value[15:0];
                      mca_pkg::REG_CH_BASE_HOLD:
                        sh_pulse_hold <= dec_value[15:0];
                      mca_pkg::REG_CH_RECOVERY_TOLERANCE:
                        sh_recovery_tolerance <= dec_value[13:0];
                      default: ;
                    endcase
                  end
                  default: ;
                endcase
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_OK, dec_reg, dec_value);
                resp_len     <= 8'd16;
                resp_valid   <= 1'b1;
                cache_valid  <= 1'b1;
                cache_transaction <= frame_transaction;
                cache_opcode <= (cmd_len_exec == 8'd20) ? frame_opcode : dec_opcode;
                cache_source <= dec_source;
                cache_response <= build_response(frame_transaction, frame_opcode,
                                                 dec_target, dec_target_id,
                                                 mca_pkg::RESP_OK, dec_reg, dec_value);
                cache_len    <= 8'd16;
              end
            end

            CMD_READ, CMD_READ_LEGACY: begin
              if (read_hit) begin
                if (dec_opcode == CMD_READ_LEGACY) begin
                  // Legacy 5-byte response: magic, version, status, transaction,
                  // then the value.
                  resp_payload <= '0;
                  resp_payload[7:0]   <= 8'h52;
                  resp_payload[15:8]  <= 8'h02;
                  resp_payload[23:16] <= mca_pkg::RESP_OK;
                  resp_payload[31:24] <= frame_transaction[7:0];
                  resp_payload[55:48] <= read_value[13:8];
                  resp_payload[63:56] <= read_value[7:0];
                  resp_len   <= 8'd8;
                  resp_valid <= 1'b1;
                end else begin
                  resp_payload <= build_response(frame_transaction, frame_opcode,
                                                 dec_target, dec_target_id,
                                                 mca_pkg::RESP_OK, dec_reg,
                                                 read_value);
                  resp_len     <= 8'd16;
                  resp_valid   <= 1'b1;
                  cache_valid  <= 1'b1;
                  cache_transaction <= frame_transaction;
                  cache_opcode <= frame_opcode;
                  cache_source <= dec_source;
                  cache_response <= build_response(frame_transaction, frame_opcode,
                                                   dec_target, dec_target_id,
                                                   mca_pkg::RESP_OK, dec_reg,
                                                   read_value);
                  cache_len    <= 8'd16;
                end
              end else begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               read_status, dec_reg, 32'd0);
                resp_len     <= 8'd16;
                resp_valid   <= 1'b1;
              end
            end

            CMD_COMMIT: begin
              if (commit_wait) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_BUSY, dec_reg, 32'd0);
                resp_len   <= 8'd16;
                resp_valid <= 1'b1;
              end else if ((dec_value != 32'd0) && (dec_value != config_version)) begin
                // A non-zero value declares the version the host expects to
                // replace; a plain commit-all sends zero.
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_CONFIG_VERSION_MISMATCH,
                                               dec_reg, config_version);
                resp_len   <= 8'd16;
                resp_valid <= 1'b1;
              end else if (!commit_accept) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               commit_status, dec_reg, 32'd0);
                resp_len   <= 8'd16;
                resp_valid <= 1'b1;
              end else begin
                cfg_toggle  <= ~cfg_toggle;
                commit_wait <= 1'b1;
              end
            end

            CMD_START: begin
              if ((sh_mode == mca_pkg::MODE_PSD) && (dec_source || !commit_accept)) begin
                resp_payload<=build_response(frame_transaction,frame_opcode,dec_target,dec_target_id,
                    mca_pkg::RESP_VALIDATION_FAILED,dec_reg,32'd0);
                resp_len<=8'd16; resp_valid<=1'b1;
              end else if (!frontend_ready || (!baseline_ready_in && sh_baseline_auto)) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_NOT_READY, dec_reg, 32'd0);
                resp_len   <= 8'd16;
                resp_valid <= 1'b1;
              end else begin
                // START is a configuration transaction.  Reply only after
                // the sampling domain has applied it; an immediate reply made
                // the host observe the old stopped state and retry/toggle UI.
                sh_run_enable <= 1'b1;
                cfg_toggle    <= ~cfg_toggle;
                commit_wait   <= 1'b1;
              end
            end

            CMD_STOP: begin
              // Like START, STOP completes only when its clock-domain crossing
              // is acknowledged.  cmd_ready remains low meanwhile, preventing
              // repeated UI requests from collapsing two toggle handshakes.
              sh_run_enable <= 1'b0;
              cfg_toggle    <= ~cfg_toggle;
              commit_wait   <= 1'b1;
            end

            CMD_ABORT: begin
              cfg_toggle <= ~cfg_toggle;
              resp_payload <= build_response(frame_transaction, frame_opcode,
                                             dec_target, dec_target_id,
                                             mca_pkg::RESP_OK, dec_reg, 32'd0);
              resp_len   <= 8'd16;
              resp_valid <= 1'b1;
            end

            CMD_CLEAR: begin
              clear_counts_toggle <= ~clear_counts_toggle;
              resp_payload <= build_response(frame_transaction, frame_opcode,
                                             dec_target, dec_target_id,
                                             mca_pkg::RESP_OK, dec_reg, 32'd0);
              resp_len   <= 8'd16;
              resp_valid <= 1'b1;
            end

            CMD_SET_RANGE: begin
              if ((dec_value[1:0] == 2'b00) || (dec_value[1:0] == 2'b11)) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_BAD_VALUE, dec_reg, 32'd0);
              end else if (range_busy || range_pending) begin
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_BUSY, dec_reg, 32'd0);
              end else if (dec_value[1:0] == range_applied_value) begin
                // Repeating the already-applied request is idempotent and
                // must not retrigger the 20 ms pulse.
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_OK, dec_reg,
                                               {30'd0, range_applied_value});
              end else begin
                pending_range <= dec_value[1:0];
                range_pending <= 1'b1;
                range_seen_busy <= 1'b0;
                resp_payload <= build_response(frame_transaction, frame_opcode,
                                               dec_target, dec_target_id,
                                               mca_pkg::RESP_OK, dec_reg, 32'd0);
              end
              resp_len   <= 8'd16;
              resp_valid <= 1'b1;
            end

            CMD_STATUS_LEGACY: begin
              resp_payload <= '0;
              resp_payload[7:0]    <= 8'h52;
              resp_payload[15:8]   <= 8'h02;
              resp_payload[23:16]  <= mca_pkg::RESP_OK;
              resp_payload[31:24]  <= frame_transaction[7:0];
              resp_payload[39:32]  <= 8'd21;
              resp_payload[47:40]  <= 8'd1;
              resp_payload[55:48]  <= 8'd1;
              resp_payload[63:56]  <= 8'd0;
              resp_payload[71:64]  <= 8'd0;
              resp_payload[79:72]  <= applied_baseline[13:8];
              resp_payload[87:80]  <= applied_baseline[7:0];
              resp_payload[95:88]  <= event_count[31:24];
              resp_payload[103:96] <= event_count[23:16];
              resp_payload[111:104]<= event_count[15:8];
              resp_payload[119:112]<= event_count[7:0];
              resp_payload[127:120]<= dropped_count[31:24];
              resp_payload[135:128]<= dropped_count[23:16];
              resp_payload[143:136]<= dropped_count[15:8];
              resp_payload[151:144]<= dropped_count[7:0];
              resp_payload[159:152]<= {5'b0, q_busy, init_done, phy_ready};
              resp_payload[167:160]<= timestamp[47:40];
              resp_payload[175:168]<= timestamp[39:32];
              resp_payload[183:176]<= timestamp[31:24];
              resp_payload[191:184]<= timestamp[23:16];
              resp_payload[199:192]<= timestamp[15:8];
              resp_payload[207:200]<= timestamp[7:0];
              resp_len   <= 8'd26;
              resp_valid <= 1'b1;
            end

            default: begin
              resp_payload <= build_response(frame_transaction, frame_opcode,
                                             dec_target, dec_target_id,
                                             mca_pkg::RESP_BAD_OPCODE, dec_reg, 32'd0);
              resp_len     <= 8'd16;
              resp_valid   <= 1'b1;
            end
          endcase
        end
      end
    end
  end

  // A command is accepted whenever no response is pending and no commit is
  // waiting for the sampling domain.
  assign cmd_ready = !cmd_pending && !dec_pending && !resp_valid && !commit_wait;
  assign busy      = commit_wait;
endmodule
