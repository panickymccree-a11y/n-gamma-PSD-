`timescale 1ns/1ps

module mca_range_handshake (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [1:0] request_value,
  input  logic       request_strobe,
  output logic [1:0] request_value_latched,
  output logic [1:0] applied_value,
  output logic       busy,
  output logic       req_toggle,
  input  logic       ack_toggle_dco,
  input  logic       applied_toggle_dco,
  input  logic [1:0] applied_value_dco
);
  logic ack_ff1, ack_ff2;
  logic applied_ff1, applied_ff2;
  logic [1:0] applied_value_ff1, applied_value_ff2;
  logic applied_seen;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      request_value_latched <= 2'b00;
      req_toggle      <= 1'b0;
      ack_ff1         <= 1'b0;
      ack_ff2         <= 1'b0;
      applied_ff1     <= 1'b0;
      applied_ff2     <= 1'b0;
      applied_value_ff1 <= 2'b00;
      applied_value_ff2 <= 2'b00;
      applied_seen     <= 1'b0;
      busy            <= 1'b0;
      applied_value   <= 2'b00;
    end else begin
      ack_ff1     <= ack_toggle_dco;
      ack_ff2     <= ack_ff1;
      applied_ff1 <= applied_toggle_dco;
      applied_ff2 <= applied_ff1;
      applied_value_ff1 <= applied_value_dco;
      applied_value_ff2 <= applied_value_ff1;
      if (request_strobe && !busy) begin
        request_value_latched <= request_value;
        req_toggle      <= ~req_toggle;
        busy            <= 1'b1;
      end
      if (busy && (ack_ff2 == req_toggle)) begin
        busy <= 1'b0;
      end
      if (applied_ff2 != applied_seen) begin
        applied_seen  <= applied_ff2;
        applied_value <= applied_value_ff2;
      end
    end
  end
endmodule

// DCO-side endpoint. The request data is held stable by the source until the
// acknowledgement returns, so only the toggle is used as the CDC event.
module mca_range_handshake_dco (
  input  logic       dco_clk,
  input  logic       rst_n,
  input  logic [1:0] request_value_clk,
  input  logic       req_toggle_clk,
  output logic       ack_toggle_dco,
  output logic       applied_toggle_dco,
  output logic [1:0] applied_value_dco,
  output logic       pause_events
);
  logic req_ff1, req_ff2, req_seen;
  logic [1:0] value_ff1, value_ff2;

  always_ff @(posedge dco_clk or negedge rst_n) begin
    if (!rst_n) begin
      req_ff1          <= 1'b0;
      req_ff2          <= 1'b0;
      req_seen         <= 1'b0;
      value_ff1        <= 2'b00;
      value_ff2        <= 2'b00;
      ack_toggle_dco   <= 1'b0;
      applied_toggle_dco <= 1'b0;
      applied_value_dco <= 2'b00;
      pause_events     <= 1'b0;
    end else begin
      req_ff1   <= req_toggle_clk;
      req_ff2   <= req_ff1;
      value_ff1 <= request_value_clk;
      value_ff2 <= value_ff1;
      if (req_ff2 != req_seen) begin
        req_seen     <= req_ff2;
        pause_events <= 1'b1;
      end else if (pause_events) begin
        applied_value_dco   <= value_ff2;
        applied_toggle_dco  <= ~applied_toggle_dco;
        ack_toggle_dco      <= req_seen;
        pause_events        <= 1'b0;
      end
    end
  end
endmodule
