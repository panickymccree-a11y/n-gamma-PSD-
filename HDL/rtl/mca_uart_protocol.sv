`timescale 1ns/1ps

// MCS2 serial envelope and spectrum sender.  The payload of CONTROL_REQUEST
// and CONTROL_RESPONSE is the existing MCF2 wire representation.
module mca_uart_protocol #(
  parameter int CLOCK_HZ = 125_000_000,
  parameter int BAUD_HZ = 500_000,
  parameter int BIN_COUNT = 4096,
  parameter int CHUNK_BINS = 256,
  parameter logic [31:0] FIRMWARE_VERSION = 32'h0002_0600
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        uart_rxd,
  output logic        uart_txd,

  output logic        cmd_valid,
  output logic [511:0] cmd_payload,
  output logic [7:0]  cmd_len,
  input  logic        cmd_ready,
  input  logic        resp_valid,
  input  logic [519:0] resp_payload,
  input  logic [7:0]  resp_len,
  output logic        resp_ready,

  input  logic        chunk_toggle,
  output logic        chunk_ack_toggle,
  input  logic [31:0] chunk_session,
  input  logic [31:0] chunk_epoch,
  input  logic [31:0] chunk_config_version,
  input  logic        chunk_kind,
  input  logic [7:0]  chunk_flags,
  input  logic [7:0]  chunk_shift,
  input  logic [15:0] chunk_total_bins,
  input  logic [15:0] chunk_start,
  input  logic [15:0] chunk_count,
  input  logic [15:0] chunk_index,
  input  logic [15:0] chunk_total,
  input  logic [47:0] chunk_ticks,
  input  logic [31:0] chunk_events,
  input  logic [31:0] chunk_dropped,
  input  logic        status_running,
  input  logic [1:0]  status_baseline_state,
  input  logic        status_baseline_ready,
  input  logic        status_baseline_auto,
  input  logic [13:0] status_baseline,
  input  logic [13:0] status_manual_baseline,
  input  logic [13:0] status_recovery_tolerance,
  input  logic        status_frontend_init,
  input  logic        status_range_busy,
  input  logic        status_filter_enabled,
  input  logic        status_peak_interpolation,
  input  logic [1:0]  status_actual_range,
  input  logic [2:0]  status_mode,
  input  logic        status_external_armed,
  input  logic        status_external_window,
  input  logic [31:0] status_external_count,
  input  logic [47:0] status_timestamp,
  input  logic [31:0] status_config_version,
  input  logic [31:0] status_event_count,
  input  logic [31:0] status_rejected_count,
  input  logic [31:0] status_dropped_count,
  output logic [7:0]  chunk_rd_index,
  input  logic [31:0] chunk_rd_data,

  output logic [31:0] crc_error_count,
  output logic [31:0] frame_error_count
);
  localparam logic [31:0] MAGIC = 32'h4d43_5332; // "MCS2"
  localparam logic [7:0] VERSION = 8'd2;
  localparam logic [7:0] TYPE_HELLO_REQ = 8'h01;
  localparam logic [7:0] TYPE_CONTROL_REQ = 8'h02;
  localparam logic [7:0] TYPE_ACK = 8'h03;
  localparam logic [7:0] TYPE_HELLO_RESP = 8'h81;
  localparam logic [7:0] TYPE_CONTROL_RESP = 8'h82;
  localparam logic [7:0] TYPE_STATUS = 8'h83;
  localparam logic [7:0] TYPE_SPECTRUM = 8'h84;
  localparam int MAX_PAYLOAD = 1064;
  localparam int SPECTRUM_HEADER = 40;
  localparam int HELLO_BYTES = 20;
  localparam int STATUS_BYTES = 52;
  localparam int STATUS_CYCLES = CLOCK_HZ / 10;
  localparam int RETRY_CYCLES = CLOCK_HZ / 10;
  localparam int ADDR_W = $clog2(BIN_COUNT);
  localparam int PEAK_SHIFT = 14 - ADDR_W;
  // Integral histogram bins use an 8-bit shift (256 LSB per bin), matching
  // mca_spectrum_histogram's production 4096-bin address mapping.
  localparam int INTEGRAL_SHIFT = 8;

  logic [7:0] phy_rx_data, phy_tx_data;
  logic phy_rx_valid, phy_rx_error, phy_tx_valid, phy_tx_ready;
  mca_uart_phy #(.CLOCK_HZ(CLOCK_HZ), .BAUD_HZ(BAUD_HZ)) u_phy (
    .clk(clk), .rst_n(rst_n), .rxd(uart_rxd), .txd(uart_txd),
    .rx_data(phy_rx_data), .rx_valid(phy_rx_valid),
    .rx_frame_error(phy_rx_error), .tx_data(phy_tx_data),
    .tx_valid(phy_tx_valid), .tx_ready(phy_tx_ready)
  );

  function automatic logic [31:0] crc32_byte(input logic [31:0] crc,
                                               input logic [7:0] data);
    logic [31:0] c;
    integer i;
    begin
      c = crc ^ data;
      for (i = 0; i < 8; i = i + 1)
        c = c[0] ? ((c >> 1) ^ 32'hedb8_8320) : (c >> 1);
      crc32_byte = c;
    end
  endfunction

  // ---------------- receive framing ----------------
  typedef enum logic [2:0] {RX_SYNC, RX_HEADER, RX_PAYLOAD, RX_CRC} rx_state_t;
  rx_state_t rx_state;
  logic [1:0] sync_match;
  logic [4:0] rx_header_index;
  logic [15:0] rx_payload_len, rx_payload_index;
  logic [1:0] rx_crc_index;
  logic [31:0] rx_crc, rx_crc_wire;
  logic [7:0] rx_type;
  logic [31:0] rx_sequence;
  logic rx_version_ok;
  logic rx_header_ok;
  logic hello_request_pulse, ack_match_pulse;
  logic hello_pending, response_pending;
  logic awaiting_ack;
  logic [31:0] sent_epoch;
  logic sent_kind;
  logic [15:0] sent_chunk_index;
  logic [7:0] rx_payload_mem [0:MAX_PAYLOAD-1];
  integer j;

  task automatic accept_frame;
    begin
      if (rx_type == TYPE_HELLO_REQ && rx_payload_len == 0) begin
        hello_request_pulse <= 1'b1;
      end else if (rx_type == TYPE_CONTROL_REQ && rx_payload_len == 20 &&
                   !cmd_valid) begin
        cmd_payload <= '0;
        for (j = 0; j < 20; j = j + 1)
          cmd_payload[j*8 +: 8] <= rx_payload_mem[j];
        cmd_len   <= 8'd20;
        cmd_valid <= 1'b1;
      end else if (rx_type == TYPE_ACK && rx_payload_len == 12) begin
        if ({rx_payload_mem[4],rx_payload_mem[5],rx_payload_mem[6],rx_payload_mem[7]}
              == sent_epoch && rx_payload_mem[8][0] == sent_kind &&
            {rx_payload_mem[9],rx_payload_mem[10]} == sent_chunk_index) begin
          ack_match_pulse <= 1'b1;
        end
      end else begin
        frame_error_count <= frame_error_count + 1'b1;
      end
    end
  endtask

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rx_state          <= RX_SYNC;
      sync_match        <= '0;
      rx_header_index   <= '0;
      rx_payload_len    <= '0;
      rx_payload_index  <= '0;
      rx_crc_index      <= '0;
      rx_crc            <= 32'hffff_ffff;
      rx_crc_wire       <= '0;
      rx_type           <= '0;
      rx_sequence       <= '0;
      rx_version_ok     <= 1'b0;
      rx_header_ok      <= 1'b0;
      hello_request_pulse <= 1'b0;
      ack_match_pulse   <= 1'b0;
      cmd_valid         <= 1'b0;
      cmd_payload       <= '0;
      cmd_len           <= '0;
      crc_error_count   <= '0;
      frame_error_count <= '0;
    end else begin
      hello_request_pulse <= 1'b0;
      ack_match_pulse     <= 1'b0;
      if (cmd_valid && cmd_ready)
        cmd_valid <= 1'b0;
      if (phy_rx_error)
        frame_error_count <= frame_error_count + 1'b1;
      if (phy_rx_valid) begin
        case (rx_state)
          RX_SYNC: begin
            case (sync_match)
              0: sync_match <= (phy_rx_data == 8'h4d) ? 1 : 0;
              1: sync_match <= (phy_rx_data == 8'h43) ? 2 :
                               ((phy_rx_data == 8'h4d) ? 1 : 0);
              2: sync_match <= (phy_rx_data == 8'h53) ? 3 :
                               ((phy_rx_data == 8'h4d) ? 1 : 0);
              default: begin
                if (phy_rx_data == 8'h32) begin
                  rx_state        <= RX_HEADER;
                  rx_header_index <= 5'd4;
                  rx_crc          <= crc32_byte(
                      crc32_byte(crc32_byte(crc32_byte(32'hffff_ffff,8'h4d),8'h43),8'h53),8'h32);
                  rx_header_ok    <= 1'b1;
                end
                sync_match <= 0;
              end
            endcase
          end
          RX_HEADER: begin
            rx_crc <= crc32_byte(rx_crc, phy_rx_data);
            case (rx_header_index)
              4: rx_version_ok <= (phy_rx_data == VERSION);
              5: rx_type <= phy_rx_data;
              7: if (phy_rx_data != 8'd16) rx_header_ok <= 1'b0;
              8: rx_sequence[31:24] <= phy_rx_data;
              9: rx_sequence[23:16] <= phy_rx_data;
              10: rx_sequence[15:8] <= phy_rx_data;
              11: rx_sequence[7:0] <= phy_rx_data;
              12: rx_payload_len[15:8] <= phy_rx_data;
              13: rx_payload_len[7:0] <= phy_rx_data;
              14: if (phy_rx_data != 0) rx_header_ok <= 1'b0;
              default: ;
            endcase
            if (rx_header_index == 15) begin
              if ((rx_payload_len > MAX_PAYLOAD) || !rx_version_ok ||
                  !rx_header_ok || phy_rx_data != 0) begin
                rx_state <= RX_SYNC;
                frame_error_count <= frame_error_count + 1'b1;
              end else if (rx_payload_len == 0) begin
                rx_crc_index <= 0;
                rx_crc_wire  <= 0;
                rx_state     <= RX_CRC;
              end else begin
                rx_payload_index <= 0;
                rx_state <= RX_PAYLOAD;
              end
            end else begin
              rx_header_index <= rx_header_index + 1'b1;
            end
          end
          RX_PAYLOAD: begin
            rx_payload_mem[rx_payload_index] <= phy_rx_data;
            rx_crc <= crc32_byte(rx_crc, phy_rx_data);
            if (rx_payload_index + 1'b1 == rx_payload_len) begin
              rx_crc_index <= 0;
              rx_crc_wire  <= 0;
              rx_state     <= RX_CRC;
            end else begin
              rx_payload_index <= rx_payload_index + 1'b1;
            end
          end
          RX_CRC: begin
            rx_crc_wire <= {rx_crc_wire[23:0], phy_rx_data};
            if (rx_crc_index == 3) begin
              if ({rx_crc_wire[23:0], phy_rx_data} == ~rx_crc)
                accept_frame();
              else
                crc_error_count <= crc_error_count + 1'b1;
              rx_state   <= RX_SYNC;
              sync_match <= 0;
            end else begin
              rx_crc_index <= rx_crc_index + 1'b1;
            end
          end
          default: rx_state <= RX_SYNC;
        endcase
      end
    end
  end

  // ---------------- transmit framing ----------------
  logic [519:0] response_q;
  logic [7:0] response_len_q;
  logic chunk_ff1, chunk_ff2, chunk_seen;
  logic [31:0] retry_timer;
  logic [31:0] tx_sequence;
  logic [31:0] meta_session, meta_epoch, meta_config_version;
  logic meta_kind;
  logic [7:0] meta_flags, meta_shift;
  logic [15:0] meta_bins, meta_start, meta_count, meta_index, meta_total;
  logic [47:0] meta_ticks;
  logic [31:0] meta_events, meta_dropped;
  logic [31:0] status_timer;
  logic status_pending;
  logic [31:0] retry_count;

  typedef enum logic [2:0] {TX_IDLE, TX_DATA, TX_CRC0, TX_CRC1, TX_CRC2, TX_CRC3} tx_state_t;
  tx_state_t tx_state;
  logic [7:0] tx_type;
  logic [15:0] tx_payload_len;
  logic [15:0] tx_index;
  logic [31:0] tx_crc, tx_crc_final;
  logic tx_is_chunk;

  assign resp_ready = !response_pending;

  function automatic logic [7:0] be32_byte(input logic [31:0] v,
                                            input logic [1:0] index);
    case (index)
      0: be32_byte=v[31:24]; 1: be32_byte=v[23:16];
      2: be32_byte=v[15:8]; default: be32_byte=v[7:0];
    endcase
  endfunction

  function automatic logic [7:0] payload_byte(input logic [15:0] pindex);
    logic [15:0] data_offset;
    begin
      payload_byte = 8'd0;
      if (tx_type == TYPE_HELLO_RESP) begin
        case (pindex)
          0: payload_byte=BAUD_HZ[31:24]; 1: payload_byte=BAUD_HZ[23:16];
          2: payload_byte=BAUD_HZ[15:8];  3: payload_byte=BAUD_HZ[7:0];
          4: payload_byte=BIN_COUNT[15:8]; 5: payload_byte=BIN_COUNT[7:0];
          6: payload_byte=PEAK_SHIFT; 7: payload_byte=INTEGRAL_SHIFT;
          8: payload_byte=CHUNK_BINS[15:8]; 9: payload_byte=CHUNK_BINS[7:0];
          10: payload_byte=CLOCK_HZ[31:24]; 11: payload_byte=CLOCK_HZ[23:16];
          12: payload_byte=CLOCK_HZ[15:8]; 13: payload_byte=CLOCK_HZ[7:0];
          // automatic, external-trigger, MCF2 control, status and dual spectra
          14: payload_byte=8'h00; 15: payload_byte=8'h3f;
          16: payload_byte=FIRMWARE_VERSION[31:24];
          17: payload_byte=FIRMWARE_VERSION[23:16];
          18: payload_byte=FIRMWARE_VERSION[15:8];
          19: payload_byte=FIRMWARE_VERSION[7:0];
          default: payload_byte=0;
        endcase
      end else if (tx_type == TYPE_CONTROL_RESP) begin
        payload_byte = response_q[pindex*8 +: 8];
      end else if (tx_type == TYPE_STATUS) begin
        case (pindex)
          0: payload_byte=8'd2;
          1: payload_byte={status_external_window, status_external_armed,
                           status_filter_enabled, status_range_busy,
                           status_frontend_init, status_baseline_auto,
                           status_baseline_ready, status_running};
          2: payload_byte={6'd0,status_baseline_state};
          3: payload_byte={5'd0,status_mode};
          4,5,6,7: payload_byte=be32_byte(status_config_version,pindex[1:0]);
          8,9,10,11: payload_byte=be32_byte(status_event_count,pindex[1:0]);
          12,13,14,15: payload_byte=be32_byte(status_rejected_count,pindex[1:0]);
          16,17,18,19: payload_byte=be32_byte(status_dropped_count,pindex[1:0]);
          20,21,22,23: payload_byte=be32_byte(crc_error_count,pindex[1:0]);
          24,25,26,27: payload_byte=be32_byte(frame_error_count,pindex[1:0]);
          28,29,30,31: payload_byte=be32_byte(retry_count,pindex[1:0]);
          32: payload_byte=status_baseline[13:8];
          33: payload_byte=status_baseline[7:0];
          34: payload_byte=status_manual_baseline[13:8];
          35: payload_byte=status_manual_baseline[7:0];
          36: payload_byte=status_recovery_tolerance[13:8];
          37: payload_byte=status_recovery_tolerance[7:0];
          38: payload_byte={6'd0,status_actual_range};
          39: payload_byte={7'd0,status_peak_interpolation};
          40,41,42,43: payload_byte=be32_byte(status_external_count,pindex[1:0]);
          44: payload_byte=status_timestamp[47:40];
          45: payload_byte=status_timestamp[39:32];
          46: payload_byte=status_timestamp[31:24];
          47: payload_byte=status_timestamp[23:16];
          48: payload_byte=status_timestamp[15:8];
          49: payload_byte=status_timestamp[7:0];
          default: payload_byte=0;
        endcase
      end else if (tx_type == TYPE_SPECTRUM) begin
        case (pindex)
          0,1,2,3: payload_byte=be32_byte(meta_session,pindex[1:0]);
          4,5,6,7: payload_byte=be32_byte(meta_config_version,pindex[1:0]);
          8,9,10,11: payload_byte=be32_byte(meta_epoch,pindex[1:0]);
          12: payload_byte={7'd0,meta_kind}; 13: payload_byte=meta_flags;
          14: payload_byte=meta_shift; 15: payload_byte=0;
          16: payload_byte=meta_bins[15:8]; 17: payload_byte=meta_bins[7:0];
          18: payload_byte=meta_start[15:8]; 19: payload_byte=meta_start[7:0];
          20: payload_byte=meta_count[15:8]; 21: payload_byte=meta_count[7:0];
          22: payload_byte=meta_index[15:8]; 23: payload_byte=meta_index[7:0];
          24: payload_byte=meta_total[15:8]; 25: payload_byte=meta_total[7:0];
          26: payload_byte=meta_ticks[47:40]; 27: payload_byte=meta_ticks[39:32];
          28: payload_byte=meta_ticks[31:24]; 29: payload_byte=meta_ticks[23:16];
          30: payload_byte=meta_ticks[15:8]; 31: payload_byte=meta_ticks[7:0];
          32,33,34,35: payload_byte=be32_byte(meta_events,pindex[1:0]);
          36,37,38,39: payload_byte=be32_byte(meta_dropped,pindex[1:0]);
          default: begin
            data_offset = pindex - SPECTRUM_HEADER;
            case (data_offset[1:0])
              0: payload_byte=chunk_rd_data[31:24];
              1: payload_byte=chunk_rd_data[23:16];
              2: payload_byte=chunk_rd_data[15:8];
              default: payload_byte=chunk_rd_data[7:0];
            endcase
          end
        endcase
      end
    end
  endfunction

  function automatic logic [7:0] frame_byte(input logic [15:0] index);
    begin
      case (index)
        0: frame_byte=8'h4d; 1: frame_byte=8'h43;
        2: frame_byte=8'h53; 3: frame_byte=8'h32;
        4: frame_byte=VERSION; 5: frame_byte=tx_type;
        6: frame_byte=0; 7: frame_byte=16;
        8,9,10,11: frame_byte=be32_byte(tx_sequence,index[1:0]);
        12: frame_byte=tx_payload_len[15:8];
        13: frame_byte=tx_payload_len[7:0];
        14,15: frame_byte=0;
        default: frame_byte=payload_byte(index-16);
      endcase
    end
  endfunction

  always_comb begin
    chunk_rd_index = (tx_index >= 16+SPECTRUM_HEADER)
        ? (tx_index - 16 - SPECTRUM_HEADER) >> 2 : 0;
  end

  // ---------------------------------------------------------------------------
  // Transmit byte pipeline.
  //
  // frame_byte() reaches through the framing multiplexers into the histogram
  // chunk buffer and its result then feeds the CRC-32 XOR tree.  Building both
  // in the cycle that presents a byte to the PHY put 15 logic levels and most of
  // the device routing span on a single path, which did not close at 125 MHz.
  //
  // The byte is therefore registered: the chunk buffer is read through a block
  // RAM port on this same clock, so one register in front of the CRC splits the
  // work into two short paths (index -> buffer address, and buffer output ->
  // framing -> CRC).
  //
  // tx_index only moves when the PHY accepts a byte, and the PHY shifts one byte
  // every 10*CLOCK_HZ/BAUD_HZ cycles, so the register always has far more time
  // than it needs.  tx_byte_ready only holds the handshake off for the two
  // cycles after tx_index moves, which is when the freshly addressed RAM word
  // and the byte register are still catching up; that is always well inside the
  // PHY's own busy window, so the transmitted byte sequence is unchanged.
  // ---------------------------------------------------------------------------
  logic [7:0]  tx_byte;
  logic [15:0] tx_index_q1, tx_index_q2;
  logic        tx_byte_ready;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      tx_byte     <= 8'd0;
      tx_index_q1 <= 16'd0;
      tx_index_q2 <= 16'd0;
    end else begin
      tx_byte     <= frame_byte(tx_index);
      tx_index_q1 <= tx_index;
      tx_index_q2 <= tx_index_q1;
    end
  end

  // chunk_rd_data is only word(tx_index) once the address has held still for a
  // cycle, so the registered byte is meaningful once tx_index has held still for
  // two.
  assign tx_byte_ready = (tx_index == tx_index_q1) && (tx_index_q1 == tx_index_q2);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      hello_pending     <= 1'b0;
      response_pending  <= 1'b0;
      response_q        <= '0;
      response_len_q    <= '0;
      chunk_ff1         <= 1'b0;
      chunk_ff2         <= 1'b0;
      chunk_seen        <= 1'b0;
      chunk_ack_toggle  <= 1'b0;
      awaiting_ack      <= 1'b0;
      retry_timer       <= 0;
      tx_sequence       <= 0;
      tx_state          <= TX_IDLE;
      tx_type           <= 0;
      tx_payload_len    <= 0;
      tx_index          <= 0;
      tx_crc            <= 32'hffff_ffff;
      tx_crc_final      <= 0;
      tx_is_chunk       <= 1'b0;
      status_timer      <= 0;
      status_pending    <= 1'b0;
      retry_count       <= 0;
      phy_tx_valid      <= 1'b0;
      phy_tx_data       <= 0;
    end else begin
      chunk_ff1 <= chunk_toggle;
      chunk_ff2 <= chunk_ff1;
      phy_tx_valid <= 1'b0;
      if (status_timer == STATUS_CYCLES-1) begin
        status_timer   <= 0;
        status_pending <= 1'b1;
      end else begin
        status_timer <= status_timer + 1'b1;
      end
      if (resp_valid && resp_ready) begin
        response_pending <= 1'b1;
        response_q       <= resp_payload;
        response_len_q   <= resp_len;
      end
      if (hello_request_pulse)
        hello_pending <= 1'b1;
      if (ack_match_pulse) begin
        awaiting_ack     <= 1'b0;
        chunk_ack_toggle <= ~chunk_ack_toggle;
      end
      if (awaiting_ack) begin
        if (retry_timer == RETRY_CYCLES-1)
          retry_timer <= 0;
        else
          retry_timer <= retry_timer + 1'b1;
      end else begin
        retry_timer <= 0;
      end

      case (tx_state)
        TX_IDLE: begin
          tx_index <= 0;
          tx_crc   <= 32'hffff_ffff;
          if (response_pending) begin
            tx_type          <= TYPE_CONTROL_RESP;
            tx_payload_len   <= response_len_q;
            response_pending <= 1'b0;
            tx_is_chunk      <= 1'b0;
            tx_state         <= TX_DATA;
          end else if (hello_pending) begin
            tx_type        <= TYPE_HELLO_RESP;
            tx_payload_len <= HELLO_BYTES;
            hello_pending  <= 1'b0;
            tx_is_chunk    <= 1'b0;
            tx_state       <= TX_DATA;
          end else if (status_pending) begin
            tx_type        <= TYPE_STATUS;
            tx_payload_len <= STATUS_BYTES;
            status_pending <= 1'b0;
            tx_is_chunk    <= 1'b0;
            tx_state       <= TX_DATA;
          end else if ((chunk_ff2 != chunk_seen) ||
                       (awaiting_ack && retry_timer == RETRY_CYCLES-1)) begin
            if (awaiting_ack && retry_timer == RETRY_CYCLES-1)
              retry_count <= retry_count + 1'b1;
            if (chunk_ff2 != chunk_seen) begin
              chunk_seen      <= chunk_ff2;
              meta_session    <= chunk_session;
              meta_epoch      <= chunk_epoch;
              meta_config_version <= chunk_config_version;
              meta_kind       <= chunk_kind;
              meta_flags      <= chunk_flags;
              meta_shift      <= chunk_shift;
              meta_bins       <= chunk_total_bins;
              meta_start      <= chunk_start;
              meta_count      <= chunk_count;
              meta_index      <= chunk_index;
              meta_total      <= chunk_total;
              meta_ticks      <= chunk_ticks;
              meta_events     <= chunk_events;
              meta_dropped    <= chunk_dropped;
              sent_epoch      <= chunk_epoch;
              sent_kind       <= chunk_kind;
              sent_chunk_index<= chunk_index;
            end
            tx_type        <= TYPE_SPECTRUM;
            tx_payload_len <= SPECTRUM_HEADER + CHUNK_BINS*4;
            tx_is_chunk    <= 1'b1;
            tx_state       <= TX_DATA;
          end
        end
        TX_DATA: if (phy_tx_ready && !phy_tx_valid && tx_byte_ready) begin
          phy_tx_data  <= tx_byte;
          phy_tx_valid <= 1'b1;
          tx_crc       <= crc32_byte(tx_crc, tx_byte);
          if (tx_index + 1'b1 == 16 + tx_payload_len) begin
            tx_crc_final <= ~crc32_byte(tx_crc, tx_byte);
            tx_state <= TX_CRC0;
          end else begin
            tx_index <= tx_index + 1'b1;
          end
        end
        TX_CRC0: if (phy_tx_ready && !phy_tx_valid) begin
          phy_tx_data<=tx_crc_final[31:24]; phy_tx_valid<=1'b1; tx_state<=TX_CRC1;
        end
        TX_CRC1: if (phy_tx_ready && !phy_tx_valid) begin
          phy_tx_data<=tx_crc_final[23:16]; phy_tx_valid<=1'b1; tx_state<=TX_CRC2;
        end
        TX_CRC2: if (phy_tx_ready && !phy_tx_valid) begin
          phy_tx_data<=tx_crc_final[15:8]; phy_tx_valid<=1'b1; tx_state<=TX_CRC3;
        end
        TX_CRC3: if (phy_tx_ready && !phy_tx_valid) begin
          phy_tx_data<=tx_crc_final[7:0]; phy_tx_valid<=1'b1;
          if (tx_is_chunk) begin awaiting_ack<=1'b1; retry_timer<=0; end
          tx_sequence<=tx_sequence+1'b1; tx_state<=TX_IDLE;
        end
        default: tx_state <= TX_IDLE;
      endcase
    end
  end
endmodule
