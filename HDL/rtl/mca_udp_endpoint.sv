`timescale 1ns/1ps

module mca_udp_endpoint #(
  parameter logic [47:0] LOCAL_MAC = 48'h02_12_34_56_78_9A,
  parameter logic [47:0] HOST_MAC  = 48'h02_00_00_00_00_01,
  parameter logic [31:0] LOCAL_IP  = 32'hC0A8_010A, // 192.168.1.10
  parameter logic [31:0] HOST_IP   = 32'hC0A8_0164, // 192.168.1.100
  parameter int UDP_RX_PORT = 5000,
  parameter int UDP_TX_PORT = 5001,
  parameter int EVENT_TIMEOUT_CYCLES = 12_500 // 100 us at 125 MHz
) (
  input  logic         clk,
  input  logic         rst_n,
  input  logic         rx_byte_valid,
  input  logic [7:0]   rx_byte,
  input  logic         rx_frame_end,
  output logic         cmd_wr,
  output logic [511:0] cmd_payload,
  output logic [7:0]   cmd_len,
  input  logic         cmd_full,
  input  logic         resp_empty,
  output logic         resp_rd_en,
  input  logic [519:0] resp_dout,
  input  logic         event_empty,
  output logic         event_rd_en,
  input  logic [95:0]  event_dout,
  input  logic [8:0]   event_level,
  output logic         tx_valid,
  output logic [7:0]   tx_byte,
  output logic         tx_last
);
  localparam int FRAME_HEADER_BYTES = 42;
  localparam int TX_PREFIX_BYTES = 8;

  typedef enum logic [2:0] {RX_SEARCH, RX_FRAME, TX_IDLE, TX_LOAD_RESP,
                            TX_CAPTURE_RESP, TX_LOAD_EVENT, TX_CAPTURE_EVENT,
                            TX_SEND} tx_state_t;
  tx_state_t state;

  logic [7:0] rx_pos;
  logic [7:0] payload_count;
  logic [7:0] rx_mem [0:63];
  logic [47:0] rx_dst_mac;
  logic [15:0] rx_ethertype;
  logic [7:0] rx_ip_protocol;
  logic [31:0] rx_dst_ip;
  logic [15:0] rx_udp_dst_port;
  logic [15:0] rx_udp_length;
  logic [7:0] preamble_count;

  logic [7:0] tx_mem [0:255];
  logic [8:0] tx_total;
  logic [8:0] tx_index;
  logic [15:0] event_timer;
  logic [2:0] event_count;
  logic [7:0] tx_payload_len;

  assign resp_rd_en = (state == TX_LOAD_RESP);
  assign event_rd_en = (state == TX_LOAD_EVENT);
  assign tx_valid = (state == TX_SEND);
  assign tx_byte = tx_mem[tx_index];
  assign tx_last = (state == TX_SEND) && (tx_index == tx_total-1'b1);

  function automatic logic [15:0] ip_checksum(input logic [15:0] total_length);
    logic [31:0] sum;
    begin
      sum = 32'd0;
      sum = sum + 16'h4500;
      sum = sum + total_length;
      sum = sum + 16'h0000;
      sum = sum + 16'h4000;
      sum = sum + 16'h4011;
      sum = sum + LOCAL_IP[31:16];
      sum = sum + LOCAL_IP[15:0];
      sum = sum + HOST_IP[31:16];
      sum = sum + HOST_IP[15:0];
      sum = (sum[31:16] + sum[15:0]);
      sum = (sum[31:16] + sum[15:0]);
      ip_checksum = ~sum[15:0];
    end
  endfunction

  task automatic build_headers(input logic [15:0] payload_length);
    logic [15:0] total_length;
    logic [15:0] udp_length;
    logic [15:0] header_checksum;
    begin
      total_length = 16'd28 + payload_length;
      udp_length   = 16'd8 + payload_length;
      header_checksum = ip_checksum(total_length);
      tx_mem[0] = 8'h55; tx_mem[1] = 8'h55; tx_mem[2] = 8'h55; tx_mem[3] = 8'h55;
      tx_mem[4] = 8'h55; tx_mem[5] = 8'h55; tx_mem[6] = 8'h55; tx_mem[7] = 8'hD5;
      tx_mem[TX_PREFIX_BYTES+0]  = HOST_MAC[47:40]; tx_mem[TX_PREFIX_BYTES+1]  = HOST_MAC[39:32];
      tx_mem[TX_PREFIX_BYTES+2]  = HOST_MAC[31:24]; tx_mem[TX_PREFIX_BYTES+3]  = HOST_MAC[23:16];
      tx_mem[TX_PREFIX_BYTES+4]  = HOST_MAC[15:8];  tx_mem[TX_PREFIX_BYTES+5]  = HOST_MAC[7:0];
      tx_mem[TX_PREFIX_BYTES+6]  = LOCAL_MAC[47:40]; tx_mem[TX_PREFIX_BYTES+7]  = LOCAL_MAC[39:32];
      tx_mem[TX_PREFIX_BYTES+8]  = LOCAL_MAC[31:24]; tx_mem[TX_PREFIX_BYTES+9]  = LOCAL_MAC[23:16];
      tx_mem[TX_PREFIX_BYTES+10] = LOCAL_MAC[15:8]; tx_mem[TX_PREFIX_BYTES+11] = LOCAL_MAC[7:0];
      tx_mem[TX_PREFIX_BYTES+12] = 8'h08; tx_mem[TX_PREFIX_BYTES+13] = 8'h00;
      tx_mem[TX_PREFIX_BYTES+14] = 8'h45; tx_mem[TX_PREFIX_BYTES+15] = 8'h00;
      tx_mem[TX_PREFIX_BYTES+16] = total_length[15:8]; tx_mem[TX_PREFIX_BYTES+17] = total_length[7:0];
      tx_mem[TX_PREFIX_BYTES+18] = 8'h00; tx_mem[TX_PREFIX_BYTES+19] = 8'h00;
      tx_mem[TX_PREFIX_BYTES+20] = 8'h40; tx_mem[TX_PREFIX_BYTES+21] = 8'h00;
      tx_mem[TX_PREFIX_BYTES+22] = 8'h40; tx_mem[TX_PREFIX_BYTES+23] = 8'h11;
      tx_mem[TX_PREFIX_BYTES+24] = header_checksum[15:8];
      tx_mem[TX_PREFIX_BYTES+25] = header_checksum[7:0];
      tx_mem[TX_PREFIX_BYTES+26] = LOCAL_IP[31:24]; tx_mem[TX_PREFIX_BYTES+27] = LOCAL_IP[23:16];
      tx_mem[TX_PREFIX_BYTES+28] = LOCAL_IP[15:8];  tx_mem[TX_PREFIX_BYTES+29] = LOCAL_IP[7:0];
      tx_mem[TX_PREFIX_BYTES+30] = HOST_IP[31:24]; tx_mem[TX_PREFIX_BYTES+31] = HOST_IP[23:16];
      tx_mem[TX_PREFIX_BYTES+32] = HOST_IP[15:8];  tx_mem[TX_PREFIX_BYTES+33] = HOST_IP[7:0];
      tx_mem[TX_PREFIX_BYTES+34] = UDP_TX_PORT[15:8]; tx_mem[TX_PREFIX_BYTES+35] = UDP_TX_PORT[7:0];
      tx_mem[TX_PREFIX_BYTES+36] = UDP_RX_PORT[15:8]; tx_mem[TX_PREFIX_BYTES+37] = UDP_RX_PORT[7:0];
      tx_mem[TX_PREFIX_BYTES+38] = udp_length[15:8]; tx_mem[TX_PREFIX_BYTES+39] = udp_length[7:0];
      tx_mem[TX_PREFIX_BYTES+40] = 8'h00; tx_mem[TX_PREFIX_BYTES+41] = 8'h00; // IPv4 permits zero UDP checksum.
    end
  endtask

  integer k;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rx_pos <= '0;
      payload_count <= '0;
      cmd_wr <= 1'b0;
      cmd_payload <= '0;
      cmd_len <= '0;
      rx_dst_mac <= '0;
      rx_ethertype <= '0;
      rx_ip_protocol <= '0;
      rx_dst_ip <= '0;
      rx_udp_dst_port <= '0;
      rx_udp_length <= '0;
      preamble_count <= '0;
      state <= TX_IDLE;
      tx_total <= '0;
      tx_index <= '0;
      tx_payload_len <= '0;
      event_timer <= '0;
      event_count <= '0;
    end else begin
      cmd_wr <= 1'b0;

      if (rx_byte_valid) begin
        if (rx_pos == 0 && rx_byte == 8'hD5) begin
          rx_pos <= 8'd0;
          payload_count <= '0;
          preamble_count <= '0;
          state <= (state == TX_SEND) ? state : RX_FRAME;
        end else if (state == RX_FRAME) begin
          if (rx_pos < 8'hFF) begin
            if (rx_pos == 0) rx_dst_mac[47:40] <= rx_byte;
            if (rx_pos == 1) rx_dst_mac[39:32] <= rx_byte;
            if (rx_pos == 2) rx_dst_mac[31:24] <= rx_byte;
            if (rx_pos == 3) rx_dst_mac[23:16] <= rx_byte;
            if (rx_pos == 4) rx_dst_mac[15:8] <= rx_byte;
            if (rx_pos == 5) rx_dst_mac[7:0] <= rx_byte;
            if (rx_pos == 12) rx_ethertype[15:8] <= rx_byte;
            if (rx_pos == 13) rx_ethertype[7:0] <= rx_byte;
            if (rx_pos == 23) rx_ip_protocol <= rx_byte;
            if (rx_pos == 30) rx_dst_ip[31:24] <= rx_byte;
            if (rx_pos == 31) rx_dst_ip[23:16] <= rx_byte;
            if (rx_pos == 32) rx_dst_ip[15:8] <= rx_byte;
            if (rx_pos == 33) rx_dst_ip[7:0] <= rx_byte;
            if (rx_pos == 36) rx_udp_dst_port[15:8] <= rx_byte;
            if (rx_pos == 37) rx_udp_dst_port[7:0] <= rx_byte;
            if (rx_pos == 38) rx_udp_length[15:8] <= rx_byte;
            if (rx_pos == 39) rx_udp_length[7:0] <= rx_byte;
            if (rx_pos >= 42 && payload_count < 64) begin
              rx_mem[payload_count] <= rx_byte;
              payload_count <= payload_count + 1'b1;
            end
            rx_pos <= rx_pos + 1'b1;
          end
        end else if (rx_byte == 8'h55 && preamble_count < 8'd8) begin
          preamble_count <= preamble_count + 1'b1;
        end
      end

      if (rx_frame_end) begin
        if (state == RX_FRAME && rx_ethertype == 16'h0800 && rx_ip_protocol == 8'h11 &&
            rx_dst_ip == LOCAL_IP && rx_udp_dst_port == UDP_RX_PORT[15:0]) begin
          cmd_payload <= '0;
          for (k = 0; k < 64; k = k + 1)
            cmd_payload[k*8 +: 8] <= rx_mem[k];
          cmd_len <= payload_count;
          if (!cmd_full)
            cmd_wr <= 1'b1;
        end
        rx_pos <= '0;
        payload_count <= '0;
        preamble_count <= '0;
        state <= TX_IDLE;
      end

      if (state == TX_IDLE) begin
        if (event_empty) begin
          event_timer <= '0;
        end else if (event_timer != 16'hFFFF) begin
          event_timer <= event_timer + 1'b1;
        end
        if (!resp_empty) begin
          state <= TX_LOAD_RESP;
        end else if (!event_empty && ((event_level >= 5'd8) || (event_timer >= EVENT_TIMEOUT_CYCLES))) begin
          event_count <= '0;
          tx_payload_len <= '0;
          state <= TX_LOAD_EVENT;
        end
      end else if (state == TX_CAPTURE_RESP) begin
        build_headers(resp_dout[519:512]);
        tx_payload_len <= resp_dout[519:512];
        for (k = 0; k < 64; k = k + 1)
          tx_mem[TX_PREFIX_BYTES+42+k] <= resp_dout[k*8 +: 8];
        tx_total <= TX_PREFIX_BYTES + 42 + resp_dout[519:512];
        tx_index <= '0;
        state <= TX_SEND;
      end else if (state == TX_CAPTURE_EVENT) begin
        build_headers(16'd12 * (event_count + 1'b1));
        tx_payload_len <= 12 * (event_count + 1'b1);
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 0] <= event_dout[95:88];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 1] <= event_dout[87:80];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 2] <= event_dout[79:72];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 3] <= event_dout[71:64];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 4] <= event_dout[63:56];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 5] <= event_dout[55:48];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 6] <= event_dout[47:40];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 7] <= event_dout[39:32];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 8] <= event_dout[31:24];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 9] <= event_dout[23:16];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 10] <= event_dout[15:8];
        tx_mem[TX_PREFIX_BYTES+42 + event_count*12 + 11] <= event_dout[7:0];
        if ((event_count == 3'd7) || event_empty) begin
        tx_total <= TX_PREFIX_BYTES + 42 + 12 * (event_count + 1'b1);
          tx_index <= '0;
          state <= TX_SEND;
        end else begin
          event_count <= event_count + 1'b1;
          state <= TX_LOAD_EVENT;
        end
      end else if (state == TX_SEND) begin
        if (tx_index == tx_total-1'b1) begin
          tx_index <= '0;
          event_timer <= '0;
          state <= TX_IDLE;
        end else begin
          tx_index <= tx_index + 1'b1;
        end
      end else if (state == TX_LOAD_RESP) begin
        state <= TX_CAPTURE_RESP;
      end else if (state == TX_LOAD_EVENT) begin
        state <= TX_CAPTURE_EVENT;
      end
    end
  end
endmodule
