`timescale 1ns/1ps

// Behavioural replacement for the generated clock IP.  The previous VIO
// debug-control stub was removed with the VIO instance; range and front-end
// configuration now travel through the MCF2 endpoint.
module mca_clk_wiz (
  input  logic clk_in1,
  output logic clk_out1,
  output logic clk_out2,
  output logic locked
);
  assign clk_out1 = clk_in1;
  initial begin
    clk_out2 = 1'b0;
    locked = 1'b0;
    #40 locked = 1'b1;
  end
  always #2.5 clk_out2 = ~clk_out2;
endmodule

// Behavioural simulation model for the generated Divider Generator IP.  The
// production project uses HDL/ip/mca_peak_divider/mca_peak_divider.xci; this
// model preserves its 30-cycle valid/TUSER latency and quotient/remainder
// packing for the source-only XSim regressions.
module mca_peak_divider (
  input  logic        aclk,
  input  logic        s_axis_divisor_tvalid,
  input  logic [15:0] s_axis_divisor_tdata,
  input  logic        s_axis_dividend_tvalid,
  input  logic [127:0] s_axis_dividend_tuser,
  input  logic [31:0] s_axis_dividend_tdata,
  output logic        m_axis_dout_tvalid,
  output logic [127:0] m_axis_dout_tuser,
  output logic [47:0] m_axis_dout_tdata
);
  logic valid_pipe [0:29];
  logic [127:0] user_pipe [0:29];
  logic [47:0] data_pipe [0:29];
  integer k;
  integer j;
  initial begin
    for (j = 0; j < 30; j = j + 1) begin
      valid_pipe[j] = 1'b0;
      user_pipe[j] = '0;
      data_pipe[j] = '0;
    end
  end
  always_ff @(posedge aclk) begin
    valid_pipe[0] <= s_axis_dividend_tvalid && s_axis_divisor_tvalid;
    user_pipe[0] <= s_axis_dividend_tuser;
    if (s_axis_dividend_tdata[27:0] == 0 || s_axis_divisor_tdata == 0)
      data_pipe[0] <= '0;
    else
      data_pipe[0] <= (s_axis_dividend_tdata[27:0] /
                       s_axis_divisor_tdata) * 48'h1_0000 +
                      (s_axis_dividend_tdata[27:0] % s_axis_divisor_tdata);
    for (k = 1; k < 30; k = k + 1) begin
      valid_pipe[k] <= valid_pipe[k-1];
      user_pipe[k] <= user_pipe[k-1];
      data_pipe[k] <= data_pipe[k-1];
    end
  end
  assign m_axis_dout_tvalid = valid_pipe[29];
  assign m_axis_dout_tuser = user_pipe[29];
  assign m_axis_dout_tdata = data_pipe[29];
endmodule

// Simulation-only stand-ins for the small Xilinx IO primitives used by the
// RGMII wrapper.  They preserve the logical data/clock behavior needed for
// top-level compile/elaboration; pin-delay and DDR placement remain board/IP
// implementation concerns.
module BUFG(input wire I, output wire O);
  assign O = I;
endmodule

module BUFIO(input wire I, output wire O);
  assign O = I;
endmodule

module IDELAYCTRL(
  input wire REFCLK,
  input wire RST,
  output wire RDY
);
  assign RDY = !RST;
endmodule

module IDELAYE2 #(
  parameter IDELAY_TYPE = "FIXED",
  parameter integer IDELAY_VALUE = 0,
  parameter real REFCLK_FREQUENCY = 200.0
) (
  output wire [4:0] CNTVALUEOUT,
  output wire DATAOUT,
  input wire C,
  input wire CE,
  input wire CINVCTRL,
  input wire [4:0] CNTVALUEIN,
  input wire DATAIN,
  input wire IDATAIN,
  input wire INC,
  input wire LD,
  input wire LDPIPEEN,
  input wire REGRST
);
  assign CNTVALUEOUT = CNTVALUEIN;
  assign DATAOUT = IDATAIN;
endmodule

module IDDR #(
  parameter DDR_CLK_EDGE = "SAME_EDGE_PIPELINED",
  parameter INIT_Q1 = 1'b0,
  parameter INIT_Q2 = 1'b0,
  parameter SRTYPE = "SYNC"
) (
  output reg Q1,
  output reg Q2,
  input wire C,
  input wire CE,
  input wire D,
  input wire R,
  input wire S
);
  initial begin Q1 = INIT_Q1; Q2 = INIT_Q2; end
  always @(posedge C) if (CE) Q1 <= R ? 1'b0 : (S ? 1'b1 : D);
  always @(negedge C) if (CE) Q2 <= R ? 1'b0 : (S ? 1'b1 : D);
endmodule

module ODDR #(
  parameter DDR_CLK_EDGE = "SAME_EDGE",
  parameter INIT = 1'b0,
  parameter SRTYPE = "SYNC"
) (
  output reg Q,
  input wire C,
  input wire CE,
  input wire D1,
  input wire D2,
  input wire R,
  input wire S
);
  initial Q = INIT;
  always @(posedge C) if (CE) Q <= R ? 1'b0 : (S ? 1'b1 : D1);
  always @(negedge C) if (CE) Q <= R ? 1'b0 : (S ? 1'b1 : D2);
endmodule
