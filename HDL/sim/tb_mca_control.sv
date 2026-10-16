`timescale 1ns/1ps

module tb_mca_control;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic cmd_valid = 1'b0;
  logic cmd_source = 1'b0;
  logic [511:0] cmd_payload = '0;
  logic [7:0] cmd_len = 8'd20;
  logic cmd_ready, busy;
  logic resp_valid;
  logic [519:0] resp_payload;
  logic [7:0] resp_len;
  logic resp_ready = 1'b0;
  logic [275:0] cfg_bus;
  logic external_armed = 1'b0;
  logic external_window_active = 1'b0;
  logic [31:0] external_trigger_count = 32'd0;
  logic cfg_toggle;
  logic cfg_ack = 1'b0;
  logic commit_ack = 1'b0;
  logic [31:0] applied_config_version = 32'h0001_0000;
  logic [1:0] range_request_value;
  logic range_request_strobe, abort_toggle, clear_counts_toggle;
  int errors = 0;

  // Waveform status snapshots.
  logic [31:0] wave_status = 32'd2;
  logic [31:0] wave_last_capture = 32'd5;
  logic [31:0] wave_capture_count = 32'd6;

  mca_control dut (
    .clk(clk), .rst_n(rst_n), .cmd_valid(cmd_valid), .cmd_source(cmd_source),
    .cmd_payload(cmd_payload), .cmd_len(cmd_len), .cmd_ready(cmd_ready),
    .busy(busy), .resp_valid(resp_valid), .resp_payload(resp_payload),
    .resp_len(resp_len), .resp_ready(resp_ready), .cfg_bus(cfg_bus),
    .external_armed(external_armed),
    .external_window_active(external_window_active),
    .external_trigger_count(external_trigger_count),
    .cfg_toggle(cfg_toggle), .cfg_ack(cfg_ack), .commit_ack(commit_ack),
    .applied_config_version(applied_config_version),
    .applied_baseline(14'd8192), .applied_baseline_state(2'd1),
    .applied_baseline_ready(1'b1), .applied_baseline_auto(1'b0),
    .applied_manual_baseline(14'd8192), .run_enable(1'b0),
    .range_applied_value(2'd1), .range_busy(1'b0), .phy_ready(1'b1),
    .init_done(1'b1), .q_busy(1'b0), .filter_enabled(1'b0),
    .frontend_ready(1'b1), .event_count(32'd10),
    .rejected_count(32'd2), .dropped_count(32'd3),
    .timestamp(48'h0102_0304_0506), .event_active(1'b0),
    .baseline_ready_in(1'b1),
    .wave_status(wave_status), .wave_last_capture(wave_last_capture),
    .wave_capture_count(wave_capture_count),
    .range_request_value(range_request_value),
    .range_request_strobe(range_request_strobe), .abort_toggle(abort_toggle),
    .clear_counts_toggle(clear_counts_toggle)
  );

  task automatic check(input bit condition, input string message);
    if (!condition) begin
      errors++;
      $display("FAIL [%0t] %s", $time, message);
    end
  endtask

  task automatic put_byte(input int index, input logic [7:0] value);
    cmd_payload[index*8 +: 8] = value;
  endtask

  task automatic send_request(input logic [31:0] magic,
                              input logic [7:0] version,
                              input logic [7:0] opcode,
                              input logic [7:0] target,
                              input logic [7:0] target_id,
                              input logic [15:0] reg_addr,
                              input logic [31:0] transaction,
                              input logic [31:0] value);
    begin
      while (!cmd_ready) @(posedge clk);
      @(negedge clk);
      cmd_payload = '0;
      put_byte(0, magic[31:24]); put_byte(1, magic[23:16]);
      put_byte(2, magic[15:8]);  put_byte(3, magic[7:0]);
      put_byte(4, version); put_byte(5, opcode); put_byte(6, target);
      put_byte(7, target_id); put_byte(8, reg_addr[15:8]);
      put_byte(9, reg_addr[7:0]);
      put_byte(12, transaction[31:24]); put_byte(13, transaction[23:16]);
      put_byte(14, transaction[15:8]);  put_byte(15, transaction[7:0]);
      put_byte(16, value[31:24]); put_byte(17, value[23:16]);
      put_byte(18, value[15:8]);  put_byte(19, value[7:0]);
      cmd_len = 8'd20;
      cmd_valid = 1'b1;
      @(negedge clk);
      cmd_valid = 1'b0;
    end
  endtask

  function automatic logic [31:0] resp_u32(input int index);
    resp_u32 = {resp_payload[index*8 +: 8],
                resp_payload[(index+1)*8 +: 8],
                resp_payload[(index+2)*8 +: 8],
                resp_payload[(index+3)*8 +: 8]};
  endfunction

  function automatic logic [15:0] resp_u16(input int index);
    resp_u16 = {resp_payload[index*8 +: 8],
                resp_payload[(index+1)*8 +: 8]};
  endfunction

  task automatic expect_response(input logic [31:0] transaction,
                                 input logic [7:0] opcode,
                                 input logic [7:0] status,
                                 input logic [15:0] reg_addr,
                                 input logic [31:0] value);
    int guard;
    begin
      guard = 0;
      while (!resp_valid && guard < 100) begin
        @(posedge clk); #1; guard++;
      end
      check(resp_valid, "response timeout");
      check(resp_len == 16, $sformatf("response length %0d", resp_len));
      check(resp_u32(0) == transaction, "transaction byte order/value");
      check(resp_payload[4*8 +: 8] == opcode, "response opcode");
      check(resp_payload[7*8 +: 8] == status,
            $sformatf("status %0d expected %0d",
                      resp_payload[7*8 +: 8], status));
      check(resp_u16(8) == reg_addr, "register byte order/value");
      check(resp_u32(12) == value,
            $sformatf("response value %08x expected %08x",
                      resp_u32(12), value));
      @(negedge clk); resp_ready = 1'b1;
      @(negedge clk); resp_ready = 1'b0;
    end
  endtask

  initial begin
    logic old_toggle;
    repeat (5) @(posedge clk);
    rst_n = 1'b1;

    // Extended capability read in the exact byte order generated by Host.
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h010B,
                 32'h0102_0304, 0);
    expect_response(32'h0102_0304, 8'h02, 8'h00, 16'h010B, 32'd14);

    // Full-range manual baseline write and idempotent replay.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0007,
                 32'h1000_0001, 32'd16383);
    expect_response(32'h1000_0001, 8'h01, 8'h00, 16'h0007, 32'd16383);
    check(cfg_bus[143:128] == 14'd16383, "manual baseline not staged");
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0007,
                 32'h1000_0001, 32'd1234);
    expect_response(32'h1000_0001, 8'h01, 8'h00, 16'h0007, 32'd16383);
    check(cfg_bus[143:128] == 14'd16383, "duplicate transaction was reapplied");

    // Invalid 14-bit value is rejected.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0007,
                 32'h1000_0002, 32'd16384);
    expect_response(32'h1000_0002, 8'h01, 8'h04, 16'h0007, 0);

    // Stage a threshold, then verify commit waits for the sampling-domain ack.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h02, 0, 16'h0010,
                 32'h1000_0003, 32'd100);
    expect_response(32'h1000_0003, 8'h01, 8'h00, 16'h0010, 32'd100);
    old_toggle = cfg_toggle;
    send_request(32'h4D43_4632, 2, 8'h03, 8'h00, 0, 0,
                 32'h1000_0004, 0);
    repeat (5) begin
      @(posedge clk); #1;
      check(!resp_valid, "commit responded before apply acknowledgement");
    end
    check(busy && cfg_toggle != old_toggle, "commit handshake did not start");
    applied_config_version = 32'h0001_0001;
    @(negedge clk); cfg_ack = 1'b1;
    @(negedge clk); cfg_ack = 1'b0;
    expect_response(32'h1000_0004, 8'h03, 8'h00, 0, 32'h0001_0001);
    check(!busy, "commit remained busy after acknowledgement");

    send_request(32'h4D43_4632, 2, 8'h02, 8'h02, 0, 16'h0020,
                 32'h1000_0005, 0);
    expect_response(32'h1000_0005, 8'h02, 8'h00, 16'h0020, 32'd100);

    // START/STOP use the same applied-configuration handshake as COMMIT.
    // Neither command may acknowledge while the DCO domain still exposes the
    // previous run state.
    old_toggle = cfg_toggle;
    send_request(32'h4D43_4632, 2, 8'h04, 8'h00, 0, 0,
                 32'h1000_0006, 0);
    repeat (3) begin
      @(posedge clk); #1;
      check(!resp_valid, "start responded before apply acknowledgement");
    end
    check(busy && cfg_toggle != old_toggle && cfg_bus[145],
          "start handshake did not stage run enable");
    applied_config_version = 32'h0001_0002;
    @(negedge clk); cfg_ack = 1'b1;
    @(negedge clk); cfg_ack = 1'b0;
    expect_response(32'h1000_0006, 8'h04, 8'h00, 0, 32'h0001_0002);

    old_toggle = cfg_toggle;
    send_request(32'h4D43_4632, 2, 8'h05, 8'h00, 0, 0,
                 32'h1000_0007, 0);
    repeat (3) begin
      @(posedge clk); #1;
      check(!resp_valid, "stop responded before apply acknowledgement");
    end
    check(busy && cfg_toggle != old_toggle && !cfg_bus[145],
          "stop handshake did not clear run enable");
    applied_config_version = 32'h0001_0003;
    @(negedge clk); cfg_ack = 1'b1;
    @(negedge clk); cfg_ack = 1'b0;
    expect_response(32'h1000_0007, 8'h05, 8'h00, 0, 32'h0001_0003);

    // Frame validation and opcode errors.
    send_request(32'hDEAD_BEEF, 2, 8'h02, 8'h00, 0, 16'h0100,
                 32'h2000_0001, 0);
    expect_response(32'h2000_0001, 8'h02, 8'h01, 16'h0100, 0);
    send_request(32'h4D43_4632, 3, 8'h02, 8'h00, 0, 16'h0100,
                 32'h2000_0002, 0);
    expect_response(32'h2000_0002, 8'h02, 8'h01, 16'h0100, 0);
    send_request(32'h4D43_4632, 2, 8'h7F, 8'h00, 0, 0,
                 32'h2000_0003, 0);
    expect_response(32'h2000_0003, 8'h7F, 8'h02, 0, 0);

    // Cross-field validation is deferred to commit.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h02, 0, 16'h0012,
                 32'h3000_0001, 32'd50);
    expect_response(32'h3000_0001, 8'h01, 8'h00, 16'h0012, 32'd50);
    send_request(32'h4D43_4632, 2, 8'h01, 8'h02, 0, 16'h0013,
                 32'h3000_0002, 32'd40);
    expect_response(32'h3000_0002, 8'h01, 8'h00, 16'h0013, 32'd40);
    send_request(32'h4D43_4632, 2, 8'h03, 8'h00, 0, 0,
                 32'h3000_0003, 0);
    expect_response(32'h3000_0003, 8'h03, 8'h07, 0, 0);

    // ------------------------------------------------------------------
    // Waveform mode registers.
    // ------------------------------------------------------------------
    // Mode 3 (waveform) and mode 4 (external trigger) are accepted; the
    // legacy multi-channel numbers 1 and 2 stay rejected.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0000,
                 32'h4000_0001, 32'd3);
    expect_response(32'h4000_0001, 8'h01, 8'h00, 16'h0000, 32'd3);
    check(cfg_bus[147] == 1'b1, "waveform mode flag not staged");
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h0000,
                 32'h4000_0002, 0);
    expect_response(32'h4000_0002, 8'h02, 8'h00, 16'h0000, 32'd3);
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0000,
                 32'h4000_0003, 32'd1);
    expect_response(32'h4000_0003, 8'h01, 8'h04, 16'h0000, 0);
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0000,
                 32'h4000_0004, 32'd2);
    expect_response(32'h4000_0004, 8'h01, 8'h04, 16'h0000, 0);
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0000,
                 32'h4000_0010, 32'd4);
    expect_response(32'h4000_0010, 8'h01, 8'h00, 16'h0000, 32'd4);
    check(cfg_bus[229] == 1'b1, "external mode flag not staged");
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h000B,
                 32'h4000_0011, 32'd1234);
    expect_response(32'h4000_0011, 8'h01, 8'h00, 16'h000B, 32'd1234);
    check(cfg_bus[249:230] == 20'd1234, "external duration not staged");
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h000B,
                 32'h4000_0012, 32'd1_000_001);
    expect_response(32'h4000_0012, 8'h01, 8'h04, 16'h000B, 0);

    // Waveform trigger mode and session id shadows.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0008,
                 32'h4000_0005, 32'd1);
    expect_response(32'h4000_0005, 8'h01, 8'h00, 16'h0008, 32'd1);
    check(cfg_bus[148] == 1'b1, "single-shot flag not staged");
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0008,
                 32'h4000_0006, 32'd2);
    expect_response(32'h4000_0006, 8'h01, 8'h04, 16'h0008, 0);
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0009,
                 32'h4000_0007, 32'hAABB_CCDD);
    expect_response(32'h4000_0007, 8'h01, 8'h00, 16'h0009, 32'hAABB_CCDD);
    check(cfg_bus[181:150] == 32'hAABB_CCDD, "session id not staged");
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h0009,
                 32'h4000_0008, 0);
    expect_response(32'h4000_0008, 8'h02, 8'h00, 16'h0009, 32'hAABB_CCDD);

    // The waveform status registers are read-only snapshots.
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h0120,
                 32'h4000_0009, 0);
    expect_response(32'h4000_0009, 8'h02, 8'h00, 16'h0120, wave_status);
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h0121,
                 32'h4000_000A, 0);
    expect_response(32'h4000_000A, 8'h02, 8'h00, 16'h0121,
                    wave_last_capture);
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h0122,
                 32'h4000_000B, 0);
    expect_response(32'h4000_000B, 8'h02, 8'h00, 16'h0122,
                    wave_capture_count);
    // A write to a read-only waveform status register is rejected.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h00, 0, 16'h0120,
                 32'h4000_000C, 32'd1);
    expect_response(32'h4000_000C, 8'h01, 8'h04, 16'h0120, 0);

    // The polarity bit of the channel control word reaches the sampling
    // domain, and bit 0 still enables the channel.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h02, 0, 16'h0000,
                 32'h4000_000D, 32'd3);
    expect_response(32'h4000_000D, 8'h01, 8'h00, 16'h0000, 32'd3);
    check(cfg_bus[146] == 1'b1, "negative polarity not staged");
    send_request(32'h4D43_4632, 2, 8'h01, 8'h02, 0, 16'h0000,
                 32'h4000_000E, 32'd0);
    expect_response(32'h4000_000E, 8'h01, 8'h00, 16'h0000, 32'd0);
    check(cfg_bus[146] == 1'b0, "positive polarity not staged");

    // The format capability flags advertise the waveform protocol.
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h010A,
                 32'h4000_000F, 0);
    expect_response(32'h4000_000F, 8'h02, 8'h00, 16'h010A,
                    32'h0000_077F);

    // Reads cross the transport ownership boundary and an identical
    // transaction id must not replay the UDP response into UART.
    cmd_source = 1'b1;
    send_request(32'h4D43_4632, 2, 8'h02, 8'h00, 0, 16'h010B,
                 32'h4000_000F, 0);
    expect_response(32'h4000_000F, 8'h02, 8'h00, 16'h010B, 32'd14);
    // UDP currently owns the shadow bank, so UART modification is BUSY.
    send_request(32'h4D43_4632, 2, 8'h01, 8'h02, 0, 16'h0010,
                 32'h5000_0001, 32'd77);
    expect_response(32'h5000_0001, 8'h01, 8'h05, 16'h0010, 0);
    cmd_source = 1'b0;

    // PSD capability and commit invariants, independent of earlier shadow values.
    rst_n=0;repeat(4)@(negedge clk);rst_n=1;repeat(4)@(negedge clk);
    send_request(32'h4D434632,2,2,0,0,16'h0111,32'h60000001,0);
    expect_response(32'h60000001,2,0,16'h0111,10000);
    send_request(32'h4D434632,2,1,0,0,0,32'h60000002,5);
    expect_response(32'h60000002,1,0,0,5);
    send_request(32'h4D434632,2,1,2,0,16'h0014,32'h60000003,1251);
    expect_response(32'h60000003,1,0,16'h0014,1251);
    send_request(32'h4D434632,2,3,0,0,0,32'h60000004,0);
    expect_response(32'h60000004,3,7,0,0);
    send_request(32'h4D434632,2,1,2,0,16'h0014,32'h60000005,1250);
    expect_response(32'h60000005,1,0,16'h0014,1250);
    send_request(32'h4D434632,2,1,2,0,16'h0015,32'h60000006,4);
    expect_response(32'h60000006,1,0,16'h0015,4);
    send_request(32'h4D434632,2,1,2,0,16'h001B,32'h60000007,-32'sd4);
    expect_response(32'h60000007,1,0,16'h001B,-32'sd4);
    send_request(32'h4D434632,2,1,2,0,16'h001C,32'h60000008,2);
    expect_response(32'h60000008,1,0,16'h001C,2);
    check(cfg_bus[263:252]==0 && cfg_bus[275:264]==6,"PSD gate conversion");
    send_request(32'h4D434632,2,1,0,0,16'h000C,32'h60000009,1);
    expect_response(32'h60000009,1,0,16'h000C,1);
    send_request(32'h4D434632,2,3,0,0,0,32'h6000000A,0);
    expect_response(32'h6000000A,3,7,0,0);
    send_request(32'h4D434632,2,1,0,0,16'h000C,32'h6000000B,0);
    expect_response(32'h6000000B,1,0,16'h000C,0);
    send_request(32'h4D434632,2,1,2,0,16'h001C,32'h6000000C,-32'sd4);
    expect_response(32'h6000000C,1,0,16'h001C,-32'sd4);
    send_request(32'h4D434632,2,3,0,0,0,32'h6000000D,0);
    expect_response(32'h6000000D,3,7,0,0);
    // The next transaction must see the newest shadow values through both
    // validation registers, including recovery from an invalid gate.
    send_request(32'h4D434632,2,1,2,0,16'h001C,32'h60000010,2);
    expect_response(32'h60000010,1,0,16'h001C,2);
    send_request(32'h4D434632,2,3,0,0,0,32'h60000011,0);
    repeat(5) begin @(posedge clk);#1;check(!resp_valid,"PSD commit replied before acknowledgement");end
    check(busy,"valid PSD commit did not start");
    applied_config_version=32'h00010004;
    @(negedge clk);cfg_ack=1;
    @(negedge clk);cfg_ack=0;
    expect_response(32'h60000011,3,0,0,32'h00010004);
    // A new reset removes transport ownership so UART mode rejection is tested directly.
    rst_n=0;repeat(4)@(negedge clk);rst_n=1;repeat(4)@(negedge clk);cmd_source=1;
    send_request(32'h4D434632,2,1,0,0,0,32'h6000000E,5);
    expect_response(32'h6000000E,1,4,0,0);
    cmd_source=0;

    if (errors == 0)
      $display("tb_mca_control PASS");
    else
      $display("tb_mca_control FAIL: %0d errors", errors);
    $finish;
  end
endmodule
