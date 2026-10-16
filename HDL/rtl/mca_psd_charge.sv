`timescale 1ns/1ps
// Two independent one-sample/cycle lanes. No subtract-to-accumulator cone.
module mca_psd_charge (
  input logic clk, rst_n, flush, start, finish,
  input logic [13:0] baseline,
  input logic negative,
  input logic [1:0] sample_valid, sample_tail, sample_overrange,
  input logic [13:0] sample_pre, sample_post,
  output logic busy, result_valid,
  output logic signed [31:0] qtotal, qtail,
  output logic [7:0] quality
);
  logic [13:0] base_q;
  logic neg_q;
  logic [1:0] v0,v1,v2,tail0,tail1,ov0,ov1,ov2;
  logic signed [14:0] a0[2],b0[2],delta1[2],total2[2],tail2[2];
  logic signed [25:0] total_acc[2],tail_acc[2];
  logic signed [26:0] total_sum,tail_sum;
  logic over_q, finish_q;
  logic [1:0] final_stage;
  wire drained = !(|v0) && !(|v1) && !(|v2);
  always_ff @(posedge clk) begin
    if (!rst_n || flush) begin
      base_q<=0; neg_q<=0; v0<=0; v1<=0; v2<=0;
      tail0<=0; tail1<=0; ov0<=0; ov1<=0; ov2<=0;
      over_q<=0; finish_q<=0; final_stage<=0;
      busy<=0; result_valid<=0; qtotal<=0; qtail<=0; quality<=0;
      total_sum<=0; tail_sum<=0;
      for (int j=0;j<2;j=j+1) begin
        a0[j]<=0; b0[j]<=0; delta1[j]<=0; total2[j]<=0; tail2[j]<=0;
        total_acc[j]<=0; tail_acc[j]<=0;
      end
    end else begin
      result_valid<=0;
      v0<=sample_valid; tail0<=sample_tail; ov0<=sample_overrange;
      v1<=v0; tail1<=tail0; ov1<=ov0;
      v2<=v1; ov2<=ov1;
      for (int j=0;j<2;j=j+1) begin
        // Stage 0: polarity mux only. The trigger sample uses the new baseline.
        if (start ? negative : neg_q) begin
          a0[j]<=$signed({1'b0,start ? baseline : base_q});
          b0[j]<=$signed({1'b0,j==0 ? sample_pre : sample_post});
        end else begin
          a0[j]<=$signed({1'b0,j==0 ? sample_pre : sample_post});
          b0[j]<=$signed({1'b0,start ? baseline : base_q});
        end
        delta1[j]<=a0[j]-b0[j];                 // Stage 1: one subtract
        total2[j]<=delta1[j];
        tail2[j]<=tail1[j] ? delta1[j] : 15'sd0; // Stage 2: gate mux
        if (v2[j]) begin                        // Stage 3: one carry chain
          total_acc[j]<=total_acc[j]+{{11{total2[j][14]}},total2[j]};
          tail_acc[j]<=tail_acc[j]+{{11{tail2[j][14]}},tail2[j]};
        end
      end
      if (|(v2 & ov2)) over_q<=1;
      if (start) begin
        base_q<=baseline; neg_q<=negative; busy<=1;
        finish_q<=0; final_stage<=0; over_q<=0;
        for (int j=0;j<2;j=j+1) begin total_acc[j]<=0; tail_acc[j]<=0; end
      end
      if (finish) finish_q<=1;
      if (finish_q && drained && final_stage==0) begin
        total_sum<=$signed({total_acc[0][25],total_acc[0]})+
                   $signed({total_acc[1][25],total_acc[1]});
        tail_sum<=$signed({tail_acc[0][25],tail_acc[0]})+
                  $signed({tail_acc[1][25],tail_acc[1]});
        final_stage<=1;
      end else if (final_stage==1) begin
        qtotal<={{5{total_sum[26]}},total_sum};
        qtail<={{5{tail_sum[26]}},tail_sum};
        quality<={6'd0,((total_sum[26]!=total_sum[25]) ||
                       (tail_sum[26]!=tail_sum[25])),over_q};
        final_stage<=2;
      end else if (final_stage==2) begin
        result_valid<=1; finish_q<=0; final_stage<=0; busy<=0;
      end
    end
  end
endmodule
