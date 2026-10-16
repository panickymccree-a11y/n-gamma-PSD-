`timescale 1ns/1ps

// MCA2 status payload builder.
//
// The payload is a byte stream of status TLVs followed by the end marker.
// Layout per TLV: [type][length][value...], matching the 32-channel host
// decoder.  Types 1..8 keep their original meaning so the existing host keeps
// working; types 9..12 carry the single-channel extensions.
//
//    1 capability       8 bytes  protocol, adc, channel, module, pair, mode,
//                                running, alignment mask
//    2 epoch           20 bytes  config version, timestamp, ready mask,
//                                tracking mask, fifo count
//    3 counters        28 bytes  event/module/pair/ambiguous/error counters
//    4 flow            20 bytes  pair/fifo/backpressure counters
//    5 baseline 0-7    12 bytes  12-bit packing for channels 0..7
//    9 baseline state   6 bytes  2 bits per channel for 24 channels
//   10 manual baseline  3 bytes  auto flag + 14-bit manual baseline
//   11 extra counters   9 bytes  dead-time rejects, fifo drops, event active
//   12 baseline16       3 bytes  one full 14-bit baseline value
//   13 frontend         4 bytes  range, flags, recovery tolerance
//    0 end                       terminates the payload (1 byte)
//
// 134 bytes in total.  The snapshot is built from the same baseline register
// the acquisition path uses, so the report and the calculation never disagree.
module mca_status_snapshot #(
  parameter int INTERVAL_CYCLES = 12_500_000,
  parameter int PAYLOAD_BYTES   = 134
) (
  input  logic          dco_clk,
  input  logic          rst_n,
  input  logic          force_refresh,
  input  logic [31:0]   config_version,
  input  logic [47:0]   timestamp,
  input  logic [31:0]   event_count,
  input  logic [31:0]   rejected_count,
  input  logic [31:0]   dropped_count,
  input  logic [13:0]   baseline,
  input  logic [1:0]    baseline_state,
  input  logic          baseline_ready,
  input  logic          baseline_auto,
  input  logic [13:0]   manual_baseline,
  input  logic          run_enable,
  input  logic [2:0]    mode,
  input  logic          event_active,
  input  logic [7:0]    event_fifo_level,
  input  logic          adc_aligned,
  input  logic [1:0]    range_actual,
  input  logic          range_busy,
  input  logic          filter_enabled,
  input  logic          peak_interpolation_enabled,
  input  logic          frontend_init_done,
  input  logic [13:0]   recovery_tolerance,
  output logic          wr_en,
  output logic [PAYLOAD_BYTES*8-1:0] payload,
  input  logic          fifo_full
);
  localparam int CW = (INTERVAL_CYCLES < 2) ? 1 : $clog2(INTERVAL_CYCLES + 1);
  localparam int PAYLOAD_BITS = PAYLOAD_BYTES * 8;
  localparam int SCRATCH_BYTES = (PAYLOAD_BYTES < 134) ? 134 : PAYLOAD_BYTES;

  // Byte layout offsets.  0 means the byte is not used.
  localparam int OFF_EPOCH      = 10;   // after TLV1 header + 8 body
  localparam int OFF_COUNTERS   = 32;   // after TLV2 header + 20 body
  localparam int OFF_FLOW       = 62;   // after TLV3 header + 28 body
  localparam int OFF_BASE5      = 84;   // after TLV4 header + 20 body
  localparam int OFF_STATE      = 98;   // after TLV5 header + 12 body
  localparam int OFF_MANUAL     = 106;  // after TLV9 header + 6 body
  localparam int OFF_EXTRA      = 111;  // after TLV10 header + 3 body
  localparam int OFF_BASE16     = 122;  // after TLV11 header + 9 body
  // TLV12 header + body ends at 126. TLV13 replaces the former end marker at
  // 127 and ends at 132; the final end marker is byte 133.
  localparam int OFF_FRONTEND  = 127;

  logic [CW-1:0] count;
  logic [31:0]   refresh_pending;
  logic [31:0]   ready_mask;
  logic [31:0]   tracking_mask;

  // Byte-indexed combinational assembly avoids a thousand handwritten bit
  // slices and makes the TLV offsets auditable.  The scratch word is wide
  // enough for the byte indices used below; only PAYLOAD_BITS are exported.
  logic [SCRATCH_BYTES*8-1:0] p;
  task automatic set_byte(input int index, input logic [7:0] value);
    begin
      p[index*8 +: 8] = value;
    end
  endtask

  always_comb begin
    p = '0;
    ready_mask    = {31'd0, baseline_ready};
    tracking_mask = {31'd0, baseline_auto};

    // ---------------- TLV 1: capability, 8 bytes ----------------
    set_byte(0, 8'd1);
    set_byte(1, 8'd8);
    set_byte(2, 8'd2);                   // protocol version
    set_byte(3, 8'd1);                   // adc count
    set_byte(4, 8'd1);                   // channel count
    set_byte(5, 8'd0);                   // module count
    set_byte(6, 8'd0);                   // pair count
    // Protocol mode is 0 for spectrum and 3 for waveform.  This field used
    // to mirror run_enable, so starting acquisition reported invalid mode 1
    // and made the host repeatedly rebuild/toggle its mode controls.
    set_byte(7, mode);                    // acquisition mode
    set_byte(8, {7'd0, run_enable});     // running
    set_byte(9, {7'd0, adc_aligned});    // alignment mask (bit0 = ADC0)

    // ---------------- TLV 2: epoch, 20 bytes ----------------
    set_byte(OFF_EPOCH + 0, 8'd2);
    set_byte(OFF_EPOCH + 1, 8'd20);
    set_byte(OFF_EPOCH + 2,  config_version[31:24]);
    set_byte(OFF_EPOCH + 3,  config_version[23:16]);
    set_byte(OFF_EPOCH + 4,  config_version[15:8]);
    set_byte(OFF_EPOCH + 5,  config_version[7:0]);
    set_byte(OFF_EPOCH + 6,  timestamp[47:40]);
    set_byte(OFF_EPOCH + 7,  timestamp[39:32]);
    set_byte(OFF_EPOCH + 8,  timestamp[31:24]);
    set_byte(OFF_EPOCH + 9,  timestamp[23:16]);
    set_byte(OFF_EPOCH + 10, timestamp[15:8]);
    set_byte(OFF_EPOCH + 11, timestamp[7:0]);
    set_byte(OFF_EPOCH + 12, ready_mask[31:24]);
    set_byte(OFF_EPOCH + 13, ready_mask[23:16]);
    set_byte(OFF_EPOCH + 14, ready_mask[15:8]);
    set_byte(OFF_EPOCH + 15, ready_mask[7:0]);
    set_byte(OFF_EPOCH + 16, tracking_mask[31:24]);
    set_byte(OFF_EPOCH + 17, tracking_mask[23:16]);
    set_byte(OFF_EPOCH + 18, tracking_mask[15:8]);
    set_byte(OFF_EPOCH + 19, tracking_mask[7:0]);
    set_byte(OFF_EPOCH + 20, event_fifo_level);
    set_byte(OFF_EPOCH + 21, 8'd0);

    // ---------------- TLV 3: main counters, 28 bytes ----------------
    set_byte(OFF_COUNTERS + 0, 8'd3);
    set_byte(OFF_COUNTERS + 1, 8'd28);
    set_byte(OFF_COUNTERS + 2, event_count[31:24]);
    set_byte(OFF_COUNTERS + 3, event_count[23:16]);
    set_byte(OFF_COUNTERS + 4, event_count[15:8]);
    set_byte(OFF_COUNTERS + 5, event_count[7:0]);

    // ---------------- TLV 4: flow counters, 20 bytes ----------------
    set_byte(OFF_FLOW + 0, 8'd4);
    set_byte(OFF_FLOW + 1, 8'd20);
    set_byte(OFF_FLOW + 10, dropped_count[31:24]);
    set_byte(OFF_FLOW + 11, dropped_count[23:16]);
    set_byte(OFF_FLOW + 12, dropped_count[15:8]);
    set_byte(OFF_FLOW + 13, dropped_count[7:0]);

    // ---------------- TLV 5: baseline 0..7, 12 bytes ----------------
    // Packing: byte0 = ch0[11:4], byte1 = {ch0[3:0], ch1[11:8]},
    // byte2 = ch1[7:0].  Only channel 0 exists on this device.
    set_byte(OFF_BASE5 + 0, 8'd5);
    set_byte(OFF_BASE5 + 1, 8'd12);
    set_byte(OFF_BASE5 + 2, baseline[11:4]);
    set_byte(OFF_BASE5 + 3, {baseline[3:0], 4'd0});
    set_byte(OFF_BASE5 + 4, 8'd0);

    // ---------------- TLV 9: baseline state, 6 bytes ----------------
    set_byte(OFF_STATE + 0, 8'd9);
    set_byte(OFF_STATE + 1, 8'd6);
    set_byte(OFF_STATE + 2, {6'd0, baseline_state[1:0]});

    // ---------------- TLV 10: manual baseline, 3 bytes ----------------
    set_byte(OFF_MANUAL + 0, 8'd10);
    set_byte(OFF_MANUAL + 1, 8'd3);
    set_byte(OFF_MANUAL + 2, {7'd0, baseline_auto});
    set_byte(OFF_MANUAL + 3, manual_baseline[13:8]);
    set_byte(OFF_MANUAL + 4, manual_baseline[7:0]);

    // ---------------- TLV 11: extra counters, 9 bytes ----------------
    set_byte(OFF_EXTRA + 0, 8'd11);
    set_byte(OFF_EXTRA + 1, 8'd9);
    set_byte(OFF_EXTRA + 2, rejected_count[31:24]);
    set_byte(OFF_EXTRA + 3, rejected_count[23:16]);
    set_byte(OFF_EXTRA + 4, rejected_count[15:8]);
    set_byte(OFF_EXTRA + 5, rejected_count[7:0]);
    set_byte(OFF_EXTRA + 6, dropped_count[31:24]);
    set_byte(OFF_EXTRA + 7, dropped_count[23:16]);
    set_byte(OFF_EXTRA + 8, dropped_count[15:8]);
    set_byte(OFF_EXTRA + 9, dropped_count[7:0]);
    set_byte(OFF_EXTRA + 10, {7'd0, event_active});

    // ---------------- TLV 12: full 14-bit baseline, 3 bytes --------
    set_byte(OFF_BASE16 + 0, 8'd12);
    set_byte(OFF_BASE16 + 1, 8'd3);
    set_byte(OFF_BASE16 + 2, 8'd0); // channel id
    set_byte(OFF_BASE16 + 3, {2'd0, baseline[13:8]});
    set_byte(OFF_BASE16 + 4, baseline[7:0]);

    // ---------------- TLV 13: actual front-end state, 4 bytes --------
    set_byte(OFF_FRONTEND + 0, 8'd13);
    set_byte(OFF_FRONTEND + 1, 8'd4);
    set_byte(OFF_FRONTEND + 2, {6'd0, range_actual});
    set_byte(OFF_FRONTEND + 3, {4'd0, peak_interpolation_enabled,
                                 frontend_init_done,
                                 filter_enabled, range_busy});
    set_byte(OFF_FRONTEND + 4, recovery_tolerance[13:8]);
    set_byte(OFF_FRONTEND + 5, recovery_tolerance[7:0]);
    // End marker. The host requires offset == payload length when the end
    // TLV is reached, so nothing may follow it.
    set_byte(OFF_FRONTEND + 6, 8'd0);
  end

  assign payload = p[PAYLOAD_BITS-1:0];

  always_ff @(posedge dco_clk or negedge rst_n) begin
    if (!rst_n) begin
      count           <= '0;
      refresh_pending <= 32'd0;
      wr_en           <= 1'b0;
    end else begin
      wr_en <= 1'b0;
      if (force_refresh)
        refresh_pending <= 32'd4;
      else if (refresh_pending != 0)
        refresh_pending <= refresh_pending - 1'b1;

      if ((count == INTERVAL_CYCLES - 1) || (refresh_pending == 32'd1)) begin
        count <= '0;
        if (!fifo_full) begin
          wr_en           <= 1'b1;
          refresh_pending <= '0;
        end
      end else begin
        count <= count + 1'b1;
      end
    end
  end
endmodule
