`timescale 1ns/1ps
module tb_mca_psd_acquisition;
  logic clk=0;always #4 clk=~clk;
  logic rst_n=0,run=0,config_apply=0,neg=0,abort_event=0,clear_counts=0,full=0;
  logic [13:0] adc=8192;
  logic [13:0] base=8192;
  logic over=0;
  logic [11:0] n=150,p=0,tail_start=7,tail_end=150;
  logic wr,active,accepting,holdbase;
  logic [95:0] legacy;
  logic [231:0] data;
  logic [47:0] timestamp;
  logic [31:0] dropped,count;
  int history[20000];bit ranges[20000];int emitted=0;
  mca_acquisition dut(
    .dco_clk(clk),.rst_n(rst_n),.run_enable(run),.trigger_enable(run),
    .holdoff(1'b0),.abort_event(abort_event),.clear_counts(clear_counts),
    .config_apply(config_apply),.cfg_threshold(14'd100),.cfg_return(14'd20),
    .cfg_window_total(n),.cfg_window_pre(p),.cfg_polarity_negative(neg),
    .peak_interpolation_enable(1'b0),.psd_enable(1'b1),
    .psd_tail_start(tail_start),.psd_tail_end(tail_end),.config_version(32'd77),
    .baseline(base),.baseline_ready(1'b1),.baseline_state(2'd0),.baseline_hold(holdbase),
    .adc_data(adc),.adc_overrange(over),.event_wr(wr),.event_data(legacy),
    .tagged_event_data(data),.event_full(full),.timestamp(timestamp),
    .event_count(count),.dropped_count(dropped),.event_active(active),.accepting(accepting)
  );
  always @(posedge clk) if(rst_n) begin
    history[timestamp%20000]=adc;ranges[timestamp%20000]=over;
    if(wr) begin
      automatic int t=int(data[191:144]);
      automatic int total=0,tail=0;
      automatic bit range_bad=0;
      for(int i=0;i<n;i++)begin
        automatic int delta=history[(t-int(p)+i)%20000]-int'(base);
        if(neg)delta=-delta;
        total+=delta;if(i>=tail_start && i<tail_end)tail+=delta;
        range_bad |= ranges[(t-int(p)+i)%20000];
      end
      if($signed(data[95:64])!==total || $signed(data[63:32])!==tail)
        $fatal(1,"PSD mismatch N=%0d P=%0d gates=[%0d,%0d) total=%0d/%0d tail=%0d/%0d t=%0d",
          n,p,tail_start,tail_end,$signed(data[95:64]),total,$signed(data[63:32]),tail,t);
      if(data[231:200]!=77 || data[199:192]!=7 || data[31:16]!=base || data[9]!=0 || data[8]!=range_bad)
        $fatal(1,"PSD metadata mismatch %h expected over=%b",data,range_bad);
      emitted++;
    end
  end
  task automatic tick(input int value,input bit ov=0);
    @(negedge clk);adc=value;over=ov;
  endtask
  task automatic run_case(input int nn,pp,ss,ee,input bit negative,input bit range_test=0,input int base_value=8192);
    automatic int old=emitted;
    run=0;base=base_value;repeat(25)tick(base);
    n=nn;p=pp;tail_start=pp+ss;tail_end=pp+ee;neg=negative;
    config_apply=1;tick(base);config_apply=0;
    // Near-baseline signed noise, including a pre-trigger overrange marker.
    repeat(pp+20)tick(base+($urandom_range(0,6)-3),range_test);
    run=1;repeat(8)tick(base);
    tick(base+(negative ? -500 : 500));
    for(int i=1;i<nn+30;i++)tick(base+(negative ? -1:1)*(i<15 ? 100+(15-i)*10 : int'($urandom_range(0,6))-3),range_test && i==nn-pp-1);
    repeat(30)tick(base);
    if(emitted!=old+1)$fatal(1,"expected one PSD event got %0d",emitted-old);
  endtask
  initial begin
    repeat(5)@(negedge clk);rst_n=1;
    repeat(4200)tick(base);
    run_case(6,0,0,6,0);
    run_case(6,5,-5,1,1,1);
    run_case(150,0,7,150,0,1);
    run_case(150,8,-3,20,1,0,15000);
    run_case(150,8,-8,-7,0,1,1000);
    run_case(1250,0,1249,1250,0,1);
    run_case(1250,1249,-1249,1,1,1);
    for(int i=0;i<8;i++)run_case(80+i,5,-2,30+i,i[0]);
    // Maximum signed electric charge: legacy 24-bit integral saturates, PSD stays valid.
    run=0;base=0;repeat(30)tick(0);n=1250;p=0;tail_start=0;tail_end=1250;neg=0;
    config_apply=1;tick(0);config_apply=0;run=1;repeat(8)tick(0);repeat(1300)tick(16383);repeat(30)tick(0);
    base=8192;
    // FIFO drop is counted once; no stale record after abort/clear.
    full=1;run=0;repeat(30)tick(base);n=6;p=0;tail_start=0;tail_end=6;neg=0;
    config_apply=1;tick(base);config_apply=0;run=1;repeat(8)tick(base);tick(9000);repeat(60)tick(base);
    if(dropped!=1)$fatal(1,"FIFO drop count %0d",dropped);
    full=0;run=0;repeat(30)tick(base);n=150;tail_end=150;
    config_apply=1;tick(base);config_apply=0;run=1;repeat(8)tick(base);tick(9000);repeat(10)tick(8300);
    abort_event=1;tick(base);abort_event=0;repeat(200)tick(base);
    run_case(6,0,0,6,0);
    $display("tb_mca_psd_acquisition PASS: integer reference, polarity, signed noise, gates, wrap, overrange, FIFO, abort");$finish;
  end
  initial begin #1000000;$fatal(1,"PSD test timeout");end
endmodule
