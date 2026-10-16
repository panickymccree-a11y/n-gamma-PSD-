`timescale 1ns/1ps

module tb_mca_frontend_filter_response;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  always #4 clk = ~clk;

  logic filter_enable = 1'b1;
  logic clear_history = 1'b0;
  logic sample_valid = 1'b0;
  logic [13:0] sample = '0;
  logic [47:0] timestamp = '0;
  logic output_valid;
  logic [13:0] output_sample;
  logic output_overrange;
  logic [47:0] output_timestamp;

  real pi = 3.14159265358979323846;
  real acc_cos, acc_sin, angle, amplitude, measured_db;
  integer measured_count;
  integer code;

  mca_frontend_filter dut (
    .clk(clk), .rst_n(rst_n), .filter_enable(filter_enable),
    .clear_history(clear_history), .sample_valid(sample_valid),
    .sample(sample), .raw_overrange(1'b0), .timestamp(timestamp),
    .output_valid(output_valid), .output_sample(output_sample),
    .output_overrange(output_overrange), .output_timestamp(output_timestamp)
  );

  task automatic tick(input logic valid, input logic [13:0] value,
                      input logic [47:0] time_value);
    begin
      @(negedge clk);
      sample_valid = valid;
      sample = value;
      timestamp = time_value;
      @(posedge clk);
      #1;
    end
  endtask

  task automatic collect(input real frequency_mhz);
    begin
      if (output_valid && output_timestamp >= 250 &&
          output_timestamp < 1250) begin
        angle = 2.0*pi*frequency_mhz*$itor(output_timestamp)/125.0;
        acc_cos = acc_cos + ($itor(output_sample)-8192.0)*$cos(angle);
        acc_sin = acc_sin + ($itor(output_sample)-8192.0)*$sin(angle);
        measured_count = measured_count + 1;
      end
    end
  endtask

  task automatic measure(input real frequency_mhz,
                         input real expected_db,
                         input real tolerance_db);
    begin
      clear_history = 1'b1;
      tick(1'b0, '0, '0);
      clear_history = 1'b0;
      acc_cos = 0.0;
      acc_sin = 0.0;
      measured_count = 0;

      // N=1000 measured samples contains an integer number of cycles at all
      // three test frequencies.  The first 250 samples discard startup.
      for (int n = 0; n < 1250; n++) begin
        code = $rtoi(8192.0 + 2000.0*$sin(2.0*pi*frequency_mhz*n/125.0));
        tick(1'b1, code[13:0], n);
        collect(frequency_mhz);
      end
      // The FIR keeps the fixed 20-cycle metadata latency; drain the full
      // latency so the 250..1249 timestamp window contains all 1000 points.
      for (int flush = 0; flush < 20; flush++) begin
        tick(1'b0, '0, '0);
        collect(frequency_mhz);
      end

      if (measured_count != 1000)
        $fatal(1, "%0.1f MHz collected %0d samples, expected 1000",
               frequency_mhz, measured_count);
      amplitude = 2.0*$sqrt(acc_cos*acc_cos + acc_sin*acc_sin)/measured_count;
      measured_db = 20.0*$ln(amplitude/2000.0)/$ln(10.0);
      $display("RTL_RESPONSE %0.1f MHz amplitude=%0.3f attenuation=%0.4f dB",
               frequency_mhz, amplitude, measured_db);
      if (measured_db < expected_db-tolerance_db ||
          measured_db > expected_db+tolerance_db)
        $fatal(1, "%0.1f MHz response outside tolerance", frequency_mhz);
    end
  endtask

  initial begin
    repeat (3) @(posedge clk);
    rst_n = 1'b1;
    measure(10.0, -0.02, 0.10);
    measure(12.0, -0.02, 0.10);
    measure(30.0, -64.2, 2.5);
    $display("tb_mca_frontend_filter_response PASS");
    $finish;
  end
endmodule
