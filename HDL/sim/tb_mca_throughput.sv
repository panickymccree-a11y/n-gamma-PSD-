`timescale 1ns/1ps
// Full batches through the real UDP request pipeline; no synthetic byte timing.
module tb_mca_throughput;
  logic clk=0,rst_n=0; always #4 clk=~clk;
  logic [231:0] words[152],dout=0;
  int read_index=0,frame=0,byte_index=0;
  logic rd,start,done,req,req_d=0,mux_d=0,en;
  logic [7:0] tx,gmii;
  logic [15:0] length;
  logic [8:0] level;
  assign level=152-read_index;
  always @(posedge clk) begin
    if(rd && read_index<152)begin dout<=words[read_index];read_index<=read_index+1;end
    req_d<=req;mux_d<=req;
  end
  mca_net_tx #(.MAX_BATCH(64),.PSD_MAX_BATCH(40)) dut(
    .clk(clk),.rst_n(rst_n),.device_id(8'd0),
    .event_empty(read_index>=152),.event_rd_en(rd),.event_dout(dout),.event_level(level),
    .resp_empty(1'b1),.resp_dout(520'd0),.status_empty(1'b1),.status_dout(1024'd0),
    .wave_valid(1'b0),.wave_cancel(1'b0),.cfg_version(32'd999),
    .tx_start_en(start),.tx_byte_num(length),.tx_data(tx),.tx_data_req(req_d),.tx_done(done)
  );
  udp_tx udp(
    .clk(clk),.rst_n(rst_n),.tx_start_en(start),.tx_data(mux_d ? tx:8'd0),.tx_byte_num(length),
    .des_mac(48'h020000000001),.des_ip(32'hc0a80164),.crc_data(32'd0),.crc_next(8'd0),
    .tx_done(done),.tx_req(req),.gmii_tx_en(en),.gmii_txd(gmii),.crc_en(),.crc_clr()
  );
  function automatic int count(input int f);
    case(f)0:count=64;1,2:count=40;3:count=3;4:count=5;default:count=0;endcase
  endfunction
  function automatic int first(input int f);
    case(f)0:first=0;1:first=64;2:first=104;3:first=144;4:first=147;default:first=0;endcase
  endfunction
  function automatic int typ(input int f);typ=(f==0 || f==3)?1:7;endfunction
  function automatic int version(input int f);version=f<3?77:(f==3?78:79);endfunction
  function automatic logic [7:0] expected(input int f,input int i);
    int payload,record_size,record_no,pos;
    record_size=typ(f)==7?24:12;payload=count(f)*record_size;
    case(i)
      0:expected=8'h4d;1:expected=8'h43;2:expected=8'h41;3:expected=8'h32;
      4:expected=2;5:expected=typ(f);6,7:expected=0;
      8,9,10,11:expected=(f>>(8*(11-i)))&255;
      12,13,14,15:expected=(version(f)>>(8*(15-i)))&255;
      16:expected=0;17:expected=count(f);18:expected=payload>>8;19:expected=payload&255;
      default:begin record_no=first(f)+(i-20)/record_size;pos=(i-20)%record_size;
        expected=typ(f)==7 ? words[record_no][191-pos*8 -:8] : words[record_no][95-pos*8 -:8];end
    endcase
  endfunction
  always @(negedge clk) if(rst_n)begin
    if(dut.stg_out_of_range)$fatal(1,"staging overflow");
    if(en)begin
      if(frame>=5)$fatal(1,"extra packet");
      if(byte_index>=50 && byte_index<50+20+count(frame)*(typ(frame)==7?24:12))
        if(gmii!==expected(frame,byte_index-50))
          $fatal(1,"frame %d byte %d got %h expected %h",frame,byte_index-50,gmii,expected(frame,byte_index-50));
      byte_index++;
    end else if(byte_index!=0)begin
      if(byte_index!=50+20+count(frame)*(typ(frame)==7?24:12)+4)$fatal(1,"frame length");
      byte_index=0;frame++;
      if(frame==5)begin
        if(read_index!=152)$fatal(1,"FIFO loss or duplication");
        $display("tb_mca_throughput PASS: 64 channel, 40 PSD, real UDP, timeout, mixed type/version and pending tail");$finish;
      end
    end
  end
  initial begin
    for(int i=0;i<152;i++)begin
      words[i]=0;
      words[i][199:192]=(i<64 || (i>=144 && i<147))?1:7;
      words[i][231:200]=i<144?77:(i<147?78:79);
      for(int j=0;j<24;j++)words[i][191-j*8 -:8]=(i+j)&255;
    end
    repeat(5)@(negedge clk);rst_n=1;
  end
  initial begin #1000000;$fatal(1,"throughput timeout");end
endmodule
