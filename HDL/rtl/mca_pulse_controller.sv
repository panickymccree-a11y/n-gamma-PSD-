`timescale 1ns/1ps

module mca_pulse_controller #(
  parameter int PULSE_CYCLES = 2_500_000
) (
  input  logic clk,
  input  logic rst_n,
  input  logic system_ready,
  input  logic run_request,
  input  logic manual_q1,
  input  logic manual_q2,
  input  logic dco_pause_ack,
  output logic q1,
  output logic q2,
  output logic busy,
  output logic pause_request,
  output logic init_done,
  output logic actual_q1,
  output logic actual_q2
);
  localparam int CW = (PULSE_CYCLES < 2) ? 1 : $clog2(PULSE_CYCLES + 1);
  logic [CW-1:0] count;
  logic auto_started;
  logic active_q1, active_q2;
  logic run_latched;
  logic pending_q1, pending_q2;
  logic preparing;

  assign busy = active_q1 | active_q2 | preparing;
  assign q1 = active_q1;
  assign q2 = active_q2;
  assign actual_q1 = q1;
  assign actual_q2 = q2;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count       <= '0;
      active_q1   <= 1'b0;
      active_q2   <= 1'b0;
      auto_started <= 1'b0;
      run_latched <= 1'b0;
      pending_q1  <= 1'b0;
      pending_q2  <= 1'b0;
      preparing   <= 1'b0;
      pause_request <= 1'b0;
      init_done   <= 1'b0;
    end else begin
      if (run_request)
        run_latched <= 1'b1;
      else if (!system_ready)
        run_latched <= 1'b0;

      if (active_q1 || active_q2) begin
        if (count == PULSE_CYCLES-1) begin
          count     <= '0;
          active_q1 <= 1'b0;
          active_q2 <= 1'b0;
          pause_request <= 1'b0;
          if (!auto_started) begin
            auto_started <= 1'b1;
            init_done    <= 1'b1;
          end
        end else begin
          count <= count + 1'b1;
        end
      end else if (preparing) begin
        if (dco_pause_ack) begin
          preparing <= 1'b0;
          active_q1 <= pending_q1;
          active_q2 <= pending_q2;
          pending_q1 <= 1'b0;
          pending_q2 <= 1'b0;
          count <= '0;
        end
      end else begin
        count <= '0;
        if (system_ready && !auto_started) begin
          preparing     <= 1'b1;
          pause_request <= 1'b1;
          pending_q1    <= 1'b0;
          pending_q2    <= 1'b1;
        end else if (system_ready && (manual_q1 ^ manual_q2)) begin
          preparing <= 1'b1;
          pause_request <= 1'b1;
          pending_q1 <= manual_q1;
          pending_q2 <= manual_q2;
        end
      end
    end
  end
endmodule
