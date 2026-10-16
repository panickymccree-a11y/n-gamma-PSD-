`timescale 1ns/1ps

module mca_rgmii_rx (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [3:0] rgmii_rxd,
  input  logic       rgmii_rx_ctl,
  output logic       byte_valid,
  output logic [7:0] byte_data,
  output logic       frame_end
);
  logic [3:0] rise_data, fall_data;
  logic rise_ctl, fall_ctl;
  logic prev_valid;

`ifdef SYNTHESIS
  genvar i;
  generate
    for (i = 0; i < 4; i = i + 1) begin : gen_iddr
      IDDR #(.DDR_CLK_EDGE("SAME_EDGE_PIPELINED")) u_iddr (
        .Q1(rise_data[i]), .Q2(fall_data[i]), .C(clk), .CE(1'b1),
        .D(rgmii_rxd[i]), .R(~rst_n), .S(1'b0)
      );
    end
  endgenerate
  IDDR #(.DDR_CLK_EDGE("SAME_EDGE_PIPELINED")) u_ctl_iddr (
    .Q1(rise_ctl), .Q2(fall_ctl), .C(clk), .CE(1'b1),
    .D(rgmii_rx_ctl), .R(~rst_n), .S(1'b0)
  );
`else
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rise_data <= '0;
      rise_ctl  <= 1'b0;
    end else begin
      rise_data <= rgmii_rxd;
      rise_ctl  <= rgmii_rx_ctl;
    end
  end
  always @(negedge clk or negedge rst_n) begin
    if (!rst_n) begin
      fall_data <= '0;
      fall_ctl  <= 1'b0;
    end else begin
      fall_data <= rgmii_rxd;
      fall_ctl  <= rgmii_rx_ctl;
    end
  end
`endif

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      prev_valid <= 1'b0;
      byte_valid <= 1'b0;
      byte_data  <= '0;
      frame_end  <= 1'b0;
    end else begin
      byte_valid <= rise_ctl;
      byte_data  <= {fall_data, rise_data};
      frame_end  <= prev_valid && !rise_ctl;
      prev_valid <= rise_ctl;
    end
  end
endmodule

module mca_rgmii_tx (
  input  logic       clk,
  input  logic       rst_n,
  input  logic       tx_valid,
  input  logic [7:0] tx_data,
  output logic [3:0] rgmii_txd,
  output logic       rgmii_tx_ctl,
  output logic       rgmii_txc
);
  assign rgmii_txc = clk;

`ifdef SYNTHESIS
  genvar j;
  generate
    for (j = 0; j < 4; j = j + 1) begin : gen_oddr
      ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) u_oddr (
        .Q(rgmii_txd[j]), .C(clk), .CE(1'b1), .D1(tx_data[j]),
        .D2(tx_data[j+4]), .R(~rst_n), .S(1'b0)
      );
    end
  endgenerate
  ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) u_ctl_oddr (
    .Q(rgmii_tx_ctl), .C(clk), .CE(1'b1), .D1(tx_valid),
    .D2(tx_valid), .R(~rst_n), .S(1'b0)
  );
`else
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rgmii_txd <= '0;
      rgmii_tx_ctl <= 1'b0;
    end else begin
      rgmii_txd <= tx_data[3:0];
      rgmii_tx_ctl <= tx_valid;
    end
  end
  always @(negedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rgmii_txd <= '0;
      rgmii_tx_ctl <= 1'b0;
    end else begin
      rgmii_txd <= tx_data[7:4];
      rgmii_tx_ctl <= tx_valid;
    end
  end
`endif
endmodule
