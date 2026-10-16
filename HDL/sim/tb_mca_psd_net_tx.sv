`timescale 1ns/1ps
module tb_mca_psd_net_tx;
  logic clk=0;always #4 clk=~clk;
  logic rst=0,rd,empty;
  logic [231:0] dout=0,words[6];
  logic [8:0] level;
  int read_index=0;
  logic tx_start,req=0,done=0;
  logic [15:0] length;
  logic [7:0] tx;
  assign empty=read_index>=6;
  assign level=6-read_index;
  always @(posedge clk)if(rd && !empty)begin dout<=words[read_index];read_index<=read_index+1;end
  mca_net_tx #(.EVENT_TIMEOUT_CYCLES(3)) dut(
    .clk(clk),.rst_n(rst),.device_id(8'd0),.event_empty(empty),.event_rd_en(rd),.event_dout(dout),.event_level(level),
    .resp_empty(1'b1),.resp_dout(520'd0),.status_empty(1'b1),.status_dout(1024'd0),
    .wave_valid(1'b0),.wave_cancel(1'b0),.cfg_version(32'd999),
    .tx_start_en(tx_start),.tx_byte_num(length),.tx_data(tx),.tx_data_req(req),.tx_done(done)
  );
  task automatic packet(input int typ,version,first,count);
    automatic int bytes_per=typ==7 ? 24:12;
    automatic int guard=0;
    while(!tx_start && guard<1000)begin @(posedge clk);#1;guard++;end
    if(!tx_start || length!=20+count*bytes_per)$fatal(1,"PSD packet length %d count %d",length,count);
    for(int i=0;i<length;i++)begin
      automatic logic [7:0] expected;
      @(negedge clk);
      if(i==5 && tx!=typ)$fatal(1,"PSD packet type %d expected%d",tx,typ);
      if(i>=12 && i<=15 && tx!=((version>>(8*(15-i)))&255))$fatal(1,"wrong config version byte");
      if(i==17 && tx!=count)$fatal(1,"wrong packet count %d",tx);
      if(i>=20) begin
        automatic int rec=(i-20)/bytes_per;
        automatic int pos=(i-20)%bytes_per;
        if(typ==7)expected=words[first+rec][191-pos*8 -:8];
        else expected=words[first+rec][95-pos*8 -:8];
        if(tx!==expected)$fatal(1,"wrong event byte %d value %h expected %h",i,tx,expected);
      end
      req=1;
    end
    @(negedge clk);req=0;done=1;@(negedge clk);done=0;
  endtask
  initial begin
    for(int i=0;i<6;i++)words[i]=0;
    words[0]={32'd77,8'd7,96'h0102030405060708090a0b0c,32'sd1000,-32'sd20,16'd8192,8'd1,8'd0};
    words[1]=words[0];words[1][95:64]=2000;
    words[2]=words[0];words[2][231:200]=78;
    words[3]={32'd78,8'd1,96'd0,96'h1112131415161718191a1b1c};
    words[4]=words[3];words[4][7:0]=8'h44;
    words[5]=words[0];words[5][231:200]=79;
    repeat(5)@(negedge clk);rst=1;
    packet(7,77,0,2);packet(7,78,2,1);packet(1,78,3,2);packet(7,79,5,1);
    if(read_index!=6)$fatal(1,"FIFO pop mismatch");
    $display("tb_mca_psd_net_tx PASS: signed payload, pending record, mixed types/config versions");$finish;
  end
  initial begin #100000;$fatal(1,"network test timeout");end
endmodule
