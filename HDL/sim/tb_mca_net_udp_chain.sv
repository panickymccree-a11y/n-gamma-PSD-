`timescale 1ns/1ps

// End-to-end byte timing check using the real UDP transmitter request timing.
module tb_mca_net_udp_chain;
  localparam int STATUS_BYTES = 128;
  logic clk = 0, rst_n = 0;
  always #4 clk = ~clk;

  logic event_rd_en, resp_rd_en, status_rd_en, wave_ack;
  logic tx_start_en, tx_done, udp_req, producer_req_d, mux_req_d;
  logic [15:0] tx_byte_num, wave_word_index;
  logic [7:0] tx_data, gmii_data;
  logic gmii_en;
  logic [519:0] resp_dout = '0;
  integer errors = 0, frame_index = 0;

  wire [7:0] udp_data = mux_req_d ? tx_data : 8'd0;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      producer_req_d <= 0;
      mux_req_d <= 0;
    end else begin
      // Same parallel request registers as mca_eth_udp and mca_eth_ctrl.
      producer_req_d <= udp_req;
      mux_req_d <= udp_req;
    end
  end

  mca_net_tx #(.STATUS_BYTES(STATUS_BYTES), .MAX_BATCH(8),
               .EVENT_TIMEOUT_CYCLES(8), .STAGE_BYTES(1080)) dut (
    .clk(clk), .rst_n(rst_n), .device_id(8'd0),
    .event_empty(1'b1), .event_rd_en(event_rd_en), .event_dout(96'd0),
    .event_level(9'd0), .resp_empty(resp_rd_en),
    .resp_rd_en(resp_rd_en), .resp_dout(resp_dout),
    .status_empty(1'b1), .status_rd_en(status_rd_en),
    .status_dout({STATUS_BYTES*8{1'b0}}),
    .wave_valid(1'b0), .wave_ack(wave_ack), .wave_cancel(1'b0),
    .wave_index(16'd0), .wave_sample_count(16'd0),
    .wave_sample_offset(16'd0), .wave_fragment_count(16'd0),
    .wave_last(1'b0), .wave_capture_id(32'd0), .wave_timestamp(48'd0),
    .wave_baseline(14'd0), .wave_threshold(14'd0), .wave_return(14'd0),
    .wave_negative(1'b0), .wave_config_version(32'd0),
    .wave_session_id(32'd0), .wave_word_index(wave_word_index),
    .wave_word_req(), .wave_word(16'd0), .wave_word_valid(1'b0),
    .cfg_version(32'h0001_001e), .tx_start_en(tx_start_en),
    .tx_byte_num(tx_byte_num), .tx_data(tx_data),
    .tx_data_req(producer_req_d), .tx_done(tx_done)
  );

  udp_tx u_udp (
    .clk(clk), .rst_n(rst_n), .tx_start_en(tx_start_en),
    .tx_data(udp_data), .tx_byte_num(tx_byte_num),
    .des_mac(48'h020000000001), .des_ip(32'hc0a80164),
    .crc_data(32'd0), .crc_next(8'd0), .tx_done(tx_done),
    .tx_req(udp_req), .gmii_tx_en(gmii_en), .gmii_txd(gmii_data),
    .crc_en(), .crc_clr()
  );

  task automatic check_byte(input int i, input logic [7:0] got);
    logic [7:0] exp;
    begin
      case (i)
        0:exp=8'h4d; 1:exp=8'h43; 2:exp=8'h41; 3:exp=8'h32;
        4:exp=2; 5:exp=5; 6,7,8,9,10,11:exp=0;
        12:exp=0; 13:exp=1; 14:exp=0; 15:exp=8'h1e;
        16:exp=0; 17:exp=1; 18:exp=0; 19:exp=8'h10;
        20:exp=0; 21:exp=0; 22:exp=8'h0f; 23:exp=8'h7a;
        24:exp=2; 25,26,27:exp=0; 28:exp=1; 29:exp=8'h21;
        30,31,32,33,34,35:exp=0;
        default: exp=8'hxx;
      endcase
      if (got !== exp) begin
        errors = errors + 1;
        $display("FAIL payload[%0d]=%02x expected %02x", i, got, exp);
      end
    end
  endtask

  // Preamble + Ethernet + IPv4 + UDP headers occupy the first 50 bytes.
  always_ff @(posedge clk) begin
    if (!rst_n)
      frame_index <= 0;
    else if (gmii_en) begin
      if ((frame_index >= 50) && (frame_index < 86))
        check_byte(frame_index - 50, gmii_data);
      frame_index <= frame_index + 1;
    end
  end

  initial begin
    resp_dout[519:512] = 8'd16;
    resp_dout[7:0]=0; resp_dout[15:8]=0;
    resp_dout[23:16]=8'h0f; resp_dout[31:24]=8'h7a;
    resp_dout[39:32]=2; resp_dout[71:64]=1; resp_dout[79:72]=8'h21;
    repeat (5) @(posedge clk);
    rst_n = 1;
    wait (tx_done);
    repeat (5) @(posedge clk);
    if (errors == 0) $display("tb_mca_net_udp_chain PASS");
    else $display("tb_mca_net_udp_chain FAIL: %0d errors", errors);
    $finish;
  end
endmodule
