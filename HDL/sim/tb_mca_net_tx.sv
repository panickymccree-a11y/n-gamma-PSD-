`timescale 1ns/1ps

module tb_mca_net_tx;
  localparam int STATUS_BYTES = 128;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic event_empty = 1'b0;
  logic event_rd_en;
  logic [231:0] event_dout = 96'h0102_0304_0506_0708_090A_0B0C;
  logic [8:0] event_level = 9'd1;
  logic resp_empty = 1'b0;
  logic resp_rd_en;
  logic [519:0] resp_dout = '0;
  logic status_empty = 1'b0;
  logic status_rd_en;
  logic [STATUS_BYTES*8-1:0] status_dout = '0;
  logic tx_start_en;
  logic [15:0] tx_byte_num;
  logic [7:0] tx_data;
  logic tx_data_req = 1'b0;
  logic tx_done = 1'b0;

  // Waveform fragment interface.
  localparam int WAVE_SAMPLES = 4;
  logic        wave_valid = 1'b0;
  logic        wave_ack;
  logic        wave_cancel = 1'b0;
  logic [15:0] wave_index = 16'd3;
  logic [15:0] wave_sample_count = WAVE_SAMPLES[15:0];
  logic [15:0] wave_sample_offset = 16'd1536;
  logic [15:0] wave_fragment_count = 16'd25;
  logic        wave_last = 1'b0;
  logic [31:0] wave_capture_id = 32'h0000_00A5;
  logic [47:0] wave_timestamp = 48'h0000_1122_3344;
  logic [13:0] wave_baseline = 14'd8200;
  logic [13:0] wave_threshold = 14'd120;
  logic [13:0] wave_return = 14'd32;
  logic        wave_negative = 1'b1;
  logic [31:0] wave_config_version = 32'h0002_0003;
  logic [31:0] wave_session_id = 32'hDEAD_BEEF;
  logic [15:0] wave_word_index;
  logic        wave_word_req;
  logic [15:0] wave_word = 16'h0000;
  logic        wave_word_valid = 1'b0;

  int errors = 0;

  mca_net_tx #(.EVENT_TIMEOUT_CYCLES(2), .MAX_BATCH(8),
               .STATUS_BYTES(STATUS_BYTES),
               .STAGE_BYTES(1080)) dut (
    .clk(clk), .rst_n(rst_n), .device_id(8'h5A),
    .event_empty(event_empty), .event_rd_en(event_rd_en),
    .event_dout(event_dout), .event_level(event_level),
    .resp_empty(resp_empty), .resp_rd_en(resp_rd_en), .resp_dout(resp_dout),
    .status_empty(status_empty), .status_rd_en(status_rd_en),
    .status_dout(status_dout),
    .wave_valid(wave_valid), .wave_ack(wave_ack), .wave_cancel(wave_cancel),
    .wave_index(wave_index), .wave_sample_count(wave_sample_count),
    .wave_sample_offset(wave_sample_offset),
    .wave_fragment_count(wave_fragment_count), .wave_last(wave_last),
    .wave_capture_id(wave_capture_id), .wave_timestamp(wave_timestamp),
    .wave_baseline(wave_baseline), .wave_threshold(wave_threshold),
    .wave_return(wave_return), .wave_negative(wave_negative),
    .wave_config_version(wave_config_version),
    .wave_session_id(wave_session_id),
    .wave_word_index(wave_word_index), .wave_word_req(wave_word_req),
    .wave_word(wave_word), .wave_word_valid(wave_word_valid),
    .cfg_version(32'h1122_3344),
    .tx_start_en(tx_start_en), .tx_byte_num(tx_byte_num), .tx_data(tx_data),
    .tx_data_req(tx_data_req), .tx_done(tx_done)
  );

  always @(posedge clk) begin
    if (status_rd_en) status_empty <= 1'b1;
    if (resp_rd_en)   resp_empty   <= 1'b1;
    if (event_rd_en) begin
      if (event_level > 9'd1) begin
        event_level <= event_level - 9'd1;
        event_empty <= 1'b0;
      end else begin
        event_level <= 9'd0;
        event_empty <= 1'b1;
      end
    end
  end

  task automatic check(input bit condition, input string message);
    if (!condition) begin
      errors++;
      $display("FAIL [%0t] %s", $time, message);
    end
  endtask

  task automatic wait_start(input int expected_length);
    int guard;
    begin
      guard = 0;
      while (!tx_start_en && guard < 400) begin
        @(posedge clk); #1;
        guard++;
      end
      check(tx_start_en, "transmit start timeout");
      check(tx_byte_num == expected_length,
            $sformatf("packet length %0d expected %0d", tx_byte_num,
                      expected_length));
    end
  endtask

  task automatic finish_packet;
    begin
      @(negedge clk); tx_done = 1'b1;
      @(negedge clk); tx_done = 1'b0;
      repeat (2) @(posedge clk);
    end
  endtask

  initial begin
    logic [7:0] expected;
    for (int i = 0; i < STATUS_BYTES; i++)
      status_dout[i*8 +: 8] = i[7:0] ^ 8'hA5;
    resp_dout[519:512] = 8'd16;
    for (int i = 0; i < 16; i++)
      resp_dout[i*8 +: 8] = 8'hC0 + i[7:0];

    repeat (5) @(posedge clk);
    rst_n = 1'b1;

    // A control response wins arbitration over simultaneous status/event
    // traffic so commands remain responsive at high count rate.
    wait_start(36);
    for (int i = 0; i < 36; i++) begin
      case (i)
        0: expected=8'h4D; 1: expected=8'h43; 2: expected=8'h41;
        3: expected=8'h32; 4: expected=8'd2; 5: expected=8'd5;
        6: expected=8'h5A; 7: expected=8'd0;
        8,9,10,11: expected=8'd0;
        12: expected=8'h11; 13: expected=8'h22;
        14: expected=8'h33; 15: expected=8'h44;
        16: expected=8'd0; 17: expected=8'd1;
        18: expected=8'd0; 19: expected=8'd16;
        default: expected=8'hC0 + (i-20);
      endcase
      @(negedge clk);
      check(tx_data === expected,
            $sformatf("response byte %0d = %02x expected %02x", i,
                      tx_data, expected));
      tx_data_req = 1'b1;
    end
    @(negedge clk); tx_data_req = 1'b0;
    finish_packet();

    // The queued status packet follows the response.
    wait_start(20 + STATUS_BYTES);
    for (int i = 0; i < 20 + STATUS_BYTES; i++) begin
      case (i)
        0: expected=8'h4D; 1: expected=8'h43; 2: expected=8'h41;
        3: expected=8'h32; 4: expected=8'd2; 5: expected=8'd4;
        6: expected=8'h5A; 7: expected=8'd0;
        8,9,10: expected=8'd0; 11: expected=8'd1;
        12: expected=8'h11; 13: expected=8'h22;
        14: expected=8'h33; 15: expected=8'h44;
        16: expected=8'd0; 17: expected=8'd1;
        18: expected=8'd0; 19: expected=STATUS_BYTES;
        default: expected=(i-20) ^ 8'hA5;
      endcase
      @(negedge clk);
      check(tx_data === expected,
            $sformatf("status byte %0d = %02x", i, tx_data));
      tx_data_req = 1'b1;
    end
    @(negedge clk); tx_data_req = 1'b0;
    finish_packet();

    // The queued event is emitted after the bounded batching timeout.
    wait_start(32);
    for (int i = 0; i < 32; i++) begin
      case (i)
        0: expected=8'h4D; 1: expected=8'h43; 2: expected=8'h41;
        3: expected=8'h32; 4: expected=8'd2; 5: expected=8'd1;
        6: expected=8'h5A; 7: expected=8'd0;
        8: expected=8'd0; 9: expected=8'd0; 10: expected=8'd0;
        11: expected=8'd2;
        12: expected=8'h11; 13: expected=8'h22;
        14: expected=8'h33; 15: expected=8'h44;
        16: expected=8'd0; 17: expected=8'd1;
        18: expected=8'd0; 19: expected=8'd12;
        default: expected=i-19;
      endcase
      @(negedge clk);
      check(tx_data === expected,
            $sformatf("event byte %0d = %02x expected %02x", i,
                      tx_data, expected));
      tx_data_req = 1'b1;
    end
    @(negedge clk); tx_data_req = 1'b0;
    finish_packet();

    // A full eight-event batch exercises the real top-level MAX_BATCH value.
    // The count must remain 8 (not truncate to zero), so the MCA2 header and
    // UDP payload both describe 96 event bytes.
    @(negedge clk); // FIFO level is stable before the transmitter clock edge.
    event_empty = 1'b0;
    event_level = 9'd8;
    wait_start(20 + 8 * 12);
    for (int i = 0; i < 20; i++) begin
      @(negedge clk);
      case (i)
        0: expected=8'h4D; 1: expected=8'h43; 2: expected=8'h41;
        3: expected=8'h32; 4: expected=8'd2; 5: expected=8'd1;
        6: expected=8'h5A; 7: expected=8'd0;
        8,9,10: expected=8'd0; 11: expected=8'd3;
        12: expected=8'h11; 13: expected=8'h22;
        14: expected=8'h33; 15: expected=8'h44;
        16: expected=8'd0; 17: expected=8'd8;
        18: expected=8'd0; 19: expected=8'd96;
      endcase
      check(tx_data === expected,
            $sformatf("full event header byte %0d = %02x", i, tx_data));
      tx_data_req = 1'b1;
    end
    @(negedge clk); tx_data_req = 1'b0;
    finish_packet();

    check(status_empty && resp_empty && event_empty,
          "not all packet sources were consumed");

    // ------------------------------------------------------------------
    // Waveform fragment: MCA2 type 6 with the 4-byte session id, the 32-byte
    // fragment header and the sample words in network byte order.  The
    // sample reader must answer exactly the requested indices.
    // ------------------------------------------------------------------
    event_empty = 1'b1;
    status_empty = 1'b1;
    resp_empty = 1'b1;
    wave_valid = 1'b1;
    wave_word_valid = 1'b0;
    wave_word = 16'h0000;
    // Background responder thread.
    fork
      begin : sample_responder
        int expected_index;
        expected_index = 0;
        forever begin
          @(posedge clk);
          #1;
          if (wave_word_req) begin
            check(wave_word_index == expected_index,
                  $sformatf("waveform word index %0d expected %0d",
                            wave_word_index, expected_index));
            wave_word = 16'h4000 | (16'h1000 + expected_index[15:0]);
            wave_word_valid = 1'b1;
            expected_index++;
          end else begin
            wave_word_valid = 1'b0;
          end
        end
      end
    join_none

    // Checker thread.
    begin
      logic [7:0] expect_byte;
      logic [15:0] expect_word;
      int sample;
      // 20-byte common header + 4-byte session id + 32-byte fragment header
      // + 4 sample words.
      wait_start(20 + 4 + 32 + WAVE_SAMPLES * 2);
      for (int i = 0; i < 20 + 4 + 32 + WAVE_SAMPLES * 2; i++) begin
        @(negedge clk);
        case (i)
          0:  expect_byte = 8'h4D;
          1:  expect_byte = 8'h43;
          2:  expect_byte = 8'h41;
          3:  expect_byte = 8'h32;
          4:  expect_byte = 8'd2;
          5:  expect_byte = 8'd6;               // MCA2 type 6
          6:  expect_byte = 8'h5A;
          7:  expect_byte = 8'd0;
          // The shared sequence counter has already issued four packets.
          8:  expect_byte = 8'd0;
          9:  expect_byte = 8'd0;
          10: expect_byte = 8'd0;
          11: expect_byte = 8'd4;
          12: expect_byte = 8'h00;              // trigger-latched version
          13: expect_byte = 8'h02;
          14: expect_byte = 8'h00;
          15: expect_byte = 8'h03;
          16: expect_byte = 8'd0;
          17: expect_byte = 8'd1;               // record_count = 1
          18: expect_byte = 8'd0;
          19: expect_byte = 4 + 32 + WAVE_SAMPLES * 2;
          // 4-byte session id
          20: expect_byte = 8'hDE;
          21: expect_byte = 8'hAD;
          22: expect_byte = 8'hBE;
          23: expect_byte = 8'hEF;
          // 32-byte fragment header
          24: expect_byte = 8'h00; 25: expect_byte = 8'h00;
          26: expect_byte = 8'h00; 27: expect_byte = 8'hA5;   // capture_id
          28: expect_byte = 8'h00; 29: expect_byte = 8'h00;
          30: expect_byte = 8'h11; 31: expect_byte = 8'h22;
          32: expect_byte = 8'h33; 33: expect_byte = 8'h44;   // timestamp
          34: expect_byte = 8'h20; 35: expect_byte = 8'h08;   // baseline 8200
          36: expect_byte = 8'h00; 37: expect_byte = 8'h78;   // threshold
          38: expect_byte = 8'h00; 39: expect_byte = 8'h20;   // return
          40: expect_byte = 8'h0E; 41: expect_byte = 8'hA6;   // pre = 3750
          42: expect_byte = 8'h30; 43: expect_byte = 8'hD4;   // total = 12500
          44: expect_byte = 8'h06; 45: expect_byte = 8'h00;   // offset 1536
          46: expect_byte = 8'h00; 47: expect_byte = 8'd4;    // sample count
          48: expect_byte = 8'h00; 49: expect_byte = 8'd3;    // fragment index
          50: expect_byte = 8'h00; 51: expect_byte = 8'd25;   // fragment count
          52: expect_byte = 8'h00;                            // reserved flags
          53: expect_byte = 8'h01;                            // negative
          54: expect_byte = 8'h00; 55: expect_byte = 8'h08;   // 8 ns
          default: begin
            sample = (i - 56) / 2;
            expect_word = 16'h4000 | (16'h1000 + sample[15:0]);
            expect_byte = ((i - 56) % 2 == 0)
                              ? expect_word[15:8] : expect_word[7:0];
          end
        endcase
        check(tx_data === expect_byte,
              $sformatf("waveform byte %0d = %02x expected %02x", i,
                        tx_data, expect_byte));
        tx_data_req = 1'b1;
      end
      @(negedge clk); tx_data_req = 1'b0;
      finish_packet();
      check(wave_ack, "waveform fragment was not acknowledged");
    end
    disable sample_responder;
    wave_valid = 1'b0;
    wave_word_valid = 1'b0;

    // Every sample index of the fragment must have been requested exactly
    // once, in order.
    if (errors == 0)
      $display("tb_mca_net_tx PASS");
    else
      $display("tb_mca_net_tx FAIL: %0d errors", errors);
    $finish;
  end
endmodule
