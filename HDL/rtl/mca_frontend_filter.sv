`timescale 1ns/1ps

// 27-tap, linear-phase FIR front end for the 125 MS/s ADC stream.
//
// The ADC is offset-binary.  The fixed 8192 subtraction below is only a
// numeric recentering for signed arithmetic; it is deliberately independent
// of the automatic/manual pulse baseline.  The latter is estimated after this
// front end in mca_baseline.
//
// The symmetric filter uses 13 pre-adds plus the centre tap and therefore
// maps to 14 DSP48 multipliers.  Products and a balanced adder tree are
// registered at every level.  No intermediate saturation is performed.
module mca_frontend_filter #(
  parameter int ADC_WIDTH = 14
) (
  input  logic clk,
  input  logic rst_n,
  input  logic filter_enable,
  input  logic clear_history,
  input  logic sample_valid,
  input  logic [ADC_WIDTH-1:0] sample,
  input  logic raw_overrange,
  input  logic [47:0] timestamp,
  output logic output_valid,
  output logic [ADC_WIDTH-1:0] output_sample,
  output logic output_overrange,
  output logic [47:0] output_timestamp
);
  localparam int COEF_WIDTH = 18;
  localparam int COEF_SHIFT = 17;
  localparam int PAIR_COUNT = 13;
  localparam int ACC_WIDTH = 38;
  localparam int EXTRA_DELAY = 13;
  localparam int WARMUP_SAMPLES = 13;

  // Q1.17 coefficients.  The integer sum is exactly 2^17.
  localparam logic signed [COEF_WIDTH-1:0] COEF [0:PAIR_COUNT] = '{
    18'sd142, 18'sd299, -18'sd79, -18'sd965, -18'sd1054,
    18'sd825, 18'sd3164, 18'sd2126, -18'sd3594, -18'sd8418,
    -18'sd3100, 18'sd15315, 18'sd37299, 18'sd47152
  };

  localparam logic signed [14:0] ADC_CENTER = 15'sd8192;
  localparam logic signed [37:0] ROUND_HALF = 38'sd65536;

  logic signed [14:0] history_q [0:25];
  logic               overrange_history_q [0:25];
  logic               initialized_q;
  logic [4:0]         warm_count_q;

  logic signed [15:0] pair_comb [0:12];
  logic signed [14:0] center_comb;
  logic               window_overrange_comb;
  logic signed [15:0] pair_q [0:12];
  logic signed [14:0] center_q;
  logic               pair_valid_q;
  logic               pair_overrange_q;

  logic signed [33:0] product_q [0:13];
  logic               product_valid_q;
  logic               product_overrange_q;

  // The coefficient bounds make every partial sum fit these widths without
  // truncation: the final tree4 register remains the required 38-bit
  // accumulator.
  logic signed [30:0] tree1_q [0:6];
  logic               tree1_valid_q;
  logic               tree1_overrange_q;
  logic signed [31:0] tree2_q [0:3];
  logic               tree2_valid_q;
  logic               tree2_overrange_q;
  logic signed [31:0] tree3_q [0:1];
  logic               tree3_valid_q;
  logic               tree3_overrange_q;
  logic signed [37:0] tree4_q;
  logic               tree4_valid_q;
  logic               tree4_overrange_q;

  logic signed [37:0] rounded_q;
  logic               rounded_valid_q;
  logic               rounded_overrange_q;
  logic [ADC_WIDTH-1:0] saturated_q;
  logic               saturated_flag_q;
  logic               saturated_valid_q;
  logic               saturated_overrange_q;

  (* shreg_extract = "yes" *)
  logic [ADC_WIDTH-1:0] raw_sample_q [0:mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
  (* shreg_extract = "yes" *)
  logic                 raw_valid_q [0:mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
  logic                 raw_overrange_q [0:mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
  (* shreg_extract = "yes" *)
  logic [47:0]          raw_timestamp_q [0:mca_pkg::FRONTEND_PIPELINE_LATENCY-1];

  (* shreg_extract = "yes" *)
  logic [ADC_WIDTH-1:0] fir_sample_delay_q [0:EXTRA_DELAY-1];
  (* shreg_extract = "yes" *)
  logic                 fir_valid_delay_q [0:EXTRA_DELAY-1];
  (* shreg_extract = "yes" *)
  logic                 fir_overrange_delay_q [0:EXTRA_DELAY-1];
  (* shreg_extract = "yes" *)
  logic                 fir_saturated_delay_q [0:EXTRA_DELAY-1];

  logic signed [14:0] current_signed;
  logic [5:0] pipeline_age_q;
  integer i;
  integer j;

  assign current_signed = $signed({1'b0, sample}) - ADC_CENTER;

  always_comb begin
    // Pair tap k combines samples n-k and n-(26-k).  The current sample is
    // the newest member of pair 0; the registered history therefore starts
    // at history[0] for pair 1.
    for (j = 1; j < PAIR_COUNT; j = j + 1)
      pair_comb[j] = history_q[j-1] + history_q[25-j];
    // The current input is newest; history_q[0] is n-1 and history_q[25] is
    // n-26.  The centre of the 27-point window is n-13.
    pair_comb[0] = current_signed + history_q[25];
    center_comb = history_q[12];
    window_overrange_comb = raw_overrange;
    for (j = 0; j < 26; j = j + 1)
      window_overrange_comb = window_overrange_comb | overrange_history_q[j];
  end

  function automatic logic signed [37:0] sign_extend_product(
      input logic signed [33:0] value);
    sign_extend_product = {{4{value[33]}}, value};
  endfunction

  function automatic logic [ADC_WIDTH-1:0] saturate_code(
      input logic signed [37:0] value,
      output logic saturated);
    logic signed [38:0] restored;
    begin
      restored = value + 39'sd8192;
      if (restored[38]) begin
        saturate_code = '0;
        saturated = 1'b1;
      end else if (|restored[38:14]) begin
        saturate_code = {ADC_WIDTH{1'b1}};
        saturated = 1'b1;
      end else begin
        saturate_code = restored[ADC_WIDTH-1:0];
        saturated = 1'b0;
      end
    end
  endfunction

  always_comb begin
    // Symmetric round-to-nearest, ties away from zero.
    if (tree4_q >= 0)
      rounded_q = (tree4_q + ROUND_HALF) >>> COEF_SHIFT;
    else
      rounded_q = (tree4_q - ROUND_HALF) >>> COEF_SHIFT;
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n || clear_history) begin
      initialized_q <= 1'b0;
      pipeline_age_q <= '0;
      warm_count_q <= '0;
      pair_valid_q <= 1'b0;
      pair_overrange_q <= 1'b0;
      product_valid_q <= 1'b0;
      product_overrange_q <= 1'b0;
      tree1_valid_q <= 1'b0;
      tree1_overrange_q <= 1'b0;
      tree2_valid_q <= 1'b0;
      tree2_overrange_q <= 1'b0;
      tree3_valid_q <= 1'b0;
      tree3_overrange_q <= 1'b0;
      tree4_valid_q <= 1'b0;
      tree4_overrange_q <= 1'b0;
      rounded_valid_q <= 1'b0;
      rounded_overrange_q <= 1'b0;
      saturated_valid_q <= 1'b0;
      saturated_flag_q <= 1'b0;
      saturated_overrange_q <= 1'b0;
      for (i = 0; i < 26; i = i + 1) begin
        history_q[i] <= '0;
        overrange_history_q[i] <= 1'b0;
      end
      for (i = 0; i < PAIR_COUNT; i = i + 1)
        pair_q[i] <= '0;
      center_q <= '0;
      for (i = 0; i < 14; i = i + 1)
        product_q[i] <= '0;
      for (i = 0; i < 7; i = i + 1)
        tree1_q[i] <= '0;
      for (i = 0; i < 4; i = i + 1)
        tree2_q[i] <= '0;
      for (i = 0; i < 2; i = i + 1)
        tree3_q[i] <= '0;
      tree4_q <= '0;
      saturated_q <= '0;
    end else begin
      if (pipeline_age_q < 6'd20)
        pipeline_age_q <= pipeline_age_q + 1'b1;
      raw_sample_q[0] <= sample;
      raw_valid_q[0] <= sample_valid;
      raw_overrange_q[0] <= raw_overrange;
      raw_timestamp_q[0] <= timestamp;
      for (i = 1; i < mca_pkg::FRONTEND_PIPELINE_LATENCY; i = i + 1) begin
        raw_sample_q[i] <= raw_sample_q[i-1];
        raw_valid_q[i] <= raw_valid_q[i-1];
        raw_overrange_q[i] <= raw_overrange_q[i-1];
        raw_timestamp_q[i] <= raw_timestamp_q[i-1];
      end

      if (sample_valid) begin
        if (!initialized_q) begin
          for (i = 0; i < 26; i = i + 1) begin
            history_q[i] <= current_signed;
            overrange_history_q[i] <= raw_overrange;
          end
          initialized_q <= 1'b1;
          warm_count_q <= 5'd1;
        end else begin
          for (i = 25; i > 0; i = i - 1) begin
            history_q[i] <= history_q[i-1];
            overrange_history_q[i] <= overrange_history_q[i-1];
          end
          history_q[0] <= current_signed;
          overrange_history_q[0] <= raw_overrange;
          if (warm_count_q < WARMUP_SAMPLES[4:0])
            warm_count_q <= warm_count_q + 1'b1;
        end
      end

      pair_valid_q <= sample_valid && filter_enable && initialized_q &&
                      (warm_count_q >= WARMUP_SAMPLES);
      pair_overrange_q <= window_overrange_comb;
      if (sample_valid && filter_enable) begin
        for (i = 0; i < PAIR_COUNT; i = i + 1)
          pair_q[i] <= pair_comb[i];
        center_q <= center_comb;
      end

      product_valid_q <= pair_valid_q;
      product_overrange_q <= pair_overrange_q;
      if (pair_valid_q) begin
        for (i = 0; i < PAIR_COUNT; i = i + 1)
          product_q[i] <= pair_q[i] * COEF[i];
        product_q[13] <= center_q * COEF[13];
      end

      tree1_valid_q <= product_valid_q;
      tree1_overrange_q <= product_overrange_q;
      if (product_valid_q) begin
        tree1_q[0] <= sign_extend_product(product_q[0]) + sign_extend_product(product_q[1]);
        tree1_q[1] <= sign_extend_product(product_q[2]) + sign_extend_product(product_q[3]);
        tree1_q[2] <= sign_extend_product(product_q[4]) + sign_extend_product(product_q[5]);
        tree1_q[3] <= sign_extend_product(product_q[6]) + sign_extend_product(product_q[7]);
        tree1_q[4] <= sign_extend_product(product_q[8]) + sign_extend_product(product_q[9]);
        tree1_q[5] <= sign_extend_product(product_q[10]) + sign_extend_product(product_q[11]);
        tree1_q[6] <= sign_extend_product(product_q[12]) + sign_extend_product(product_q[13]);
      end

      tree2_valid_q <= tree1_valid_q;
      tree2_overrange_q <= tree1_overrange_q;
      if (tree1_valid_q) begin
        tree2_q[0] <= $signed({tree1_q[0][30], tree1_q[0]}) +
                      $signed({tree1_q[1][30], tree1_q[1]});
        tree2_q[1] <= $signed({tree1_q[2][30], tree1_q[2]}) +
                      $signed({tree1_q[3][30], tree1_q[3]});
        tree2_q[2] <= $signed({tree1_q[4][30], tree1_q[4]}) +
                      $signed({tree1_q[5][30], tree1_q[5]});
        tree2_q[3] <= $signed({tree1_q[6][30], tree1_q[6]});
      end

      tree3_valid_q <= tree2_valid_q;
      tree3_overrange_q <= tree2_overrange_q;
      if (tree2_valid_q) begin
        tree3_q[0] <= $signed({tree2_q[0][31], tree2_q[0]}) +
                      $signed({tree2_q[1][31], tree2_q[1]});
        tree3_q[1] <= $signed({tree2_q[2][31], tree2_q[2]}) +
                      $signed({tree2_q[3][31], tree2_q[3]});
      end

      tree4_valid_q <= tree3_valid_q;
      tree4_overrange_q <= tree3_overrange_q;
      if (tree3_valid_q)
        tree4_q <= $signed({{6{tree3_q[0][31]}}, tree3_q[0]}) +
                   $signed({{6{tree3_q[1][31]}}, tree3_q[1]});

      rounded_valid_q <= tree4_valid_q;
      rounded_overrange_q <= tree4_overrange_q;
      saturated_valid_q <= rounded_valid_q;
      saturated_overrange_q <= rounded_overrange_q;
      if (rounded_valid_q)
        saturated_q <= saturate_code(rounded_q, saturated_flag_q);

      fir_sample_delay_q[0] <= saturated_q;
      fir_valid_delay_q[0] <= saturated_valid_q;
      fir_overrange_delay_q[0] <= saturated_overrange_q;
      fir_saturated_delay_q[0] <= saturated_flag_q;
      for (i = 1; i < EXTRA_DELAY; i = i + 1) begin
        fir_sample_delay_q[i] <= fir_sample_delay_q[i-1];
        fir_valid_delay_q[i] <= fir_valid_delay_q[i-1];
        fir_overrange_delay_q[i] <= fir_overrange_delay_q[i-1];
        fir_saturated_delay_q[i] <= fir_saturated_delay_q[i-1];
      end
    end
  end

  always_comb begin
    if (filter_enable) begin
      output_valid = (pipeline_age_q >= 6'd20) &&
                     fir_valid_delay_q[EXTRA_DELAY-1];
      output_sample = fir_sample_delay_q[EXTRA_DELAY-1];
      output_overrange = fir_overrange_delay_q[EXTRA_DELAY-1] |
                         fir_saturated_delay_q[EXTRA_DELAY-1];
    end else begin
      output_valid = (pipeline_age_q >= 6'd20) &&
                     raw_valid_q[mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
      output_sample = raw_sample_q[mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
      output_overrange = raw_overrange_q[mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
    end
    output_timestamp = raw_timestamp_q[mca_pkg::FRONTEND_PIPELINE_LATENCY-1];
  end
endmodule
