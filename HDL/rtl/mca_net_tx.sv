`timescale 1ns/1ps

// UDP transmit path: builds MCA2/MCF2 payloads and streams them to the
// proven Ethernet/UDP stack.
//
// Arbitration is bounded so control responses cannot be starved by a busy
// event stream: a response takes priority immediately after the current
// packet, followed by status.  Events are only batched while neither is
// pending.  Waveform fragments are the lowest-priority source, so a waveform
// upload delays higher-priority traffic by at most the fragment in flight.
//
// Packet framing:
//   channel events : 20-byte MCA2 common header + N x 12-byte records
//   control reply  : MCA2 type-5 header + one 16-byte MCF2 response record
//   status         : MCA2 type-4 header + status TLV payload
//   waveform       : MCA2 type-6 header + 4-byte session id + 32-byte
//                    fragment header + up to 512 16-bit sample words
//
// The MCA2 common header is
//   0..3 magic "MCA2", 4 version, 5 type, 6 device id, 7 flags,
//   8..11 sequence, 12..15 config version, 16..17 record count,
//   18..19 payload bytes.
//
// Every packet is assembled completely before transmission starts.  The
// staging store is a 16-bit block RAM addressed by byte, so the byte-rate
// output path is one register instead of a wide combinational multiplexer
// over a thousand register bits.  Because each staged byte is emitted by a
// marked byte in the low lane and the read side only ever consumes a fully
// staged packet, a half-written word can never be transmitted.
module mca_net_tx #(
  parameter logic [47:0] LOCAL_MAC   = 48'h02_12_34_56_78_9A,
  parameter logic [47:0] HOST_MAC    = 48'h02_00_00_00_00_01,
  parameter logic [31:0] LOCAL_IP    = 32'hC0A8_010A,
  parameter logic [31:0] HOST_IP     = 32'hC0A8_0164,
  parameter int          UDP_SRC_PORT = 9000,
  parameter int          UDP_DST_PORT = 9001,
  parameter int          EVENT_TIMEOUT_CYCLES = 12_500,
  parameter int          MAX_BATCH    = 8,
  parameter int          PSD_MAX_BATCH = 40,
  parameter int          STATUS_BYTES = 128,
  // Keep the standalone transmitter's legacy default.  The single-channel
  // top explicitly advertises the new front-end status capability with C0.
  parameter logic [7:0]  STATUS_FLAGS = 8'h00,
  parameter int          STAGE_BYTES  = 1080,
  parameter int          STAGE_AW     = 10
) (
  input  logic         clk,
  input  logic         rst_n,
  input  logic [7:0]   device_id,

  // Event stream from the sampling domain
  input  logic         event_empty,
  output logic         event_rd_en,
  input  logic [231:0] event_dout,
  input  logic [8:0]   event_level,

  // MCF2 control response
  input  logic         resp_empty,
  output logic         resp_rd_en,
  input  logic [519:0] resp_dout,

  // Status snapshot
  input  logic          status_empty,
  output logic          status_rd_en,
  input  logic [STATUS_BYTES*8-1:0] status_dout,

  // Waveform fragment stream
  input  logic          wave_valid,
  output logic          wave_ack,
  input  logic          wave_cancel,
  input  logic [15:0]   wave_index,
  input  logic [15:0]   wave_sample_count,
  input  logic [15:0]   wave_sample_offset,
  input  logic [15:0]   wave_fragment_count,
  input  logic          wave_last,
  input  logic [31:0]   wave_capture_id,
  input  logic [47:0]   wave_timestamp,
  input  logic [13:0]   wave_baseline,
  input  logic [13:0]   wave_threshold,
  input  logic [13:0]   wave_return,
  input  logic          wave_negative,
  input  logic [31:0]   wave_config_version,
  input  logic [31:0]   wave_session_id,
  output logic [15:0]   wave_word_index,
  output logic          wave_word_req,
  input  logic [15:0]   wave_word,
  input  logic          wave_word_valid,

  // Control-domain packet metadata
  input  logic [31:0]  cfg_version,

  // Ethernet/UDP payload streaming
  output logic         tx_start_en,
  output logic [15:0]  tx_byte_num,
  output logic [7:0]   tx_data,
  input  logic         tx_data_req,
  input  logic         tx_done
);
  // 20-byte common header + 4-byte session id + 32-byte fragment header.
  localparam int WAVE_HEADER_BYTES = 20 + 4 + 32;
  localparam int PSD_BATCH_LIMIT = (PSD_MAX_BATCH < MAX_BATCH) ? PSD_MAX_BATCH : MAX_BATCH;
  initial begin
    if (MAX_BATCH < 1 || MAX_BATCH > 255 || PSD_MAX_BATCH < 1 || PSD_MAX_BATCH > 255)
      $fatal(1, "invalid event batch parameter");
    if ((20+12*MAX_BATCH)>STAGE_BYTES || (20+24*PSD_BATCH_LIMIT)>STAGE_BYTES ||
        (20+12*MAX_BATCH)>1472 || (20+24*PSD_BATCH_LIMIT)>1472 ||
        STAGE_BYTES>(2*(1<<STAGE_AW)) || STAGE_BYTES<WAVE_HEADER_BYTES+1024)
      $fatal(1, "packet exceeds staging or UDP capacity");
  end

  typedef enum logic [3:0] {
    TX_IDLE,
    TX_EV_RD,
    TX_EV_CAP,
    TX_EV_STAGE,
    TX_RESP_RD,
    TX_RESP_CAP,
    TX_RESP_STAGE,
    TX_ST_RD,
    TX_ST_STAGE,
    TX_FORM_HDR,
    TX_FORM_READ,
    TX_FORM_SAMPLE,
    TX_PRIME,
    TX_DATA
  } tx_state_t;

  tx_state_t state;
  logic [15:0] byte_index;
  logic [15:0] payload_len;
  logic [15:0] header_payload_len;
  localparam int BATCH_COUNT_WIDTH = (MAX_BATCH < 1) ? 1 : $clog2(MAX_BATCH + 1);
  localparam int EVENT_TIMER_WIDTH = (EVENT_TIMEOUT_CYCLES < 1) ? 1
                                   : $clog2(EVENT_TIMEOUT_CYCLES + 1);
  logic [BATCH_COUNT_WIDTH-1:0] batch_index;
  logic [BATCH_COUNT_WIDTH-1:0] batch_count;
  logic [31:0] pkt_sequence;
  logic [231:0] event_word, pending_word;
  logic pending_valid, use_pending;
  logic [7:0] batch_type;
  logic [31:0] batch_version;
  logic [4:0] record_pos, record_bytes;
  logic [BATCH_COUNT_WIDTH-1:0] batch_target;
  wire [9:0] available_events={1'b0,event_level}+pending_valid;
  wire [231:0] rx_word = use_pending ? pending_word : event_dout;
  wire [7:0] rx_type = rx_word[199:192]==8'h07 ? 8'h07 : 8'h01;
  wire [31:0] rx_version = (rx_word[199:192]==8'h07 || rx_word[199:192]==8'h01)
                             ? rx_word[231:200] : cfg_version;
  logic [EVENT_TIMER_WIDTH-1:0] event_timer;

  logic [519:0] resp_word;
  logic         tx_active;

  // -----------------------------------------------------------------
  // Staging store: 16-bit distributed RAM addressed by word.  Its asynchronous
  // read is intentional: the Ethernet byte interface cannot tolerate an
  // implementation-dependent extra BRAM output cycle.  That extra cycle was
  // observed on hardware as two duplicated 16-bit words in every packet,
  // corrupting the MCA2 payload length and making the host retry forever.
  // -----------------------------------------------------------------
  logic                stg_we;
  logic [STAGE_AW-1:0] stg_write_address;
  logic [15:0]         stg_write_data;
  logic [STAGE_AW-1:0] stg_read_address;
  logic [15:0]         stg_dout;
  logic [STAGE_AW-1:0] stg_prefetch_word;
  logic [15:0]         tx_byte_register;
  logic [2:0]          prime_count;
  logic [7:0]          stage_byte_lo;

  // Staging byte sequencer
  logic [15:0] emit_index;
  logic [15:0] form_offset;
  logic [STAGE_AW-1:0] form_write_index;
  logic [15:0] form_sample_index;
  logic        form_sample_last_q;

  // Waveform descriptor latched for the fragment in flight
  logic [31:0] form_capture_id_q;
  logic [47:0] form_timestamp_q;
  logic [15:0] form_index_q, form_offset_q;
  logic [15:0] form_fragment_count_q, form_sample_count_q;
  logic [13:0] form_baseline_q, form_threshold_q, form_return_q;
  logic        form_negative_q;
  logic [31:0] form_session_q, form_version_q;
  logic        wave_ack_q;
  logic        tx_is_waveform;

  assign header_payload_len = payload_len - 16'd20;

  wire idle = (state == TX_IDLE);

  assign event_rd_en   = (state == TX_EV_RD);
  assign resp_rd_en    = (state == TX_RESP_RD);
  assign status_rd_en  = (state == TX_ST_RD);
  assign wave_word_req = (state == TX_FORM_READ);
  assign wave_word_index = form_sample_index;

  // -----------------------------------------------------------------
  // Staging store
  // -----------------------------------------------------------------
  (* ram_style = "distributed" *)
  logic [15:0] stage_mem [0:(1<<STAGE_AW)-1];
  always_ff @(posedge clk) begin
    if (stg_we)
      stage_mem[stg_write_address] <= stg_write_data;
  end
  assign stg_dout = stage_mem[stg_read_address];

  // -----------------------------------------------------------------
  // Byte generation for the packet being staged.  Exactly one byte (or one
  // sample word in the waveform case) is written per cycle.
  // -----------------------------------------------------------------
  logic [7:0]  emit_byte;
  logic [15:0] emit_word;

  always_comb begin
    emit_byte    = 8'h00;
    emit_word    = 16'h0000;
    unique case (state)
      TX_EV_STAGE: begin
        case (emit_index)
          16'd0:  emit_byte = 8'h4D;
          16'd1:  emit_byte = 8'h43;
          16'd2:  emit_byte = 8'h41;
          16'd3:  emit_byte = 8'h32;
          16'd4:  emit_byte = 8'd2;
          16'd5:  emit_byte = batch_type;
          16'd6:  emit_byte = device_id;
          16'd7:  emit_byte = 8'd0;
          16'd8:  emit_byte = pkt_sequence[31:24];
          16'd9:  emit_byte = pkt_sequence[23:16];
          16'd10: emit_byte = pkt_sequence[15:8];
          16'd11: emit_byte = pkt_sequence[7:0];
          16'd12: emit_byte = batch_version[31:24];
          16'd13: emit_byte = batch_version[23:16];
          16'd14: emit_byte = batch_version[15:8];
          16'd15: emit_byte = batch_version[7:0];
          16'd16: emit_byte = 8'd0;
          16'd17: emit_byte = batch_count;
          16'd18: emit_byte = header_payload_len[15:8];
          16'd19: emit_byte = header_payload_len[7:0];
          default: begin
            case (record_pos)
              5'd0: emit_byte = batch_type==8'h07 ? event_word[191:184] : event_word[95:88];
              5'd1: emit_byte = batch_type==8'h07 ? event_word[183:176] : event_word[87:80];
              5'd2: emit_byte = batch_type==8'h07 ? event_word[175:168] : event_word[79:72];
              5'd3: emit_byte = batch_type==8'h07 ? event_word[167:160] : event_word[71:64];
              5'd4: emit_byte = batch_type==8'h07 ? event_word[159:152] : event_word[63:56];
              5'd5: emit_byte = batch_type==8'h07 ? event_word[151:144] : event_word[55:48];
              5'd6: emit_byte = batch_type==8'h07 ? event_word[143:136] : event_word[47:40];
              5'd7: emit_byte = batch_type==8'h07 ? event_word[135:128] : event_word[39:32];
              5'd8: emit_byte = batch_type==8'h07 ? event_word[127:120] : event_word[31:24];
              5'd9: emit_byte = batch_type==8'h07 ? event_word[119:112] : event_word[23:16];
              5'd10: emit_byte = batch_type==8'h07 ? event_word[111:104] : event_word[15:8];
              5'd11: emit_byte = batch_type==8'h07 ? event_word[103:96] : event_word[7:0];
              5'd12: emit_byte = event_word[95:88];
              5'd13: emit_byte = event_word[87:80];
              5'd14: emit_byte = event_word[79:72];
              5'd15: emit_byte = event_word[71:64];
              5'd16: emit_byte = event_word[63:56];
              5'd17: emit_byte = event_word[55:48];
              5'd18: emit_byte = event_word[47:40];
              5'd19: emit_byte = event_word[39:32];
              5'd20: emit_byte = event_word[31:24];
              5'd21: emit_byte = event_word[23:16];
              5'd22: emit_byte = event_word[15:8];
              5'd23: emit_byte = event_word[7:0];
              default: emit_byte=0;
            endcase
          end
        endcase
      end

      TX_RESP_STAGE: begin
        case (emit_index)
          16'd0:  emit_byte = 8'h4D;
          16'd1:  emit_byte = 8'h43;
          16'd2:  emit_byte = 8'h41;
          16'd3:  emit_byte = 8'h32;
          16'd4:  emit_byte = 8'd2;
          16'd5:  emit_byte = mca_pkg::PKT_CONTROL_RESP;
          16'd6:  emit_byte = device_id;
          16'd7:  emit_byte = 8'd0;
          16'd8:  emit_byte = pkt_sequence[31:24];
          16'd9:  emit_byte = pkt_sequence[23:16];
          16'd10: emit_byte = pkt_sequence[15:8];
          16'd11: emit_byte = pkt_sequence[7:0];
          16'd12: emit_byte = cfg_version[31:24];
          16'd13: emit_byte = cfg_version[23:16];
          16'd14: emit_byte = cfg_version[15:8];
          16'd15: emit_byte = cfg_version[7:0];
          16'd16: emit_byte = 8'd0;
          16'd17: emit_byte = 8'd1;
          16'd18: emit_byte = 8'd0;
          16'd19: emit_byte = 8'd16;
          // `resp_word` contains the complete FIFO item.  Its low 512 bits
          // are the MCF2 payload, which begins at MCA2 byte 20.
          default: emit_byte = resp_word[((emit_index - 16'd20) * 8) +: 8];
        endcase
      end

      TX_ST_STAGE: begin
        case (emit_index)
          16'd0:  emit_byte = 8'h4D;
          16'd1:  emit_byte = 8'h43;
          16'd2:  emit_byte = 8'h41;
          16'd3:  emit_byte = 8'h32;
          16'd4:  emit_byte = 8'd2;
          16'd5:  emit_byte = mca_pkg::PKT_STATUS;
          16'd6:  emit_byte = device_id;
          16'd7:  emit_byte = STATUS_FLAGS;
          16'd8:  emit_byte = pkt_sequence[31:24];
          16'd9:  emit_byte = pkt_sequence[23:16];
          16'd10: emit_byte = pkt_sequence[15:8];
          16'd11: emit_byte = pkt_sequence[7:0];
          16'd12: emit_byte = cfg_version[31:24];
          16'd13: emit_byte = cfg_version[23:16];
          16'd14: emit_byte = cfg_version[15:8];
          16'd15: emit_byte = cfg_version[7:0];
          16'd16: emit_byte = 8'd0;
          16'd17: emit_byte = 8'd1;
          16'd18: emit_byte = STATUS_BYTES[15:8];
          16'd19: emit_byte = STATUS_BYTES[7:0];
          // Status byte 0 starts immediately after the 20-byte MCA2 header.
          default: emit_byte = status_dout[((emit_index - 16'd20) * 8) +: 8];
        endcase
      end

      TX_FORM_HDR: begin
        case (emit_index)
          16'd0:  emit_byte = 8'h4D;
          16'd1:  emit_byte = 8'h43;
          16'd2:  emit_byte = 8'h41;
          16'd3:  emit_byte = 8'h32;
          16'd4:  emit_byte = 8'd2;
          16'd5:  emit_byte = mca_pkg::PKT_WAVEFORM;
          16'd6:  emit_byte = device_id;
          16'd7:  emit_byte = 8'd0;
          16'd8:  emit_byte = pkt_sequence[31:24];
          16'd9:  emit_byte = pkt_sequence[23:16];
          16'd10: emit_byte = pkt_sequence[15:8];
          16'd11: emit_byte = pkt_sequence[7:0];
          16'd12: emit_byte = form_version_q[31:24];
          16'd13: emit_byte = form_version_q[23:16];
          16'd14: emit_byte = form_version_q[15:8];
          16'd15: emit_byte = form_version_q[7:0];
          16'd16: emit_byte = 8'd0;
          16'd17: emit_byte = 8'd1;                     // one fragment record
          16'd18: emit_byte = header_payload_len[15:8];
          16'd19: emit_byte = header_payload_len[7:0];
          16'd20: emit_byte = form_session_q[31:24];
          16'd21: emit_byte = form_session_q[23:16];
          16'd22: emit_byte = form_session_q[15:8];
          16'd23: emit_byte = form_session_q[7:0];
          16'd24: emit_byte = form_capture_id_q[31:24];
          16'd25: emit_byte = form_capture_id_q[23:16];
          16'd26: emit_byte = form_capture_id_q[15:8];
          16'd27: emit_byte = form_capture_id_q[7:0];
          16'd28: emit_byte = form_timestamp_q[47:40];
          16'd29: emit_byte = form_timestamp_q[39:32];
          16'd30: emit_byte = form_timestamp_q[31:24];
          16'd31: emit_byte = form_timestamp_q[23:16];
          16'd32: emit_byte = form_timestamp_q[15:8];
          16'd33: emit_byte = form_timestamp_q[7:0];
          16'd34: emit_byte = {2'd0, form_baseline_q[13:8]};
          16'd35: emit_byte = form_baseline_q[7:0];
          16'd36: emit_byte = {2'd0, form_threshold_q[13:8]};
          16'd37: emit_byte = form_threshold_q[7:0];
          16'd38: emit_byte = {2'd0, form_return_q[13:8]};
          16'd39: emit_byte = form_return_q[7:0];
          16'd40: emit_byte = mca_pkg::WAVE_PRE_SAMPLES[15:8];
          16'd41: emit_byte = mca_pkg::WAVE_PRE_SAMPLES[7:0];
          16'd42: emit_byte = mca_pkg::WAVE_TOTAL_SAMPLES[15:8];
          16'd43: emit_byte = mca_pkg::WAVE_TOTAL_SAMPLES[7:0];
          16'd44: emit_byte = form_offset_q[15:8];
          16'd45: emit_byte = form_offset_q[7:0];
          16'd46: emit_byte = form_sample_count_q[15:8];
          16'd47: emit_byte = form_sample_count_q[7:0];
          16'd48: emit_byte = form_index_q[15:8];
          16'd49: emit_byte = form_index_q[7:0];
          16'd50: emit_byte = form_fragment_count_q[15:8];
          16'd51: emit_byte = form_fragment_count_q[7:0];
          16'd52: emit_byte = 8'd0;                       // reserved flags
          16'd53: emit_byte = {7'd0, form_negative_q};
          16'd54: emit_byte = mca_pkg::WAVE_SAMPLE_NS[15:8];
          default: emit_byte = mca_pkg::WAVE_SAMPLE_NS[7:0];
        endcase
      end

      TX_FORM_SAMPLE: begin
        // Samples are written as whole 16-bit words immediately after the
        // header, so both lanes of every staging word are written at once.
        emit_word = wave_word;
      end

      default: ;
    endcase
  end

  // -----------------------------------------------------------------
  // Staging write port and byte sequencer
  // -----------------------------------------------------------------
  // `stg_out_of_range` is only ever high for a malformed internal sequence.
  // It is exposed so the behavioural simulation reports a broken sequencer
  // instead of silently corrupting the payload.
  logic stg_out_of_range;
  assign stg_out_of_range = stg_we &&
                            (stg_write_address >= (STAGE_BYTES / 2));

`ifdef MCA_NO_XPM
  always @(posedge clk)
    if (rst_n && stg_out_of_range)
      $display("ERROR [%0t] mca_net_tx staging address out of range", $time);
`endif

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      stg_we            <= 1'b0;
      stg_write_address <= '0;
      stg_write_data    <= '0;
      stage_byte_lo     <= '0;
    end else begin
      stg_we <= 1'b0;
      unique case (state)
        TX_EV_STAGE, TX_RESP_STAGE, TX_ST_STAGE: begin
          // The staging RAM is 16 bits wide.  Collect the even byte first,
          // then commit one complete word when the odd byte arrives.
          if (!emit_index[0]) begin
            stage_byte_lo <= emit_byte;
          end else begin
            stg_write_address <= emit_index[STAGE_AW-1:1];
            stg_write_data    <= {emit_byte, stage_byte_lo};
            stg_we            <= 1'b1;
          end
        end

        TX_FORM_HDR: begin
          if (!emit_index[0]) begin
            stage_byte_lo <= emit_byte;
          end else begin
            stg_write_address <= emit_index[STAGE_AW-1:1];
            stg_write_data    <= {emit_byte, stage_byte_lo};
            stg_we            <= 1'b1;
          end
        end

        TX_FORM_SAMPLE: begin
          // Sample k lives at byte offset 56 + 2k, which is staging address
          // 28 + k.  The address is computed from the sample index so it does
          // not depend on how many times the byte offset is stepped.
          stg_write_address <= STAGE_AW'(28) + form_write_index;
          // Network byte order is high byte first, while the transmitter
          // consumes the low lane before the high lane.
          stg_write_data    <= {emit_word[7:0], emit_word[15:8]};
          stg_we            <= 1'b1;
        end

        default: ;
      endcase
    end
  end

  // -----------------------------------------------------------------
  // Sequencer
  // -----------------------------------------------------------------
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state              <= TX_IDLE;
      byte_index         <= '0;
      payload_len        <= '0;
      batch_index        <= '0;
      batch_count        <= '0;
      pkt_sequence       <= '0;
      event_word<=0; pending_word<=0; pending_valid<=0; use_pending<=0;
      batch_type<=1; batch_version<=0; record_pos<=0; record_bytes<=12; batch_target<=0;
      event_timer        <= '0;
      tx_start_en        <= 1'b0;
      tx_byte_num        <= '0;
      resp_word          <= '0;
      tx_active          <= 1'b0;
      stg_read_address   <= '0;
      stg_prefetch_word  <= '0;
      tx_byte_register   <= '0;
      prime_count        <= '0;
      emit_index         <= '0;
      form_offset        <= '0;
      form_write_index   <= '0;
      form_sample_index  <= '0;
      form_sample_last_q <= 1'b0;
      form_capture_id_q  <= '0;
      form_timestamp_q   <= '0;
      form_index_q       <= '0;
      form_offset_q      <= '0;
      form_fragment_count_q <= '0;
      form_sample_count_q   <= '0;
      form_baseline_q    <= '0;
      form_threshold_q   <= '0;
      form_return_q      <= '0;
      form_negative_q    <= 1'b0;
      form_session_q     <= '0;
      form_version_q     <= '0;
      wave_ack_q         <= 1'b0;
      tx_is_waveform     <= 1'b0;
    end else begin
      tx_start_en <= 1'b0;
      // The staging RAM address is presented one cycle ahead of the byte
      // being consumed, so the registered data is valid exactly when the UDP
      // core asserts its byte request.
      stg_read_address <= stg_prefetch_word;

      // Keep acknowledgement asserted until the producer releases valid.
      // This prevents a one-cycle pulse from being missed across the buffer
      // ownership handshake and avoids accepting the same fragment twice.
      if (wave_ack_q && !wave_valid)
        wave_ack_q <= 1'b0;

      // Event age belongs only to an event that is waiting in the FIFO.  A
      // response or status packet must not make a waiting event look new, and
      // time spent transmitting a locked batch must not make the next partial
      // batch fire immediately on return to IDLE.
      if (event_empty) begin
        event_timer <= '0;
      end else if (!idle) begin
        event_timer <= '0;
      end else if ((event_level >= MAX_BATCH) ||
                   (event_timer >= EVENT_TIMEOUT_CYCLES)) begin
        // The batch is locked below in TX_IDLE; restart age for the next
        // batch, including the case where more events remain queued.
        event_timer <= '0;
      end else begin
        event_timer <= event_timer + 1'b1;
      end

      unique case (state)
        TX_IDLE: begin
          byte_index <= '0;
          if (!resp_empty) begin
            tx_is_waveform <= 1'b0;
            state <= TX_RESP_RD;
          end else if (!status_empty) begin
            tx_is_waveform <= 1'b0;
            state <= TX_ST_RD;
          end else if ((pending_valid || !event_empty) &&
                       (pending_valid || (event_level >= MAX_BATCH) ||
                        (event_timer >= EVENT_TIMEOUT_CYCLES))) begin
            tx_is_waveform <= 1'b0;
            // Latch the complete batch size before the first FIFO pop.  Each
            // following record gets its own read cycle; the previous code
            // popped once and repeated the same event MAX_BATCH times.
            batch_target <= (available_events >= MAX_BATCH) ? BATCH_COUNT_WIDTH'(MAX_BATCH) : available_events[BATCH_COUNT_WIDTH-1:0];
            batch_count<=0; batch_index<=0; record_pos<=0;
            emit_index<=16'd20; use_pending<=pending_valid;
            state<=pending_valid ? TX_EV_CAP : TX_EV_RD;
          end else if (wave_valid && !wave_ack_q) begin
            tx_is_waveform <= 1'b0;
            if (wave_cancel) begin
              // Discard this capture without sending it, and acknowledge the
              // fragment so the sampling domain releases the buffer.
              wave_ack_q <= 1'b1;
            end else begin
              form_capture_id_q     <= wave_capture_id;
              form_timestamp_q      <= wave_timestamp;
              form_baseline_q       <= wave_baseline;
              form_threshold_q      <= wave_threshold;
              form_return_q         <= wave_return;
              form_negative_q       <= wave_negative;
              form_session_q        <= wave_session_id;
              form_version_q        <= wave_config_version;
              form_index_q          <= wave_index;
              form_offset_q         <= wave_sample_offset;
              form_fragment_count_q <= wave_fragment_count;
              form_sample_count_q   <= wave_sample_count;
              tx_is_waveform        <= 1'b1;
              payload_len           <= WAVE_HEADER_BYTES[15:0]
                                       + (wave_sample_count << 1);
              emit_index            <= '0;
              form_offset           <= WAVE_HEADER_BYTES[15:0];
              form_write_index      <= '0;
              form_sample_index     <= '0;
              state                 <= TX_FORM_HDR;
            end
          end
        end

        // ---------------- event packet ----------------
        TX_EV_RD: state <= TX_EV_CAP;

        TX_EV_CAP: begin
          if (use_pending) pending_valid<=0;
          use_pending<=0;
          if (batch_count!=0 && (rx_type!=batch_type || rx_version!=batch_version)) begin
            pending_word<=rx_word; pending_valid<=1;
            payload_len<=emit_index; 
            emit_index<=0; state<=TX_EV_STAGE;
          end else begin
            event_word<=rx_word; record_pos<=0;
            if (batch_count==0) begin
              batch_type<=rx_type; batch_version<=rx_version;
              record_bytes<=rx_type==8'h07 ? 5'd24 : 5'd12;
              if (rx_type==8'h07 && batch_target>PSD_BATCH_LIMIT)
                batch_target<=BATCH_COUNT_WIDTH'(PSD_BATCH_LIMIT);
            end
            state<=TX_EV_STAGE;
          end
        end

        TX_EV_STAGE: begin
          emit_index<=emit_index+16'd1;
          if (emit_index<16'd20) begin
            if (emit_index==16'd19) begin
              stg_read_address<=0; stg_prefetch_word<=0; prime_count<=0;
              state<=TX_PRIME;
            end
          end else if (record_pos==record_bytes-1'b1) begin
            batch_count<=batch_count+1'b1;
            if ((batch_count+1'b1)>=batch_target || event_empty) begin
              payload_len<=emit_index+16'd1;
              
              emit_index<=0;
            end else state<=TX_EV_RD;
          end else record_pos<=record_pos+1'b1;
        end

        // ---------------- control response ----------------
        // The response FIFO uses standard read mode with one registered read
        // cycle.  Wait one cycle after asserting rd_en so the complete FIFO
        // item is stable before staging it.
        TX_RESP_RD: state <= TX_RESP_CAP;

        TX_RESP_CAP: begin
          resp_word  <= resp_dout;
          payload_len <= 16'd36; // 20-byte MCA2 header + one 16-byte MCF2 record
          emit_index <= '0;
          state       <= TX_RESP_STAGE;
        end

        TX_RESP_STAGE: begin
          emit_index <= emit_index + 16'd1;
          if ((emit_index + 16'd1) >= 16'd36) begin
            stg_read_address  <= '0;
            stg_prefetch_word <= '0;
            prime_count       <= '0;
            state <= TX_PRIME;
          end
        end

        // ---------------- status ----------------
        TX_ST_RD: begin
          payload_len <= 16'd20 + STATUS_BYTES[15:0];
          emit_index <= '0;
          state      <= TX_ST_STAGE;
        end

        TX_ST_STAGE: begin
          emit_index <= emit_index + 16'd1;
          if ((emit_index + 16'd1) >= (16'd20 + STATUS_BYTES[15:0])) begin
            stg_read_address  <= '0;
            stg_prefetch_word <= '0;
            prime_count       <= '0;
            state <= TX_PRIME;
          end
        end

        // ---------------- waveform fragment ----------------
        TX_FORM_HDR: begin
          emit_index <= emit_index + 16'd1;
          if ((emit_index + 16'd1) >= WAVE_HEADER_BYTES[15:0])
            state <= TX_FORM_READ;
        end

        TX_FORM_READ: begin
          if (wave_word_valid) begin
            form_sample_index <= form_sample_index + 16'd1;
            form_sample_last_q <=
                ((form_sample_index + 16'd1) >= form_sample_count_q);
            state <= TX_FORM_SAMPLE;
          end
        end

        TX_FORM_SAMPLE: begin
          form_offset      <= form_offset + 16'd1;
          form_write_index <= form_write_index + 1'b1;
          // The final word must pass through TX_FORM_SAMPLE as well; going
          // directly from READ to PRIME left the last staging word unwritten.
          if (form_sample_last_q) begin
              stg_read_address  <= '0;
              stg_prefetch_word <= '0;
              prime_count       <= '0;
              state <= TX_PRIME;
          end else begin
            state <= TX_FORM_READ;
          end
        end

        // ---------------- transmission ----------------
        TX_PRIME: begin
          // Cycle 0: `stg_read_address` is already 0, so `stg_dout` holds
          // staging byte 0 at the end of this cycle.  Cycle 1: capture byte 1
          // and move the read address on.  The read-only lanes below then
          // keep one byte of look-ahead for the rest of the packet.
          if (prime_count == 3'd0) begin
            tx_byte_register <= stg_dout;            // byte 0
            prime_count      <= 3'd1;
          end else if (prime_count == 3'd1) begin
            tx_byte_register  <= stg_dout;           // byte 1
            // Start the one-word look-ahead.  The registered RAM output for
            // word 1 is then ready when byte 2 is requested.
            stg_prefetch_word <= STAGE_AW'(1);
            prime_count       <= 3'd2;
          end else begin
            prime_count   <= '0;
            byte_index    <= '0;
            tx_byte_num   <= payload_len;
            pkt_sequence  <= pkt_sequence + 32'd1;
            state         <= TX_DATA;
          end
        end

        TX_DATA: begin
          if (!tx_active) begin
            tx_start_en <= 1'b1;
            tx_active   <= 1'b1;
            byte_index  <= '0;
            tx_data     <= tx_byte_register[7:0];
          end else if (tx_done) begin
            if (tx_is_waveform)
              wave_ack_q <= 1'b1;
            tx_is_waveform <= 1'b0;
            tx_active <= 1'b0;
            state     <= TX_IDLE;
          end else if (tx_data_req && (byte_index + 1'b1 < payload_len)) begin
            byte_index <= byte_index + 1'b1;
            if (!byte_index[0]) begin
              // The odd byte shares the currently registered word with the
              // even byte already in tx_byte_register.
              tx_data <= tx_byte_register[15:8];
            end else begin
              // At an odd byte index the next word has arrived from the
              // one-cycle RAM read pipeline.  Emit its low lane and retain
              // the word for the following high lane.
              tx_data          <= stg_dout[7:0];
              tx_byte_register <= stg_dout;
              if (byte_index + 16'd1 < payload_len) begin
                // `stg_read_address` drives a synchronous BRAM directly.
                // Advance it here rather than waiting for the look-ahead
                // register to add another cycle of latency.
                stg_read_address  <= STAGE_AW'(byte_index[15:1] + 16'd2);
                stg_prefetch_word <= STAGE_AW'(byte_index[15:1] + 16'd2);
              end
            end
          end
        end

        default: state <= TX_IDLE;
      endcase
    end
  end

  assign wave_ack = wave_ack_q;
endmodule
