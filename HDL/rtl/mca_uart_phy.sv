`timescale 1ns/1ps

// Fractional-divider 8N1 UART.  The production link runs at 500 kbaud to keep
// margin on the board's CH340C sustained receive path; the accumulator supports
// non-integer clock/baud ratios without systematic baud error.
module mca_uart_phy #(
  parameter int CLOCK_HZ = 125_000_000,
  parameter int BAUD_HZ  = 500_000
) (
  input  logic       clk,
  input  logic       rst_n,
  input  logic       rxd,
  output logic       txd,
  output logic [7:0] rx_data,
  output logic       rx_valid,
  output logic       rx_frame_error,
  input  logic [7:0] tx_data,
  input  logic       tx_valid,
  output logic       tx_ready
);
  localparam int PHASE_W = $clog2(CLOCK_HZ + BAUD_HZ + 1);
  (* ASYNC_REG = "TRUE" *) logic rxd_ff1, rxd_ff2;
  logic [PHASE_W-1:0] rx_phase, tx_phase;
  logic [3:0] rx_bit, tx_bit;
  logic [7:0] rx_shift, tx_shift;
  logic rx_busy, tx_busy;
  logic rx_tick, tx_tick;

  always_comb begin
    rx_tick = rx_busy && (rx_phase >= CLOCK_HZ-BAUD_HZ);
    tx_tick = tx_busy && (tx_phase >= CLOCK_HZ-BAUD_HZ);
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rxd_ff1       <= 1'b1;
      rxd_ff2       <= 1'b1;
      rx_phase      <= '0;
      rx_bit        <= '0;
      rx_shift      <= '0;
      rx_busy       <= 1'b0;
      rx_data       <= '0;
      rx_valid      <= 1'b0;
      rx_frame_error<= 1'b0;
    end else begin
      rxd_ff1        <= rxd;
      rxd_ff2        <= rxd_ff1;
      rx_valid       <= 1'b0;
      rx_frame_error <= 1'b0;
      if (!rx_busy) begin
        if (!rxd_ff2) begin
          rx_busy  <= 1'b1;
          rx_bit   <= 4'd0;
          // First tick occurs in the middle of the start bit.
          rx_phase <= CLOCK_HZ/2;
        end
      end else if (rx_tick) begin
        rx_phase <= rx_phase + BAUD_HZ - CLOCK_HZ;
        if (rx_bit == 0) begin
          if (rxd_ff2) begin
            rx_busy        <= 1'b0;
            rx_frame_error <= 1'b1;
          end else begin
            rx_bit <= 4'd1;
          end
        end else if (rx_bit <= 8) begin
          rx_shift[rx_bit-1] <= rxd_ff2;
          rx_bit <= rx_bit + 1'b1;
        end else begin
          rx_busy <= 1'b0;
          if (rxd_ff2) begin
            rx_data  <= rx_shift;
            rx_valid <= 1'b1;
          end else begin
            rx_frame_error <= 1'b1;
          end
        end
      end else begin
        rx_phase <= rx_phase + BAUD_HZ;
      end
    end
  end

  assign tx_ready = !tx_busy;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      txd      <= 1'b1;
      tx_phase <= '0;
      tx_bit   <= '0;
      tx_shift <= '0;
      tx_busy  <= 1'b0;
    end else begin
      if (!tx_busy) begin
        txd <= 1'b1;
        if (tx_valid) begin
          tx_busy  <= 1'b1;
          tx_shift <= tx_data;
          tx_bit   <= 4'd0;
          tx_phase <= '0;
          txd      <= 1'b0;
        end
      end else if (tx_tick) begin
        tx_phase <= tx_phase + BAUD_HZ - CLOCK_HZ;
        if (tx_bit < 8) begin
          txd    <= tx_shift[tx_bit];
          tx_bit <= tx_bit + 1'b1;
        end else if (tx_bit == 8) begin
          txd    <= 1'b1;
          tx_bit <= 4'd9;
        end else begin
          txd     <= 1'b1;
          tx_busy <= 1'b0;
        end
      end else begin
        tx_phase <= tx_phase + BAUD_HZ;
      end
    end
  end
endmodule
