`timescale 1ns/1ps

module tb_mca_external_trigger;
  logic clk = 1'b0;
  always #4 clk = ~clk; // 125 MHz DCO

  logic rst_n = 1'b0;
  logic ex_tri = 1'b0;
  logic enable = 1'b0;
  logic abort = 1'b0;
  logic clear_count = 1'b0;
  logic [19:0] duration_us = 20'd1;
  logic active;
  logic [31:0] count;

  mca_external_trigger dut (
    .clk(clk), .rst_n(rst_n), .ex_tri_async(ex_tri), .enable(enable),
    .abort(abort), .clear_count(clear_count), .duration_us(duration_us),
    .window_active(active), .trigger_count(count));

  task automatic check(input logic condition, input string message);
    if (!condition) begin
      $display("FAIL: %s", message);
      $fatal(1);
    end
  endtask

  task automatic step(input int n);
    repeat(n)begin @(posedge clk);#1;end
  endtask
  task automatic wait_rise;
    automatic logic [31:0] previous=count;
    @(negedge clk);ex_tri=1;
    while(count==previous)step(1);
    @(negedge clk);ex_tri=0;
  endtask
  initial begin
    step(3);@(negedge clk);rst_n=1;step(2);
    // Disabled mode ignores edges.
    @(negedge clk);ex_tri=1;step(5);@(negedge clk);ex_tri=0;step(4);
    check(!active && count==0,"disabled EXTRI must be ignored");
    // Synchronize the high level while disabled, before arming.
    @(negedge clk);ex_tri=1;step(4);@(negedge clk);enable=1;step(4);
    check(!active && count==0,"arming synchronized high level must not trigger");
    @(negedge clk);ex_tri=0;step(4);
    wait_rise();check(active && count==1,"rising edge opens one-us window");
    step(124);check(active,"one-us window ended early");
    step(1);check(!active,"one-us window closes after 125 clocks");
    duration_us=2;wait_rise();check(active && count==2,"second edge counts");
    step(124);wait_rise();check(active && count==3,"retrigger reloads duration");
    step(249);check(active,"two-us window ended early");step(1);check(!active,"two-us window closes");
    wait_rise();check(active && count==4,"next edge counts");
    @(negedge clk);abort=1;step(1);check(!active,"abort closes window");@(negedge clk);abort=0;step(4);
    wait_rise();check(active && count==5,"remains armed after abort");
    @(negedge clk);clear_count=1;step(1);check(count==0,"clear count");@(negedge clk);clear_count=0;
    $display("PASS: external trigger gate");
    $finish;
  end
endmodule
