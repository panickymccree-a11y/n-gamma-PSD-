`timescale 1ns/1ps

// Local peak/integral spectra for the UART transport.  Both histogram RAMs
// have exactly one procedural writer.  Event read/modify/write and the
// incremental read-and-clear scan are serialized by this state machine.
module mca_spectrum_histogram #(
  parameter int BIN_COUNT = 4096,
  parameter int CHUNK_BINS = 256,
  parameter int CLOCK_HZ = 125_000_000
) (
  input  logic        dco_clk,
  input  logic        host_clk,
  input  logic        rst_n,
  input  logic        run_enable,
  input  logic        clear,
  input  logic        event_valid,
  input  logic [13:0] event_peak,
  input  logic [23:0] event_integral,
  input  logic [31:0] config_version,

  output logic        ready,
  output logic        chunk_toggle,
  input  logic        chunk_ack_toggle,
  output logic [31:0] session_id,
  output logic [31:0] epoch,
  output logic [31:0] chunk_config_version,
  output logic        chunk_kind,       // 0 peak, 1 integral
  output logic [7:0]  chunk_flags,      // bit0 bin sat, bit1 total sat, bit2 final
  output logic [7:0]  bin_shift,
  output logic [15:0] total_bins,
  output logic [15:0] chunk_start,
  output logic [15:0] chunk_count,
  output logic [15:0] chunk_index,
  output logic [15:0] chunk_total,
  output logic [47:0] interval_ticks,
  output logic [31:0] interval_events,
  output logic [31:0] histogram_overflow,
  input  logic [7:0]  chunk_rd_index,
  output logic [31:0] chunk_rd_data
);
  localparam int ADDR_W = $clog2(BIN_COUNT);
  localparam int CHUNKS = BIN_COUNT / CHUNK_BINS;
  localparam int PEAK_SHIFT = 14 - ADDR_W;
  // The UART integral spectrum uses an 8-bit fixed-point bin shift for the
  // production 4096-bin histogram: one bin represents 2^8 = 256 LSB.
  // Keep the peak geometry unchanged (Q14 peak, PEAK_SHIFT = 2).
  localparam int INTEGRAL_SHIFT = 8;

  initial begin
    if ((BIN_COUNT < 1024) || (BIN_COUNT > 8192) ||
        ((BIN_COUNT & (BIN_COUNT-1)) != 0) ||
        ((BIN_COUNT % CHUNK_BINS) != 0))
      $error("BIN_COUNT must be 1024/2048/4096/8192 and divisible by CHUNK_BINS");
  end

  // The chunk staging buffer that backs chunk_rd_data is instantiated further
  // down, after the state machine that fills it.

  // ---------------------------------------------------------------------------
  // Histogram memories: two BIN_COUNT x 32 block RAMs.
  //
  // Both are instantiated explicitly rather than left to inference: a memory
  // with an asynchronous read cannot be mapped to block RAM, and letting Vivado
  // fall back silently produced a 4096x32 LUTRAM that does not fit the device.
  // The XPM simple-dual-port macro gives port A for writes and port B for
  // reads with one cycle of read latency, so each consumer presents its address
  // one clock before it needs the data.
  //
  // MCA_NO_XPM is defined for the behavioural testbench runs, which use the
  // equivalent array model instead.
  // ---------------------------------------------------------------------------
  // Peak and integral events use different address geometries (the peak
  // address is derived from 14 bits while the integral address is derived
  // from 24 bits).  Keep independent read/write addresses so an event can
  // update both histograms at their respective bins in the same cycle.
  logic [ADDR_W-1:0] peak_rd_addr;
  logic [ADDR_W-1:0] integral_rd_addr;
  logic [ADDR_W-1:0] peak_slot_addr;
  logic [ADDR_W-1:0] integral_slot_addr;
  logic [ADDR_W-1:0] scan_rd_addr;
  logic [31:0]       peak_rd_data;
  logic [31:0]       integral_rd_data;

  logic              ram_peak_we;
  logic              ram_integral_we;
  logic [ADDR_W-1:0] peak_ram_addr;
  logic [ADDR_W-1:0] integral_ram_addr;
  logic [31:0]       ram_peak_data;
  logic [31:0]       ram_integral_data;

  // The read port address is a register: it is loaded with the bin that the
  // next consumer needs, one clock before that data is used.
  assign peak_rd_addr = peak_slot_addr;
  assign integral_rd_addr = integral_slot_addr;

`ifdef MCA_NO_XPM
  (* ram_style = "block" *) logic [31:0] peak_mem [0:BIN_COUNT-1];
  (* ram_style = "block" *) logic [31:0] integral_mem [0:BIN_COUNT-1];

  always_ff @(posedge dco_clk) begin
    if (ram_peak_we)
      peak_mem[peak_ram_addr] <= ram_peak_data;
    peak_rd_data <= peak_mem[peak_rd_addr];
  end

  always_ff @(posedge dco_clk) begin
    if (ram_integral_we)
      integral_mem[integral_ram_addr] <= ram_integral_data;
    integral_rd_data <= integral_mem[integral_rd_addr];
  end
`else
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(ADDR_W), .ADDR_WIDTH_B(ADDR_W),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(32),
    .CLOCKING_MODE("common_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("block"),
    .MEMORY_SIZE(BIN_COUNT * 32), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(32), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .USE_EMBEDDED_CONSTRAINT(0),
    .USE_MEM_INIT(0), .WAKEUP_TIME("disable_sleep"),
    .WRITE_DATA_WIDTH_A(32), .WRITE_MODE_B("no_change")
  ) u_peak_ram (
    .dbiterrb(), .doutb(peak_rd_data), .sbiterrb(),
    .addra(peak_ram_addr), .addrb(peak_rd_addr),
    .clka(dco_clk), .clkb(dco_clk), .dina(ram_peak_data),
    .ena(1'b1), .enb(1'b1),
    .injectdbiterra(1'b0), .injectsbiterra(1'b0),
    .regceb(1'b1), .rstb(1'b0), .sleep(1'b0), .wea(ram_peak_we)
  );

  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(ADDR_W), .ADDR_WIDTH_B(ADDR_W),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(32),
    .CLOCKING_MODE("common_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("block"),
    .MEMORY_SIZE(BIN_COUNT * 32), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(32), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .USE_EMBEDDED_CONSTRAINT(0),
    .USE_MEM_INIT(0), .WAKEUP_TIME("disable_sleep"),
    .WRITE_DATA_WIDTH_A(32), .WRITE_MODE_B("no_change")
  ) u_integral_ram (
    .dbiterrb(), .doutb(integral_rd_data), .sbiterrb(),
    .addra(integral_ram_addr), .addrb(integral_rd_addr),
    .clka(dco_clk), .clkb(dco_clk), .dina(ram_integral_data),
    .ena(1'b1), .enb(1'b1),
    .injectdbiterra(1'b0), .injectsbiterra(1'b0),
    .regceb(1'b1), .rstb(1'b0), .sleep(1'b0), .wea(ram_integral_we)
  );
`endif

  typedef enum logic [3:0] {
    ST_CLEAR, ST_IDLE, ST_EVENT_READ, ST_EVENT_WRITE, ST_SCAN_READ,
    ST_SCAN_WRITE, ST_WAIT_ACK, ST_EVENT_CAPTURE, ST_SCAN_CAPTURE
  } state_t;
  state_t state;

  logic [ADDR_W-1:0] clear_addr, scan_addr;
  logic [ADDR_W-1:0] peak_event_addr;
  logic [ADDR_W-1:0] integral_event_addr;
  logic [7:0] scan_offset;
  logic [31:0] peak_old, integral_old, scan_old;
  logic pending_valid;
  logic [13:0] pending_peak;
  logic [23:0] pending_integral;
  logic [26:0] report_timer;
  logic [47:0] ticks_running;
  logic [31:0] events_running;
  logic [47:0] report_ticks;
  logic [31:0] report_events;
  logic bins_saturated, total_saturated;
  logic report_pending, final_pending, previous_run;
  logic event_scan_start;
  logic event_return_wait_ack;
  (* ASYNC_REG = "TRUE" *) logic ack_ff1, ack_ff2;
  logic ack_seen;

  wire [ADDR_W-1:0] peak_address = event_peak[13 -: ADDR_W];
  wire [ADDR_W-1:0] integral_address =
      event_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W];
  wire one_second = report_timer == CLOCK_HZ-1;
  wire closing_interval = run_enable && ready && one_second &&
                          state == ST_IDLE && !report_pending;

  function automatic logic [31:0] sat_inc(input logic [31:0] value);
    sat_inc = (&value) ? value : value + 1'b1;
  endfunction

  always_ff @(posedge dco_clk or negedge rst_n) begin
    if (!rst_n) begin
      state              <= ST_CLEAR;
      clear_addr         <= '0;
      ready              <= 1'b0;
      chunk_toggle       <= 1'b0;
      session_id         <= 32'd0;
      epoch              <= 32'd0;
      chunk_config_version <= 32'd0;
      chunk_kind         <= 1'b0;
      chunk_flags        <= 8'd0;
      bin_shift          <= PEAK_SHIFT;
      total_bins         <= BIN_COUNT;
      chunk_start        <= 16'd0;
      chunk_count        <= CHUNK_BINS;
      chunk_index        <= 16'd0;
      chunk_total        <= CHUNKS;
      interval_ticks     <= 48'd0;
      interval_events    <= 32'd0;
      histogram_overflow <= 32'd0;
      pending_valid      <= 1'b0;
      report_timer       <= 27'd0;
      ticks_running      <= 48'd0;
      events_running     <= 32'd0;
      report_ticks       <= 48'd0;
      report_events      <= 32'd0;
      bins_saturated     <= 1'b0;
      total_saturated    <= 1'b0;
      report_pending     <= 1'b0;
      final_pending      <= 1'b0;
      previous_run       <= 1'b0;
      event_scan_start   <= 1'b0;
      event_return_wait_ack <= 1'b0;
      ack_ff1            <= 1'b0;
      ack_ff2            <= 1'b0;
      ack_seen           <= 1'b0;
      scan_addr          <= '0;
      scan_offset        <= '0;
      peak_event_addr    <= '0;
      integral_event_addr<= '0;
      peak_slot_addr     <= '0;
      integral_slot_addr <= '0;
      scan_rd_addr       <= '0;
    end else begin
      ack_ff1      <= chunk_ack_toggle;
      ack_ff2      <= ack_ff1;
      previous_run <= run_enable;

      if (run_enable && ready) begin
        ticks_running <= ticks_running + 1'b1;
        if (one_second && state == ST_IDLE && !report_pending) begin
          report_timer   <= 27'd0;
          report_pending <= 1'b1;
          report_ticks   <= ticks_running + 1'b1;
          report_events  <= events_running +
                            ((event_valid && events_running != 32'hffff_ffff)
                                 ? 1'b1 : 1'b0);
          ticks_running  <= '0;
          events_running <= '0;
        end else if (one_second) begin
          // Hold the boundary until the preceding ACK-controlled drain has
          // completed.  The extended interval is reported exactly once.
          report_timer <= report_timer;
        end else begin
          report_timer <= report_timer + 1'b1;
        end
      end
      if (previous_run && !run_enable && ready) begin
        report_pending <= 1'b1;
        final_pending  <= 1'b1;
        // If the current scan started on the first event, include all events
        // accepted before STOP in the final interval metadata.
        interval_ticks  <= ticks_running;
        interval_events <= events_running;
        // An event may already have started the incremental scan before STOP
        // arrives.  That scan is the final interval as well, so propagate the
        // final marker immediately instead of waiting for a new report setup
        // cycle (which would be skipped while the scanner is busy).
        chunk_flags[2]  <= 1'b1;
        report_ticks   <= ticks_running;
        report_events  <= events_running;
        ticks_running  <= '0;
        events_running <= '0;
      end

      if (event_valid && ready) begin
        if (!closing_interval) begin
          if (events_running != 32'hffff_ffff)
            events_running <= events_running + 1'b1;
          else
            total_saturated <= 1'b1;
        end
        if (state != ST_IDLE || pending_valid) begin
          if (!pending_valid) begin
            pending_valid    <= 1'b1;
            pending_peak     <= event_peak;
            pending_integral <= event_integral;
          end else begin
            histogram_overflow <= histogram_overflow + 1'b1;
          end
        end
      end

      if (clear) begin
        state          <= ST_CLEAR;
        clear_addr     <= '0;
        ready          <= 1'b0;
        pending_valid  <= 1'b0;
        report_pending <= 1'b0;
        final_pending  <= 1'b0;
        report_timer   <= '0;
        ticks_running  <= '0;
        events_running <= '0;
        bins_saturated <= 1'b0;
        total_saturated<= 1'b0;
        epoch          <= 32'd0;
        session_id     <= session_id + 1'b1;
      end else case (state)
        ST_CLEAR: begin
          if (clear_addr == BIN_COUNT-1) begin
            clear_addr <= '0;
            ready      <= 1'b1;
            state      <= ST_IDLE;
          end else begin
            clear_addr <= clear_addr + 1'b1;
          end
        end

        ST_IDLE: begin
          if (pending_valid || event_valid) begin
            // Present the event bin on the read port.  The explicit
            // ST_EVENT_READ stage below waits for the synchronous RAM output
            // before ST_EVENT_WRITE performs the read/modify/write update.
            // The address is latched in its own register so the scan can keep
            // driving the read port without disturbing this read.
            peak_event_addr <= pending_valid ? pending_peak[13 -: ADDR_W]
                                             : peak_address;
            integral_event_addr <= pending_valid
                ? pending_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W]
                : integral_address;
            peak_slot_addr  <= pending_valid ? pending_peak[13 -: ADDR_W]
                                             : peak_address;
            integral_slot_addr <= pending_valid
                ? pending_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W]
                : integral_address;
            pending_valid  <= 1'b0;
            // An event arriving while the pending entry is consumed takes its
            // place instead of being dropped.
            if (pending_valid && event_valid &&
                !((pending_peak == event_peak) &&
                  (pending_integral == event_integral))) begin
              pending_valid    <= 1'b1;
              pending_peak     <= event_peak;
              pending_integral <= event_integral;
            end
            state <= ST_EVENT_READ;
            event_scan_start <= 1'b1;
            event_return_wait_ack <= 1'b0;
          end else if (report_pending) begin
            report_pending  <= 1'b0;
            epoch           <= epoch + 1'b1;
            chunk_config_version <= config_version;
            interval_ticks  <= report_ticks;
            interval_events <= report_events;
            chunk_kind      <= 1'b0;
            bin_shift       <= PEAK_SHIFT;
            chunk_index     <= 16'd0;
            chunk_start     <= 16'd0;
            scan_addr       <= '0;
            scan_offset     <= '0;
            scan_rd_addr    <= '0;
            peak_slot_addr  <= '0;
            integral_slot_addr <= '0;
            chunk_flags     <= {5'd0, final_pending, total_saturated,
                                bins_saturated};
            state           <= ST_SCAN_READ;
          end
        end

        ST_EVENT_READ: begin
          // Allow the synchronous histogram RAM one full cycle to return the
          // value addressed by ST_IDLE/ST_SCAN_READ.
          state <= ST_EVENT_CAPTURE;
        end

        ST_EVENT_CAPTURE: begin
          // The registered read data for both event addresses is valid on the next
          // cycle; capture it before asserting the event write port.
          peak_old     <= peak_rd_data;
          integral_old <= integral_rd_data;
          peak_slot_addr <= scan_rd_addr;
          integral_slot_addr <= scan_rd_addr;
          state <= ST_EVENT_WRITE;
        end

        ST_EVENT_WRITE: begin
          // peak_old/integral_old now contain the value read for the event
          // bin.  The write-port combinational logic below increments it.
          if ((&peak_old) || (&integral_old))
            bins_saturated <= 1'b1;
          if (event_scan_start) begin
            // An event arriving while idle starts an immediate report scan.
            // Snapshot its counters and reset the scan cursor to the peak
            // spectrum; events that pre-empt an existing scan leave its
            // cursor and kind unchanged.
            event_scan_start <= 1'b0;
            epoch            <= epoch + 1'b1;
            interval_ticks   <= ticks_running;
            interval_events  <= events_running;
            chunk_kind       <= 1'b0;
            bin_shift        <= PEAK_SHIFT;
            chunk_index      <= 16'd0;
            chunk_start      <= 16'd0;
            scan_addr        <= '0;
            scan_offset      <= '0;
            scan_rd_addr     <= '0;
            peak_slot_addr   <= '0;
            integral_slot_addr <= '0;
            chunk_flags      <= {5'd0, final_pending, total_saturated,
                                 bins_saturated};
          end
          if (event_return_wait_ack) begin
            // Events may still update the histogram while a completed staging
            // chunk awaits its UART ACK, but they must not restart the scanner:
            // doing so rewrites the dual-clock staging RAM underneath the
            // transmitter.
            event_return_wait_ack <= 1'b0;
            state <= ST_WAIT_ACK;
          end else begin
            // The scan read address is presented a cycle ahead, because the
            // scan loop can only spare one address cycle per bin.
            peak_slot_addr <= scan_rd_addr;
            integral_slot_addr <= scan_rd_addr;
            state <= ST_SCAN_READ;
          end
        end

        ST_SCAN_READ: begin
          if (pending_valid || event_valid) begin
            // An event overtakes the scan; the scan bin is picked up again from
            // the top of the chunk after the event has been accumulated.
            peak_event_addr <= pending_valid ? pending_peak[13 -: ADDR_W]
                                             : peak_address;
            integral_event_addr <= pending_valid
                ? pending_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W]
                : integral_address;
            peak_slot_addr  <= pending_valid ? pending_peak[13 -: ADDR_W]
                                             : peak_address;
            integral_slot_addr <= pending_valid
                ? pending_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W]
                : integral_address;
            pending_valid  <= 1'b0;
            if (pending_valid && event_valid) begin
              pending_valid    <= 1'b1;
              pending_peak     <= event_peak;
              pending_integral <= event_integral;
            end
            event_return_wait_ack <= 1'b0;
            state <= ST_EVENT_READ;
          end else begin
            // The synchronous RAM output for the address presented by the
            // preceding edge is available on the next edge.  Capture it in a
            // separate stage before writing the chunk buffer.
            state    <= ST_SCAN_CAPTURE;
          end
        end

        ST_SCAN_CAPTURE: begin
          scan_old <= chunk_kind ? integral_rd_data : peak_rd_data;
          state    <= ST_SCAN_WRITE;
        end

        ST_SCAN_WRITE: begin
          // The bin just read is copied into the chunk staging buffer by the
          // dedicated write port below; the same edge also clears it in the
          // histogram RAM through the shared write port.
          if (scan_offset == CHUNK_BINS-1) begin
            chunk_toggle <= ~chunk_toggle;
            state        <= ST_WAIT_ACK;
          end else begin
            scan_offset   <= scan_offset + 1'b1;
            scan_addr     <= scan_addr + 1'b1;
            // Present the next bin one clock before ST_SCAN_READ needs it.
            scan_rd_addr  <= scan_addr + 1'b1;
            peak_slot_addr <= scan_addr + 1'b1;
            integral_slot_addr <= scan_addr + 1'b1;
            state         <= ST_SCAN_READ;
          end
        end

        ST_WAIT_ACK: begin
          if (pending_valid || event_valid) begin
            peak_event_addr <= pending_valid ? pending_peak[13 -: ADDR_W]
                                             : peak_address;
            integral_event_addr <= pending_valid
                ? pending_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W]
                : integral_address;
            peak_slot_addr  <= pending_valid ? pending_peak[13 -: ADDR_W]
                                             : peak_address;
            integral_slot_addr <= pending_valid
                ? pending_integral[INTEGRAL_SHIFT + ADDR_W - 1 -: ADDR_W]
                : integral_address;
            pending_valid  <= 1'b0;
            if (pending_valid && event_valid) begin
              pending_valid    <= 1'b1;
              pending_peak     <= event_peak;
              pending_integral <= event_integral;
            end
            event_return_wait_ack <= 1'b1;
            state <= ST_EVENT_READ;
          end else if (ack_ff2 != ack_seen) begin
            ack_seen <= ack_ff2;
            if (chunk_index == CHUNKS-1) begin
              if (!chunk_kind) begin
                chunk_kind  <= 1'b1;
                bin_shift   <= INTEGRAL_SHIFT;
                chunk_index <= 16'd0;
                chunk_start <= 16'd0;
                scan_addr   <= '0;
                scan_offset <= '0;
                state       <= ST_SCAN_READ;
              end else begin
                bins_saturated  <= 1'b0;
                total_saturated <= 1'b0;
                if (final_pending)
                  report_pending <= 1'b0;
                else if (run_enable && one_second) begin
                  // A one-second boundary can arrive while the previous
                  // event-driven scan is waiting for ACKs.  Fold the held
                  // interval into the report that is just completing instead
                  // of publishing the short snapshot taken at its start.
                  interval_ticks  <= ticks_running;
                  interval_events <= events_running;
                  report_timer    <= 27'd0;
                  ticks_running   <= '0;
                  events_running  <= '0;
                end
                final_pending   <= 1'b0;
                state           <= ST_IDLE;
              end
            end else begin
              chunk_index <= chunk_index + 1'b1;
              chunk_start <= chunk_start + CHUNK_BINS;
              scan_offset <= '0;
              state       <= ST_SCAN_READ;
            end
          end
        end
        default: state <= ST_CLEAR;
      endcase
    end
  end

  // ---------------------------------------------------------------------------
  // Chunk staging buffer.
  //
  // The scan fills this one bin per dco clock; the serial transport reads it
  // back out of the host clock domain.  It is therefore a dual-clock block RAM:
  // port A writes on dco_clk, port B reads on host_clk with one cycle of read
  // latency.  Two problems are solved by making this a registered block RAM
  // rather than a distributed asynchronous one:
  //
  //   * the read no longer sits in the UART frame-byte/CRC-32 path, which was
  //     15 logic levels and most of the device routing span in one cycle;
  //   * the 256 x 32 write data fan-out no longer spreads across the whole
  //     device, which was the worst path in the dco_clk domain.
  //
  // The chunk_toggle/chunk_ack_toggle handshake is what makes the contents safe
  // to sample from the other clock domain: the buffer is only rewritten after
  // the transport has acknowledged the chunk it just read.
  // ---------------------------------------------------------------------------
  localparam int CHUNK_AW = $clog2(CHUNK_BINS);
  wire chunk_write = (state == ST_SCAN_WRITE);

`ifdef MCA_NO_XPM
  (* ram_style = "block" *) logic [31:0] chunk_mem [0:CHUNK_BINS-1];
  always_ff @(posedge dco_clk)
    if (chunk_write) chunk_mem[scan_offset[CHUNK_AW-1:0]] <= scan_old;
  always_ff @(posedge host_clk)
    chunk_rd_data <= chunk_mem[chunk_rd_index[CHUNK_AW-1:0]];
`else
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(CHUNK_AW), .ADDR_WIDTH_B(CHUNK_AW),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(32),
    .CLOCKING_MODE("independent_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("block"),
    .MEMORY_SIZE(CHUNK_BINS * 32), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(32), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .USE_EMBEDDED_CONSTRAINT(0),
    .USE_MEM_INIT(0), .WAKEUP_TIME("disable_sleep"),
    .WRITE_DATA_WIDTH_A(32), .WRITE_MODE_B("no_change")
  ) u_chunk_ram (
    .dbiterrb(), .doutb(chunk_rd_data), .sbiterrb(),
    .addra(scan_offset[CHUNK_AW-1:0]), .addrb(chunk_rd_index[CHUNK_AW-1:0]),
    .clka(dco_clk), .clkb(host_clk), .dina(scan_old),
    .ena(1'b1), .enb(1'b1),
    .injectdbiterra(1'b0), .injectsbiterra(1'b0),
    .regceb(1'b1), .rstb(1'b0), .sleep(1'b0), .wea(chunk_write)
  );
`endif

  // ---------------------------------------------------------------------------
  // Histogram memory write port.
  //
  // Both spectra are BIN_COUNT x 32 block RAMs.  Two synthesis constraints shape
  // this block:
  //
  //   * Vivado cannot infer block RAM from a memory written inside a process
  //     that also has an asynchronous reset, so the writes live in their own
  //     clocked process with no reset.  The reset-time image is produced by the
  //     ST_CLEAR sweep that the state machine walks after reset.
  //   * Vivado also refuses a RAM that has "multiple writes via different ports
  //     in same process", so the three write sources - the clear sweep, the
  //     event read/modify/write and the read-and-clear scan - are collapsed
  //     into a single write port here.  They are mutually exclusive because
  //     each is gated by a different state, and every array element still has
  //     exactly one procedural writer.
  // ---------------------------------------------------------------------------
  logic                    ram_clear_sel;

  wire event_write = (state == ST_EVENT_WRITE);
  wire scan_write  = (state == ST_SCAN_WRITE);
  assign ram_clear_sel = (state == ST_CLEAR);

  always_comb begin
    if (ram_clear_sel) begin
      peak_ram_addr     = clear_addr;
      integral_ram_addr = clear_addr;
      ram_peak_data     = 32'd0;
      ram_integral_data = 32'd0;
    end else if (event_write) begin
      peak_ram_addr     = peak_event_addr;
      integral_ram_addr = integral_event_addr;
      ram_peak_data     = sat_inc(peak_old);
      ram_integral_data = sat_inc(integral_old);
    end else begin
      // ST_SCAN_WRITE: clear the bin just copied out.
      peak_ram_addr     = scan_addr;
      integral_ram_addr = scan_addr;
      ram_peak_data     = 32'd0;
      ram_integral_data = 32'd0;
    end
  end

  // Only one write port per RAM, and the enable picks the owning state: the
  // clear sweep writes both spectra, an event writes both, and the incremental
  // scan clears only the spectrum it is currently draining.
  assign ram_peak_we     = ram_clear_sel | event_write |
                           (scan_write && !chunk_kind);
  assign ram_integral_we = ram_clear_sel | event_write |
                           (scan_write &&  chunk_kind);
endmodule
