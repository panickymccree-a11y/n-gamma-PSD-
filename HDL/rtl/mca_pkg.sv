`timescale 1ns/1ps

// Shared protocol and numeric constants for the single-channel MCA.
//
// The MCA2/MCF2 wire formats implemented here are the ones the 32-channel
// host already speaks (see docs/protocol.md):
//
//   MCF2 control request  : 20 bytes, magic 0x4D434632, version 2
//   MCF2 control response : 16-byte response record carried in MCA2 type 5
//   MCA2 data packet      : 20-byte common header + records
package mca_pkg;
  // ---------------------------------------------------------------------
  // Sizes
  // ---------------------------------------------------------------------
  localparam int ADC_WIDTH       = 14;
  localparam int ADC_MAX         = 16383;
  localparam int EVENT_WIDTH     = 96;
  localparam int PSD_EVENT_BYTES = 24;
  localparam int FIFO_EVENT_WIDTH = 232;
  localparam int TIMESTAMP_WIDTH = 48;

  localparam int CONTROL_FRAME_BYTES  = 20;
  localparam int CONTROL_FRAME_BITS   = CONTROL_FRAME_BYTES * 8;
  localparam int CONTROL_RESP_BYTES   = 16;
  localparam int PACKET_HEADER_BYTES  = 20;
  localparam int PACKET_HEADER_BITS   = PACKET_HEADER_BYTES * 8;
  localparam int CHANNEL_EVENT_BYTES  = 12;
  // Fixed ADC-front-end latency used by bypass and filtered data paths.  The
  // FIR has 13 samples of linear-phase group delay plus registered arithmetic;
  // both bypass and filtered paths are aligned to this 20-clock boundary.
  localparam int FRONTEND_PIPELINE_LATENCY = 20;

  // ---------------------------------------------------------------------
  // Magic numbers.  MCF2_MAGIC is kept as an 8-bit view for the legacy
  // single-byte comparisons and as a 32-bit value for the 20-byte frame.
  // ---------------------------------------------------------------------
  localparam logic [31:0] MCF2_MAGIC32 = 32'h4D43_4632; // "MCF2"
  localparam logic [31:0] MCA2_MAGIC32 = 32'h4D43_4132; // "MCA2"
  localparam logic [7:0]  MCF2_MAGIC   = 8'h4D;         // "M"
  localparam logic [7:0]  MCA2_TYPE_VERSION = 8'h02;

  // ---------------------------------------------------------------------
  // MCF2 opcodes (host ControlOpcode)
  // ---------------------------------------------------------------------
  typedef enum logic [7:0] {
    OP_WRITE_SHADOW  = 8'h01,
    OP_READ          = 8'h02,
    OP_COMMIT_ALL    = 8'h03,
    OP_START         = 8'h04,
    OP_STOP          = 8'h05,
    OP_ABORT         = 8'h06,
    OP_CLEAR_COUNTERS= 8'h07,
    // Legacy single-channel opcodes.  They remain accepted so that older
    // host builds keep working, and they never collide with the values above.
    OP_READ_CONFIG   = 8'h21,
    OP_WRITE_CONFIG  = 8'h22,
    OP_COMMIT        = 8'h23,
    OP_CLEAR         = 8'h27,
    OP_READ_STATUS   = 8'h28,
    OP_SET_RANGE     = 8'h29
  } mcf2_opcode_t;

  // ---------------------------------------------------------------------
  // MCF2 targets
  // ---------------------------------------------------------------------
  typedef enum logic [7:0] {
    TARGET_GLOBAL  = 8'h00,
    TARGET_ADC     = 8'h01,
    TARGET_CHANNEL = 8'h02,
    TARGET_MODULE  = 8'h03,
    TARGET_PAIR    = 8'h04
  } mcf2_target_t;

  // ---------------------------------------------------------------------
  // MCF2 status codes (host ControlStatus)
  // ---------------------------------------------------------------------
  typedef enum logic [7:0] {
    RESP_OK                  = 8'h00,
    RESP_BAD_FRAME           = 8'h01,
    RESP_BAD_OPCODE          = 8'h02,
    RESP_BAD_TARGET          = 8'h03,
    RESP_BAD_VALUE           = 8'h04,
    RESP_BUSY                = 8'h05,
    RESP_NOT_READY           = 8'h06,
    RESP_VALIDATION_FAILED   = 8'h07,
    RESP_CONFIG_VERSION_MISMATCH = 8'h08
  } mcf2_status_t;

  // ---------------------------------------------------------------------
  // MCA2 packet types
  // ---------------------------------------------------------------------
  typedef enum logic [7:0] {
    PKT_CHANNEL_EVENTS = 8'h01,
    PKT_MODULE_EVENTS  = 8'h02,
    PKT_COINC_EVENTS   = 8'h03,
    PKT_STATUS         = 8'h04,
    PKT_CONTROL_RESP   = 8'h05,
    PKT_WAVEFORM       = 8'h06,
    PKT_PSD_EVENTS     = 8'h07
  } mca2_packet_t;

  // ---------------------------------------------------------------------
  // Global register map (target 0)
  // ---------------------------------------------------------------------
  localparam logic [15:0] REG_GLOBAL_MODE              = 16'h0000;
  localparam logic [15:0] REG_GLOBAL_BASELINE_TRACKING = 16'h0001;
  localparam logic [15:0] REG_GLOBAL_INTEGRATION_NS    = 16'h0002;
  localparam logic [15:0] REG_GLOBAL_MANUAL_BASELINE   = 16'h0003;
  localparam logic [15:0] REG_GLOBAL_BASELINE_AUTO     = 16'h0004;
  localparam logic [15:0] REG_GLOBAL_RANGE             = 16'h0005; // 1=Q1, 2=Q2
  localparam logic [15:0] REG_GLOBAL_ABORT             = 16'h0006;
  localparam logic [15:0] REG_GLOBAL_MANUAL_BASELINE14 = 16'h0007;
  // Waveform-mode shadow registers.  They are staged and committed through the
  // same shadow/COMMIT mechanism as the acquisition configuration.
  localparam logic [15:0] REG_GLOBAL_WAVE_TRIGGER_MODE = 16'h0008;
  localparam logic [15:0] REG_GLOBAL_WAVE_SESSION      = 16'h0009;
  localparam logic [15:0] REG_GLOBAL_BANDWIDTH_ENABLE  = 16'h000A;
  localparam logic [15:0] REG_GLOBAL_EXT_DURATION_US   = 16'h000B;
  localparam logic [15:0] REG_GLOBAL_PEAK_INTERPOLATION = 16'h000C;
  // Read-only, coherent front-end status snapshot.
  localparam logic [15:0] REG_FRONTEND_STATUS          = 16'h0123;

  // Capability registers read back by the host during its capability scan.
  localparam logic [15:0] REG_CAP_ADC_COUNT      = 16'h0100;
  localparam logic [15:0] REG_CAP_MAX_ADC_COUNT  = 16'h0101;
  localparam logic [15:0] REG_CAP_CH_COUNT       = 16'h0102;
  localparam logic [15:0] REG_CAP_MAX_CH_COUNT   = 16'h0103;
  localparam logic [15:0] REG_CAP_MODULE_COUNT   = 16'h0104;
  localparam logic [15:0] REG_CAP_MAX_MODULE     = 16'h0105;
  localparam logic [15:0] REG_CAP_PAIR_COUNT     = 16'h0106;
  localparam logic [15:0] REG_CAP_SAMPLE_RATE    = 16'h0107;
  localparam logic [15:0] REG_CAP_FIRMWARE       = 16'h0108;
  localparam logic [15:0] REG_CAP_CONFIG_VERSION = 16'h0109;
  // Format capability registers.  A device that does not answer these reads
  // returns RESP_BAD_VALUE, and the host keeps its legacy 12-bit/Q12.4 view.
  localparam logic [15:0] REG_CAP_FORMAT_FLAGS   = 16'h010A;
  localparam logic [15:0] REG_CAP_ADC_BITS       = 16'h010B;
  localparam logic [15:0] REG_CAP_PEAK_Q_BITS    = 16'h010C;
  localparam logic [15:0] REG_CAP_INTEGRAL_BITS  = 16'h010D;
  localparam logic [15:0] REG_CAP_INTEGRAL_WINDOW_MIN_NS = 16'h010E;
  localparam logic [15:0] REG_CAP_INTEGRAL_WINDOW_MAX_NS = 16'h010F;
  localparam logic [15:0] REG_CAP_TIMEBASE_PS    = 16'h0110;
  // Read-only waveform status.  It is sampled from a consistent DCO snapshot,
  // never from the raw asynchronous status bus.
  localparam logic [15:0] REG_WAVE_STATUS        = 16'h0120;
  localparam logic [15:0] REG_WAVE_LAST_CAPTURE  = 16'h0121;
  localparam logic [15:0] REG_WAVE_CAPTURE_COUNT = 16'h0122;
  localparam logic [15:0] REG_EXT_STATUS         = 16'h0124;
  localparam logic [15:0] REG_EXT_TRIGGER_COUNT  = 16'h0125;

  // Format flag bits reported through REG_CAP_FORMAT_FLAGS.
  localparam logic [31:0] FMT_FLAG_EXTENDED_QUERY  = 32'h0000_0001;
  localparam logic [31:0] FMT_FLAG_MANUAL_BASELINE = 32'h0000_0002;
  localparam logic [31:0] FMT_FLAG_EXTENDED_STATUS = 32'h0000_0004;
  localparam logic [31:0] FMT_FLAG_PEAK_ABSOLUTE   = 32'h0000_0008;
  localparam logic [31:0] FMT_FLAG_BASELINE_TLV16  = 32'h0000_0010;
  localparam logic [31:0] FMT_FLAG_WAVEFORM        = 32'h0000_0020;
  localparam logic [31:0] FMT_FLAG_EXTERNAL_TRIGGER = 32'h0000_0100;
  localparam logic [31:0] FMT_FLAG_PEAK_INTERPOLATION = 32'h0000_0200;

  // ---------------------------------------------------------------------
  // Acquisition modes.  The waveform mode keeps the historical numbering for
  // the spectrum modes so old host builds keep working.
  // ---------------------------------------------------------------------
  localparam logic [31:0] MODE_SPECTRUM = 32'd0;
  localparam logic [31:0] MODE_WAVEFORM = 32'd3;
  localparam logic [31:0] MODE_EXTERNAL = 32'd4;
  localparam logic [31:0] MODE_PSD = 32'd5;
  localparam logic [31:0] FMT_FLAG_PSD = 32'h0000_0400;
  localparam logic [15:0] REG_CAP_PSD_MAX_NS = 16'h0111;
  localparam logic [15:0] REG_CH_PSD_START = 16'h001B;
  localparam logic [15:0] REG_CH_PSD_END = 16'h001C;

  // ---------------------------------------------------------------------
  // Fixed waveform capture geometry (see docs/protocol.md).
  // ---------------------------------------------------------------------
  localparam int WAVE_SAMPLE_WIDTH   = 16;      // 14-bit code + overrange + pad
  localparam int WAVE_PRE_SAMPLES    = 3750;
  localparam int WAVE_TOTAL_SAMPLES  = 12500;
  localparam int WAVE_POST_SAMPLES   = WAVE_TOTAL_SAMPLES - WAVE_PRE_SAMPLES;
  localparam int WAVE_MEM_DEPTH      = 16384;   // power of two ring buffer
  localparam int WAVE_MEM_AW         = 14;
  localparam int WAVE_SAMPLE_NS      = 8;
  localparam int WAVE_FRAGMENT_MAX   = 512;     // samples per UDP fragment
  localparam int WAVE_FRAGMENT_COUNT = 25;      // 24 x 512 + 212
  localparam int WAVE_LAST_SAMPLES   = WAVE_TOTAL_SAMPLES
                                       - (WAVE_FRAGMENT_COUNT - 1)
                                         * WAVE_FRAGMENT_MAX; // 212
  localparam int WAVE_FRAGMENT_HEADER_BYTES = 4 + 32;  // session id + fragment hdr
  localparam int WAVE_PAYLOAD_MAX_BYTES = WAVE_FRAGMENT_HEADER_BYTES
                                          + WAVE_FRAGMENT_MAX * 2; // 1060

  // Waveform ST_WAVE_* state codes reported through REG_WAVE_STATUS.
  localparam logic [31:0] WAVE_STATE_STOPPED   = 32'd0;
  localparam logic [31:0] WAVE_STATE_FILLING   = 32'd1;
  localparam logic [31:0] WAVE_STATE_WAITING   = 32'd2;
  localparam logic [31:0] WAVE_STATE_CAPTURING = 32'd3;
  localparam logic [31:0] WAVE_STATE_UPLOADING = 32'd4;
  localparam logic [31:0] WAVE_STATE_SINGLE_DONE = 32'd5;

  // ---------------------------------------------------------------------
  // Channel register map (target 2, targetId = channel)
  // The low addresses keep the historical host register numbers; the upper
  // addresses carry the 14-bit-capable single-channel format.
  // ---------------------------------------------------------------------
  localparam logic [15:0] REG_CH_CONTROL        = 16'h0000;
  localparam logic [15:0] REG_CH_THRESHOLD      = 16'h0001; // 12-bit legacy
  localparam logic [15:0] REG_CH_RETURN         = 16'h0002;
  localparam logic [15:0] REG_CH_GUARD          = 16'h0003;
  localparam logic [15:0] REG_CH_BASE_SHIFT     = 16'h0005;
  localparam logic [15:0] REG_CH_WINDOW         = 16'h0008; // pre | post<<16
  localparam logic [15:0] REG_CH_BASE_PULSE_THR = 16'h000A;
  localparam logic [15:0] REG_CH_BASE_HOLD      = 16'h000B;
  localparam logic [15:0] REG_CH_THRESHOLD14    = 16'h0010;
  localparam logic [15:0] REG_CH_RETURN14       = 16'h0011;
  localparam logic [15:0] REG_CH_GUARD14        = 16'h0012;
  localparam logic [15:0] REG_CH_PULSE_THR14    = 16'h0013;
  localparam logic [15:0] REG_CH_WINDOW_TOTAL   = 16'h0014;
  localparam logic [15:0] REG_CH_WINDOW_PRE     = 16'h0015;
  localparam logic [15:0] REG_CH_FAST_SHIFT     = 16'h0016;
  localparam logic [15:0] REG_CH_FAST_SAMPLES   = 16'h0017;
  localparam logic [15:0] REG_CH_RECOVERY_SAMPLES = 16'h0018;
  localparam logic [15:0] REG_CH_OFFSET_QUALIFY = 16'h0019;
  localparam logic [15:0] REG_CH_RECOVERY_TOLERANCE = 16'h001A;

  // ---------------------------------------------------------------------
  // Status TLV types.  Types 1-8 are the 32-channel host definitions and
  // keep their exact layout; the single-channel device reuses them for one
  // channel and adds the extended types below.
  // ---------------------------------------------------------------------
  localparam logic [7:0] TLV_CAPABILITY      = 8'd1;
  localparam logic [7:0] TLV_EPOCH           = 8'd2;
  localparam logic [7:0] TLV_COUNTERS        = 8'd3;
  localparam logic [7:0] TLV_FLOW            = 8'd4;
  localparam logic [7:0] TLV_BASELINE_0      = 8'd5; // channels 0..7, 12-bit pairs
  localparam logic [7:0] TLV_BASELINE_1      = 8'd6;
  localparam logic [7:0] TLV_BASELINE_2      = 8'd7;
  localparam logic [7:0] TLV_BASELINE_3      = 8'd8;
  localparam logic [7:0] TLV_BASELINE_STATE  = 8'd9; // 2 bits per channel
  localparam logic [7:0] TLV_MANUAL_BASELINE = 8'd10;
  localparam logic [7:0] TLV_EXTRA_COUNTERS  = 8'd11;
  localparam logic [7:0] TLV_BASELINE16      = 8'd12; // full 14-bit baseline
  localparam logic [7:0] TLV_FRONTEND        = 8'd13;
  localparam logic [7:0] TLV_END             = 8'd0;

  localparam int TLV_MAX_BYTES = 134;
  localparam logic [31:0] FMT_FLAG_FRONTEND_STATE = 32'h0000_0040;
  localparam logic [31:0] FMT_FLAG_RECOVERY_TOLERANCE = 32'h0000_0080;

  // ---------------------------------------------------------------------
  // Event flag bits, matching the host ChannelEvent decoder:
  //   bit0 polarity (1 = negative pulse)
  //   bit1 overflow (overrange, peak saturation or integral saturation)
  //   bit2 pileup
  //   bit7:3 channel number
  // ---------------------------------------------------------------------
  localparam logic [7:0] EVT_POLARITY   = 8'h01;
  localparam logic [7:0] EVT_OVERFLOW   = 8'h02;
  localparam logic [7:0] EVT_PILEUP     = 8'h04;

  function automatic logic [7:0] event_flags(input logic negative,
                                             input logic overflow,
                                             input logic pileup,
                                             input logic [4:0] channel);
    event_flags = (negative ? EVT_POLARITY : 8'h00) |
                  (overflow ? EVT_OVERFLOW : 8'h00) |
                  (pileup   ? EVT_PILEUP   : 8'h00) |
                  {channel, 3'b000};
  endfunction

  // ---------------------------------------------------------------------
  // Numeric helpers
  // ---------------------------------------------------------------------
  function automatic logic [13:0] clamp_adc(input logic signed [17:0] v);
    if (v < 0)
      clamp_adc = 14'd0;
    else if (v > 18'sd16383)
      clamp_adc = 14'd16383;
    else
      clamp_adc = v[13:0];
  endfunction

  function automatic logic [23:0] saturate_u24(input logic [31:0] v);
    if (|v[31:24])
      saturate_u24 = 24'hFFFFFF;
    else
      saturate_u24 = v[23:0];
  endfunction

  // Peak is transmitted as a Q14.2 value: the 14-bit code times four.
  function automatic logic [15:0] adc_to_q2(input logic [13:0] adc_code);
    adc_to_q2 = {adc_code, 2'b00};
  endfunction

  function automatic logic [13:0] q2_to_adc(input logic [15:0] q2);
    q2_to_adc = (q2[15:14] != 2'b00) ? 14'd16383 : q2[13:0];
  endfunction

  // ---------------------------------------------------------------------
  // Shared polarity-aware trigger arithmetic.
  //
  // Both the spectrum discriminator and the waveform discriminator use
  // exactly these two functions so the two modes can never disagree about
  // what a trigger is.  The difference is computed in at least 15 signed
  // bits, so a 14-bit unsigned subtraction can never wrap around.
  // ---------------------------------------------------------------------
  localparam int POLARITY_DIFF_WIDTH = 15;

  function automatic logic signed [POLARITY_DIFF_WIDTH-1:0] polarity_delta(
      input logic [ADC_WIDTH-1:0] sample,
      input logic [ADC_WIDTH-1:0] baseline,
      input logic                 negative);
    logic signed [POLARITY_DIFF_WIDTH-1:0] up;
    logic signed [POLARITY_DIFF_WIDTH-1:0] down;
    begin
      up   = $signed({1'b0, sample}) - $signed({1'b0, baseline});
      down = $signed({1'b0, baseline}) - $signed({1'b0, sample});
      polarity_delta = negative ? down : up;
    end
  endfunction

  // Signed magnitude of polarity_delta, widened by one bit so the negation of
  // the most negative value cannot overflow.
  function automatic logic [POLARITY_DIFF_WIDTH:0] polarity_magnitude(
      input logic signed [POLARITY_DIFF_WIDTH-1:0] delta);
    logic signed [POLARITY_DIFF_WIDTH:0] wide;
    begin
      wide = {delta[POLARITY_DIFF_WIDTH-1], delta};
      polarity_magnitude = wide[POLARITY_DIFF_WIDTH]
                           ? $unsigned(-wide) : $unsigned(wide);
    end
  endfunction

  // Trigger test: the selected-polarity deviation is at or above the
  // threshold H measured from the baseline B, exactly as documented.
  function automatic logic polarity_above_threshold(
      input logic [ADC_WIDTH-1:0] sample,
      input logic [ADC_WIDTH-1:0] baseline,
      input logic [ADC_WIDTH-1:0] threshold,
      input logic                 negative);
    logic signed [POLARITY_DIFF_WIDTH-1:0] delta;
    logic [POLARITY_DIFF_WIDTH:0] magnitude;
    begin
      delta = polarity_delta(sample, baseline, negative);
      magnitude = polarity_magnitude(delta);
      polarity_above_threshold = !delta[POLARITY_DIFF_WIDTH-1] &&
                                 (magnitude >= {1'b0, threshold});
    end
  endfunction
endpackage
