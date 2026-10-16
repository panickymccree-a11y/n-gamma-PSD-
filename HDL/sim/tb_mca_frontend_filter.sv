`timescale 1ns/1ps

module tb_mca_frontend_filter;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic filter_enable = 1'b0;
  logic clear_history = 1'b0;
  logic sample_valid = 1'b0;
  logic [13:0] sample = '0;
  logic raw_overrange = 1'b0;
  logic [47:0] timestamp = '0;
  logic output_valid;
  logic [13:0] output_sample;
  logic output_overrange;
  logic [47:0] output_timestamp;
  int seen;
  int expected_index;
  logic [13:0] last_filtered;

  mca_frontend_filter dut (
    .clk(clk), .rst_n(rst_n), .filter_enable(filter_enable),
    .clear_history(clear_history), .sample_valid(sample_valid),
    .sample(sample), .raw_overrange(raw_overrange), .timestamp(timestamp),
    .output_valid(output_valid), .output_sample(output_sample),
    .output_overrange(output_overrange), .output_timestamp(output_timestamp)
  );

  task automatic tick(input logic valid, input logic [13:0] value,
                      input logic overrange, input logic [47:0] time_value);
    begin
      @(negedge clk);
      sample_valid = valid;
      sample = value;
      raw_overrange = overrange;
      timestamp = time_value;
      @(posedge clk);
      #1;
    end
  endtask

  initial begin
    repeat (3) @(posedge clk);
    rst_n = 1'b1;

    // Bypass still carries the fixed twenty-cycle metadata/sample latency.
    tick(1'b1, 14'h1234, 1'b1, 48'h55);
    for (int cycle = 1; cycle <= 19; cycle++) begin
      tick(1'b0, '0, 1'b0, '0);
      if (cycle < 19 && output_valid)
        $fatal(1, "bypass output became valid before twenty-cycle latency");
    end
    if (!output_valid || output_sample !== 14'h1234 ||
        !output_overrange || output_timestamp !== 48'h55)
      $fatal(1, "bypass sample or metadata did not preserve fixed latency");

    // Clearing history removes stale RC state.  A constant stream must then
    // converge to the same code while the metadata remains aligned.
    filter_enable = 1'b1;
    clear_history = 1'b1;
    tick(1'b0, '0, 1'b0, '0);
    clear_history = 1'b0;

    seen = 0;
    last_filtered = '0;
    for (int input_index = 0; input_index < 40; input_index++) begin
      tick(1'b1, 14'd6000, input_index == 7, input_index);
      if (output_valid) begin
        expected_index = input_index - 19;
        if (output_timestamp !== expected_index ||
            output_overrange !== ((expected_index >= 13) &&
                                  (expected_index <= 33)))
          $fatal(1, "filtered metadata lost alignment at output %0d", input_index);
        last_filtered = output_sample;
        seen++;
      end
    end
    for (int flush = 0; flush < 20; flush++) begin
      tick(1'b0, '0, 1'b0, '0);
      if (output_valid) begin
        last_filtered = output_sample;
        seen++;
      end
    end
    if (seen < 20)
      $fatal(1, "expected at least 20 filtered outputs, got %0d", seen);
    if (last_filtered < 14'd5990 || last_filtered > 14'd6000)
      $fatal(1, "filtered constant did not converge: %0d", last_filtered);

    $display("tb_mca_frontend_filter PASS");
    $finish;
  end
endmodule
