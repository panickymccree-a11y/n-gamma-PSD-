`timescale 1ns/1ps

module tb_mca_peak_interpolator;
  logic clk = 1'b0;
  always #4 clk = ~clk;
  logic rst_n = 1'b0;
  logic flush = 1'b0;
  logic enable;
  logic submit;
  logic [13:0] raw_peak, left_sample, center_sample, right_sample;
  logic candidate_valid, invalid_overrange;
  logic [47:0] timestamp;
  logic [23:0] integral;
  logic [7:0] flags;
  logic busy, result_valid;
  logic [95:0] event_data;
  logic [13:0] histogram_peak;
  logic [23:0] histogram_integral;

  mca_peak_interpolator dut (.*);

  task automatic submit_event(input logic do_interpolate,
                              input logic [13:0] raw,
                              input logic [13:0] left,
                              input logic [13:0] center,
                              input logic [13:0] right,
                              input logic valid,
                              input logic [47:0] ts);
    begin
      @(negedge clk);
      enable = do_interpolate;
      raw_peak = raw;
      left_sample = left;
      center_sample = center;
      right_sample = right;
      candidate_valid = valid;
      invalid_overrange = 1'b0;
      timestamp = ts;
      integral = 24'h123456;
      flags = 8'h12;
      submit = 1'b1;
      @(negedge clk);
      submit = 1'b0;
    end
  endtask

  initial begin
    enable = 1'b0; submit = 1'b0; raw_peak = '0;
    left_sample = '0; center_sample = '0; right_sample = '0;
    candidate_valid = 1'b0; invalid_overrange = 1'b0;
    timestamp = '0; integral = '0; flags = '0;
    repeat (3) @(negedge clk);
    rst_n = 1'b1;

    submit_event(1'b0, 14'd1000, 14'd0, 14'd0, 14'd0, 1'b0, 48'h55);
    wait (result_valid);
    #1;
    if (event_data[95:48] !== 48'h55 || event_data[47:32] !== 16'd4000 ||
        histogram_peak !== 14'd1000 || histogram_integral !== 24'h123456)
      $fatal(1, "bypass result mismatch: data=%h hist=%0d", event_data, histogram_peak);
    @(negedge clk);

    // d=10, s=30, d*d/(2*s)=100/60 rounded to 2; 100*4+2=402.
    submit_event(1'b1, 14'd100, 14'd90, 14'd100, 14'd80, 1'b1, 48'h1234);
    wait (result_valid);
    #1;
    if (event_data[95:48] !== 48'h1234 || event_data[47:32] !== 16'd402 ||
        histogram_peak !== 14'd100 || event_data[7:0] !== 8'h12)
      $fatal(1, "interpolated result mismatch: data=%h hist=%0d", event_data, histogram_peak);
    @(negedge clk);

    // A platform is a deterministic fallback to the raw center value.
    submit_event(1'b1, 14'd200, 14'd200, 14'd200, 14'd200, 1'b1, 48'h9876);
    wait (result_valid);
    #1;
    if (event_data[47:32] !== 16'd800 || histogram_peak !== 14'd200)
      $fatal(1, "fallback result mismatch: data=%h hist=%0d", event_data, histogram_peak);
    $display("tb_mca_peak_interpolator PASS");
    $finish;
  end
endmodule
