`timescale 1ns/1ps

module tb_mca_uart_protocol;
  localparam int CLOCK_HZ = 125_000_000;
  localparam int BAUD_HZ = 500_000;
  localparam int BIT_CYCLES = CLOCK_HZ / BAUD_HZ;
  logic clk = 0, rst_n = 0, rxd = 1;
  always #5 clk = ~clk;
  logic txd, cmd_valid, cmd_ready = 0, resp_ready, chunk_ack;
  logic chunk_toggle = 0;
  logic [511:0] cmd_payload;
  logic [7:0] cmd_len, rd_index;
  logic [31:0] crc_errors, frame_errors;
  logic [7:0] bytes [0:1100];

  mca_uart_protocol #(
    .CLOCK_HZ(CLOCK_HZ), .BAUD_HZ(BAUD_HZ),
    .BIN_COUNT(1024), .CHUNK_BINS(256)
  ) dut (
    .clk(clk), .rst_n(rst_n), .uart_rxd(rxd), .uart_txd(txd),
    .cmd_valid(cmd_valid), .cmd_payload(cmd_payload), .cmd_len(cmd_len),
    .cmd_ready(cmd_ready), .resp_valid(1'b0), .resp_payload('0),
    .resp_len('0), .resp_ready(resp_ready), .chunk_toggle(chunk_toggle),
    .chunk_ack_toggle(chunk_ack), .chunk_session('0), .chunk_epoch('0),
    .chunk_config_version('0), .chunk_kind(1'b0), .chunk_flags('0),
    .chunk_shift('0), .chunk_total_bins(16'd1024), .chunk_start('0),
    .chunk_count(16'd256), .chunk_index('0), .chunk_total(16'd4),
    .chunk_ticks('0), .chunk_events('0), .chunk_dropped('0),
    .status_running(1'b0), .status_config_version('0),
    .status_event_count('0), .status_dropped_count('0),
    .chunk_rd_index(rd_index), .chunk_rd_data('0),
    .crc_error_count(crc_errors), .frame_error_count(frame_errors)
  );

  function automatic [31:0] crc_byte(input [31:0] crc, input [7:0] data);
    reg [31:0] c; integer b;
    begin
      c = crc ^ data;
      for (b=0; b<8; b=b+1) c = c[0] ? ((c>>1)^32'hedb88320) : (c>>1);
      crc_byte = c;
    end
  endfunction

  task automatic send_byte(input [7:0] value, input bit good_stop);
    integer bit_index;
    begin
      rxd = 0; repeat (BIT_CYCLES) @(posedge clk);
      for (bit_index=0; bit_index<8; bit_index=bit_index+1) begin
        rxd = value[bit_index]; repeat (BIT_CYCLES) @(posedge clk);
      end
      rxd = good_stop; repeat (BIT_CYCLES) @(posedge clk); rxd = 1;
    end
  endtask

  task automatic send_array(input integer count);
    integer k;
    begin for (k=0; k<count; k=k+1) send_byte(bytes[k], 1'b1); end
  endtask

  task automatic receive_byte(output logic [7:0] value);
    integer bit_index;
    begin
      @(negedge txd);
      // Sample data bit 0 one and a half bit periods after the start edge.
      repeat (BIT_CYCLES + BIT_CYCLES/2) @(posedge clk);
      for (bit_index=0; bit_index<8; bit_index=bit_index+1) begin
        value[bit_index] = txd;
        repeat (BIT_CYCLES) @(posedge clk);
      end
      if (!txd) $fatal(1, "transmit stop bit missing");
    end
  endtask

  task automatic make_frame(input [7:0] msg_type, input integer payload_len,
                            input [31:0] seq_value);
    integer k; reg [31:0] crc;
    begin
      bytes[0]=8'h4d; bytes[1]=8'h43; bytes[2]=8'h53; bytes[3]=8'h32;
      bytes[4]=2; bytes[5]=msg_type; bytes[6]=0; bytes[7]=16;
      bytes[8]=seq_value[31:24]; bytes[9]=seq_value[23:16];
      bytes[10]=seq_value[15:8]; bytes[11]=seq_value[7:0];
      bytes[12]=payload_len[15:8]; bytes[13]=payload_len[7:0];
      bytes[14]=0; bytes[15]=0;
      crc=32'hffffffff;
      for (k=0; k<16+payload_len; k=k+1) crc=crc_byte(crc,bytes[k]);
      crc=~crc;
      bytes[16+payload_len]=crc[31:24]; bytes[17+payload_len]=crc[23:16];
      bytes[18+payload_len]=crc[15:8]; bytes[19+payload_len]=crc[7:0];
    end
  endtask

  integer i;
  reg [31:0] captured_crc;
  initial begin
    repeat (5) @(posedge clk); rst_n=1; repeat (5) @(posedge clk);

    make_frame(8'h01,0,32'hffff_ffff);
    bytes[19] = bytes[19] ^ 8'h01;
    send_array(20);
    wait (crc_errors == 1);

    make_frame(8'h01,0,32'hffff_ffff);
    fork
      send_array(20);
      begin
        for (i=0; i<40; i=i+1) receive_byte(bytes[i]);
      end
    join
    if ({bytes[0],bytes[1],bytes[2],bytes[3]} != 32'h4d435332 ||
        bytes[4] != 2 || bytes[5] != 8'h81 || bytes[7] != 16 ||
        {bytes[12],bytes[13]} != 16'd20)
      $fatal(1, "HELLO response header malformed");
    captured_crc=32'hffffffff;
    for (i=0; i<36; i=i+1) captured_crc=crc_byte(captured_crc,bytes[i]);
    captured_crc=~captured_crc;
    if ({bytes[36],bytes[37],bytes[38],bytes[39]} != captured_crc)
      $fatal(1, "HELLO response CRC mismatch");

    chunk_toggle = 1;
    for (i=0; i<1084; i=i+1) receive_byte(bytes[i]);
    if ({bytes[0],bytes[1],bytes[2],bytes[3]} != 32'h4d435332 ||
        bytes[4] != 2 || bytes[5] != 8'h84 || bytes[7] != 16 ||
        {bytes[12],bytes[13]} != 16'd1064)
      $fatal(1, "spectrum frame header malformed");
    captured_crc=32'hffffffff;
    for (i=0; i<1080; i=i+1) captured_crc=crc_byte(captured_crc,bytes[i]);
    captured_crc=~captured_crc;
    if ({bytes[1080],bytes[1081],bytes[1082],bytes[1083]} != captured_crc)
      $fatal(1, "spectrum frame CRC mismatch");

    // A malformed stop bit must be reported without wedging magic search.
    send_byte(8'h55, 1'b0);
    wait (frame_errors != 0);

    for (i=0; i<20; i=i+1) bytes[16+i]=i;
    bytes[16]=8'h4d; bytes[17]=8'h43; bytes[18]=8'h46; bytes[19]=8'h32;
    make_frame(8'h02,20,32'h0000_0001);
    // make_frame writes only the header/CRC and preserves payload bytes.
    send_array(40);
    wait (cmd_valid);
    if (cmd_len != 20 || cmd_payload[31:0] != 32'h3246434d)
      $fatal(1, "MCF2 payload forwarding failed");
    $display("tb_mca_uart_protocol PASS");
    $finish;
  end
endmodule
