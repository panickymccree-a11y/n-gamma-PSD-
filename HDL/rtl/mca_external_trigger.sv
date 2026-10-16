`timescale 1ns/1ps

// Synchronous external-trigger acquisition gate.
//
// EXTRI is an asynchronous package input.  A two-flop synchronizer is used;
// therefore the external source must hold the signal high for at least one
// 125 MHz sampling interval (the documented minimum is 16 ns).  A detected
// rising edge reloads a retriggerable gate for duration_us microseconds.
// The gate is deliberately independent of the event discriminator: it only
// qualifies new-event acceptance and does not abort an event already in flight.
module mca_external_trigger (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        ex_tri_async,
  input  logic        enable,
  input  logic        abort,
  input  logic        clear_count,
  input  logic [19:0] duration_us,
  output logic        window_active,
  output logic [31:0] trigger_count
);
  localparam logic [6:0] TICKS_PER_US = 7'd124;

  (* ASYNC_REG = "TRUE" *) logic ex_tri_meta_q;
  (* ASYNC_REG = "TRUE" *) logic ex_tri_sync_q;
  logic ex_tri_sync_d_q;
  wire ex_tri_rise = ex_tri_sync_q && !ex_tri_sync_d_q;

  logic [6:0]  tick_q;
  logic [19:0] remaining_us_q;
  wire [19:0] duration_eff = (duration_us == 20'd0)
                            ? 20'd1 : duration_us;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      ex_tri_meta_q   <= 1'b0;
      ex_tri_sync_q   <= 1'b0;
      ex_tri_sync_d_q <= 1'b0;
    end else begin
      ex_tri_meta_q   <= ex_tri_async;
      ex_tri_sync_q   <= ex_tri_meta_q;
      ex_tri_sync_d_q <= ex_tri_sync_q;
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      tick_q        <= 7'd0;
      remaining_us_q<= 20'd0;
      window_active <= 1'b0;
      trigger_count <= 32'd0;
    end else begin
      if (clear_count)
        trigger_count <= 32'd0;

      if (!enable || abort) begin
        tick_q         <= 7'd0;
        remaining_us_q <= 20'd0;
        window_active  <= 1'b0;
      end else if (ex_tri_rise) begin
        // Retrigger has priority over the expiry tick.
        tick_q         <= 7'd0;
        remaining_us_q <= duration_eff;
        window_active  <= 1'b1;
        if (!clear_count)
          trigger_count <= trigger_count + 32'd1;
      end else if (window_active) begin
        if (tick_q == TICKS_PER_US) begin
          tick_q <= 7'd0;
          if (remaining_us_q <= 20'd1) begin
            remaining_us_q <= 20'd0;
            window_active  <= 1'b0;
          end else begin
            remaining_us_q <= remaining_us_q - 20'd1;
          end
        end else begin
          tick_q <= tick_q + 7'd1;
        end
      end
    end
  end
endmodule
