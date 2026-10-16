`timescale 1ns/1ps

// Adapts the byte-stream interface of the proven Ethernet/ARP/UDP stack to
// the command, response and event asynchronous FIFOs used by this design.
module mca_udp_bridge #(
  parameter int EVENT_TIMEOUT_CYCLES = 12_500
) (
  input  logic         clk,
  input  logic         rst_n,
  input  logic         rec_en,
  input  logic [7:0]   rec_data,
  input  logic         rec_pkt_done,
  input  logic [15:0]  rec_byte_num,
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
  output logic         tx_start_en,
  output logic [15:0]  tx_byte_num,
  output logic [7:0]   tx_data,
  input  logic         tx_data_req,
  input  logic         tx_done
);
  localparam int TIMER_WIDTH = (EVENT_TIMEOUT_CYCLES < 2) ? 1 :
                               $clog2(EVENT_TIMEOUT_CYCLES + 1);
  typedef enum logic [2:0] {
    TX_IDLE, TX_RESP_WAIT, TX_EVENT_WAIT, TX_EVENT_NEXT, TX_STREAM
  } tx_state_t;

  logic [7:0] rx_index;
  logic       rx_finish_pending;
  logic [15:0] rx_finished_len;
  logic [7:0] tx_mem [0:95];
  logic [6:0] tx_index;
  logic [3:0] event_batch_count;
  logic [TIMER_WIDTH-1:0] event_timer;
  tx_state_t tx_state;
  integer i;

  assign tx_data = tx_mem[tx_index];

  // udp_rx raises rec_pkt_done with the last rec_en byte.  Emit cmd_wr on the
  // following cycle so the final byte is already registered in cmd_payload.
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      rx_index          <= '0;
      rx_finish_pending <= 1'b0;
      rx_finished_len   <= '0;
      cmd_wr            <= 1'b0;
      cmd_payload       <= '0;
      cmd_len           <= '0;
    end else begin
      cmd_wr <= 1'b0;
      if (rec_en) begin
        if (rx_index < 8'd64)
          cmd_payload[rx_index*8 +: 8] <= rec_data;
        if (rx_index != 8'hff)
          rx_index <= rx_index + 1'b1;
      end
      if (rec_pkt_done) begin
        rx_finish_pending <= 1'b1;
        rx_finished_len   <= rec_byte_num;
      end
      if (rx_finish_pending) begin
        rx_finish_pending <= 1'b0;
        if ((rx_finished_len <= 16'd64) && !cmd_full) begin
          cmd_len <= rx_finished_len[7:0];
          cmd_wr  <= 1'b1;
        end
        rx_index <= '0;
      end
    end
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      resp_rd_en       <= 1'b0;
      event_rd_en      <= 1'b0;
      tx_start_en      <= 1'b0;
      tx_byte_num      <= '0;
      tx_index         <= '0;
      event_batch_count<= '0;
      event_timer      <= '0;
      tx_state         <= TX_IDLE;
      for (i = 0; i < 96; i = i + 1)
        tx_mem[i] <= 8'd0;
    end else begin
      resp_rd_en  <= 1'b0;
      event_rd_en <= 1'b0;
      tx_start_en <= 1'b0;

      case (tx_state)
        TX_IDLE: begin
          tx_index <= '0;
          if (event_empty)
            event_timer <= '0;
          else if (event_timer < EVENT_TIMEOUT_CYCLES)
            event_timer <= event_timer + 1'b1;

          if (!resp_empty) begin
            resp_rd_en <= 1'b1;
            tx_state   <= TX_RESP_WAIT;
          end else if (!event_empty &&
                       ((event_level >= 9'd8) ||
                        (event_timer >= EVENT_TIMEOUT_CYCLES-1))) begin
            event_batch_count <= '0;
            event_rd_en       <= 1'b1;
            tx_state          <= TX_EVENT_WAIT;
          end
        end

        TX_RESP_WAIT: begin
          tx_byte_num <= (resp_dout[519:512] > 8'd64) ? 16'd64 :
                         {8'd0, resp_dout[519:512]};
          for (i = 0; i < 64; i = i + 1)
            tx_mem[i] <= resp_dout[i*8 +: 8];
          tx_start_en <= 1'b1;
          tx_state    <= TX_STREAM;
        end

        TX_EVENT_WAIT: begin
          tx_mem[event_batch_count*12 + 0]  <= event_dout[95:88];
          tx_mem[event_batch_count*12 + 1]  <= event_dout[87:80];
          tx_mem[event_batch_count*12 + 2]  <= event_dout[79:72];
          tx_mem[event_batch_count*12 + 3]  <= event_dout[71:64];
          tx_mem[event_batch_count*12 + 4]  <= event_dout[63:56];
          tx_mem[event_batch_count*12 + 5]  <= event_dout[55:48];
          tx_mem[event_batch_count*12 + 6]  <= event_dout[47:40];
          tx_mem[event_batch_count*12 + 7]  <= event_dout[39:32];
          tx_mem[event_batch_count*12 + 8]  <= event_dout[31:24];
          tx_mem[event_batch_count*12 + 9]  <= event_dout[23:16];
          tx_mem[event_batch_count*12 + 10] <= event_dout[15:8];
          tx_mem[event_batch_count*12 + 11] <= event_dout[7:0];
          tx_state <= TX_EVENT_NEXT;
        end

        TX_EVENT_NEXT: begin
          if ((event_batch_count == 4'd7) || event_empty) begin
            tx_byte_num <= 16'd12 * (event_batch_count + 1'b1);
            tx_start_en <= 1'b1;
            event_timer <= '0;
            tx_state    <= TX_STREAM;
          end else begin
            event_batch_count <= event_batch_count + 1'b1;
            event_rd_en       <= 1'b1;
            tx_state          <= TX_EVENT_WAIT;
          end
        end

        TX_STREAM: begin
          if (tx_data_req && (tx_index + 1'b1 < tx_byte_num))
            tx_index <= tx_index + 1'b1;
          if (tx_done) begin
            tx_index <= '0;
            tx_state <= TX_IDLE;
          end
        end

        default: tx_state <= TX_IDLE;
      endcase
    end
  end
endmodule
