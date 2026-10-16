`timescale 1ns/1ps

module mca_eth_ctrl (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       arp_rx_done,
    input  wire       arp_rx_type,
    output reg        arp_tx_en,
    output wire       arp_tx_type,
    input  wire       arp_tx_done,
    input  wire       arp_gmii_tx_en,
    input  wire [7:0] arp_gmii_txd,
    input  wire       udp_tx_start_en,
    input  wire       udp_tx_done,
    input  wire       udp_gmii_tx_en,
    input  wire [7:0] udp_gmii_txd,
    input  wire       udp_tx_req,
    output wire [7:0] udp_tx_data,
    input  wire [7:0] tx_data,
    output reg        gmii_tx_en,
    output reg  [7:0] gmii_txd
);
    reg udp_busy;
    reg arp_pending;
    reg select_udp;
    reg udp_tx_req_d;

    assign arp_tx_type = 1'b1;
    assign udp_tx_data = udp_tx_req_d ? tx_data : 8'd0;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            udp_tx_req_d <= 1'b0;
        end else begin
            udp_tx_req_d <= udp_tx_req;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            udp_busy   <= 1'b0;
            arp_pending <= 1'b0;
            select_udp <= 1'b0;
            arp_tx_en  <= 1'b0;
        end else begin
            arp_tx_en <= 1'b0;

            if (arp_rx_done && !arp_rx_type) begin
                arp_pending <= 1'b1;
            end

            if (udp_tx_start_en && !arp_pending) begin
                udp_busy   <= 1'b1;
                select_udp <= 1'b1;
            end

            if (udp_tx_done) begin
                udp_busy   <= 1'b0;
                select_udp <= 1'b0;
            end

            if (arp_pending && !udp_busy) begin
                arp_tx_en  <= 1'b1;
                arp_pending <= 1'b0;
                select_udp <= 1'b0;
            end

            if (arp_tx_done) begin
                select_udp <= 1'b0;
            end
        end
    end

    always @(*) begin
        if (select_udp) begin
            gmii_tx_en = udp_gmii_tx_en;
            gmii_txd   = udp_gmii_txd;
        end else begin
            gmii_tx_en = arp_gmii_tx_en;
            gmii_txd   = arp_gmii_txd;
        end
    end
endmodule

