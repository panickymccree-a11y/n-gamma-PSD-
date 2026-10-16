`timescale 1ns/1ps
module mca_eth_udp #(
    parameter [47:0] BOARD_MAC = 48'h00_11_22_33_44_55,
    parameter [31:0] BOARD_IP  = {8'd192,8'd168,8'd1,8'd10},
    parameter [47:0] DES_MAC   = 48'hff_ff_ff_ff_ff_ff,
    parameter [31:0] DES_IP    = {8'd192,8'd168,8'd1,8'd102},
    parameter [15:0] DATA_PORT = 16'd9000,
    parameter [15:0] CONTROL_PORT = 16'd9001,
    parameter integer IDELAY_VALUE = 0
) (
    input  wire       idelay_clk,
    input  wire       idelay_rst_n,
    input  wire       phy_rst_n,
    input  wire       rst_n,
    input  wire       eth_rxc,
    input  wire       eth_rx_ctl,
    input  wire [3:0] eth_rxd,
    output wire       eth_txc,
    output wire       eth_tx_ctl,
    output wire [3:0] eth_txd,
    output wire       eth_rst_n,
    output wire       idelay_ready,
    output wire       gmii_clk,
    input  wire       tx_start_en,
    input  wire [15:0]tx_byte_num,
    input  wire [7:0] tx_data,
    output wire       tx_data_req,
    output wire       tx_done,
    output wire       rec_en,
    output wire [7:0] rec_data,
    output wire       rec_pkt_done,
    output wire [15:0]rec_byte_num
);
    wire gmii_rx_dv;
    wire [7:0] gmii_rxd;
    wire gmii_tx_en;
    wire [7:0] gmii_txd;

    wire arp_gmii_tx_en, arp_rx_done, arp_rx_type, arp_tx_en;
    wire arp_tx_type, arp_tx_done;
    wire [7:0] arp_gmii_txd;
    wire [47:0] src_mac;
    wire [31:0] src_ip;
    wire [47:0] learned_mac = (src_mac != 0) ? src_mac : DES_MAC;
    wire [31:0] learned_ip  = (src_ip  != 0) ? src_ip  : DES_IP;

    wire udp_gmii_tx_en, udp_tx_req;
    wire [7:0] udp_gmii_txd, udp_tx_data;
    reg udp_tx_req_d;

    assign eth_rst_n = phy_rst_n;
    assign tx_data_req = udp_tx_req_d;

    always @(posedge gmii_clk or negedge rst_n) begin
        if (!rst_n)
            udp_tx_req_d <= 1'b0;
        else
            udp_tx_req_d <= udp_tx_req;
    end

    gmii_to_rgmii #(
        .IDELAY_VALUE (IDELAY_VALUE)
    ) u_rgmii (
        .idelay_clk   (idelay_clk),
        .idelay_rst_n (idelay_rst_n),
        .gmii_rx_clk  (gmii_clk),
        .gmii_rx_dv   (gmii_rx_dv),
        .gmii_rxd     (gmii_rxd),
        .gmii_tx_clk  (),
        .gmii_tx_en   (gmii_tx_en),
        .gmii_txd     (gmii_txd),
        .rgmii_rxc    (eth_rxc),
        .rgmii_rx_ctl (eth_rx_ctl),
        .rgmii_rxd    (eth_rxd),
        .rgmii_txc    (eth_txc),
        .rgmii_tx_ctl (eth_tx_ctl),
        .rgmii_txd    (eth_txd),
        .idelay_ready (idelay_ready)
    );

    arp #(
        .BOARD_MAC (BOARD_MAC),
        .BOARD_IP  (BOARD_IP),
        .DES_MAC   (DES_MAC),
        .DES_IP    (DES_IP)
    ) u_arp (
        .rst_n         (rst_n),
        .gmii_rx_clk   (gmii_clk),
        .gmii_rx_dv    (gmii_rx_dv),
        .gmii_rxd      (gmii_rxd),
        .gmii_tx_clk   (gmii_clk),
        .gmii_tx_en    (arp_gmii_tx_en),
        .gmii_txd      (arp_gmii_txd),
        .arp_rx_done   (arp_rx_done),
        .arp_rx_type   (arp_rx_type),
        .src_mac       (src_mac),
        .src_ip        (src_ip),
        .arp_tx_en     (arp_tx_en),
        .arp_tx_type   (arp_tx_type),
        .des_mac       (learned_mac),
        .des_ip        (learned_ip),
        .tx_done       (arp_tx_done)
    );

    udp #(
        .BOARD_MAC (BOARD_MAC),
        .BOARD_IP  (BOARD_IP),
        .DES_MAC   (DES_MAC),
        .DES_IP    (DES_IP),
        .SRC_PORT  (DATA_PORT),
        .DES_PORT  (DATA_PORT),
        .CONTROL_PORT (CONTROL_PORT)
    ) u_udp (
        .rst_n         (rst_n),
        .gmii_rx_clk   (gmii_clk),
        .gmii_rx_dv    (gmii_rx_dv),
        .gmii_rxd      (gmii_rxd),
        .gmii_tx_clk   (gmii_clk),
        .gmii_tx_en    (udp_gmii_tx_en),
        .gmii_txd      (udp_gmii_txd),
        .rec_pkt_done  (rec_pkt_done),
        .rec_en        (rec_en),
        .rec_data      (rec_data),
        .rec_byte_num  (rec_byte_num),
        .tx_start_en   (tx_start_en),
        .tx_data       (udp_tx_data),
        .tx_byte_num   (tx_byte_num),
        .des_mac       (learned_mac),
        .des_ip        (learned_ip),
        .tx_done       (tx_done),
        .tx_req        (udp_tx_req)
    );

    mca_eth_ctrl u_ctrl (
        .clk              (gmii_clk),
        .rst_n            (rst_n),
        .arp_rx_done      (arp_rx_done),
        .arp_rx_type      (arp_rx_type),
        .arp_tx_en        (arp_tx_en),
        .arp_tx_type      (arp_tx_type),
        .arp_tx_done      (arp_tx_done),
        .arp_gmii_tx_en   (arp_gmii_tx_en),
        .arp_gmii_txd     (arp_gmii_txd),
        .udp_tx_start_en  (tx_start_en),
        .udp_tx_done      (tx_done),
        .udp_gmii_tx_en   (udp_gmii_tx_en),
        .udp_gmii_txd     (udp_gmii_txd),
        .udp_tx_req       (udp_tx_req),
        .udp_tx_data      (udp_tx_data),
        .tx_data          (tx_data),
        .gmii_tx_en       (gmii_tx_en),
        .gmii_txd         (gmii_txd)
    );
endmodule
