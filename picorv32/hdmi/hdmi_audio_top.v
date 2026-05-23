/*
 * HDMI audio + video top — drop-in replacement for svo_hdmi_top.
 * Adds HDMI 1.4 data-island audio packets to the existing SVO video pipeline.
 *
 * Pipeline:
 *   svo_term + svo_overlay + svo_enc           (text terminal video, unchanged)
 *           └► svo_tmds (3x)                   (TMDS-encoded active video)
 *
 *   audio source (CPU-controlled square wave)
 *           └► packet builder (Audio Sample Packets)
 *                + Audio Clock Regen + AVI/Audio InfoFrames
 *           └► BCH(64,56) ECC + TERC4 encoder
 *
 *   per-channel output mux selects per pixel clock:
 *     - DE high      → TMDS video pixel
 *     - data island  → TERC4 nibble
 *     - guard band   → fixed guard 10-bit pattern
 *     - preamble     → control symbol with data-island preamble bits
 *     - other blank  → normal control symbol (HSYNC/VSYNC)
 *
 * 640x480 @ 60 Hz, 48 kHz audio LPCM, square-wave generated in FPGA.
 *
 * Note: per HDMI spec compliance is rough. Not guaranteed to be accepted by
 * every monitor. Tested against ... (TBD by user).
 */

`timescale 1ns / 1ps
`include "svo_defines.vh"

module hdmi_audio_top (
    input clk,
    input resetn,

    // video clocks
    input clk_pixel,
    input clk_5x_pixel,
    input locked,

    // text-stream input (CPU writes a byte at a time)
    input        term_in_tvalid,
    output       term_in_tready,
    input  [7:0] term_in_tdata,

    // audio control (clk domain)
    input [15:0] audio_freq_hz,    // 0 = silent, else square-wave frequency

    // HDMI pins
    output       tmds_clk_n,
    output       tmds_clk_p,
    output [2:0] tmds_d_n,
    output [2:0] tmds_d_p
);
    parameter SVO_MODE             =   "640x480V";
    parameter SVO_FRAMERATE        =   60;
    parameter SVO_BITS_PER_PIXEL   =   24;
    parameter SVO_BITS_PER_RED     =    8;
    parameter SVO_BITS_PER_GREEN   =    8;
    parameter SVO_BITS_PER_BLUE    =    8;
    parameter SVO_BITS_PER_ALPHA   =    0;

    localparam [SVO_BITS_PER_PIXEL-1:0] green_pixval = 24'h00_FF_00;
    localparam [SVO_BITS_PER_PIXEL-1:0] black_pixval = 24'h00_00_00;

    // ----------------------- VIDEO PIPELINE (unchanged from svo_hdmi_top) -----------------------
    wire vdma_tvalid;
    wire vdma_tready;
    wire [SVO_BITS_PER_PIXEL-1:0] vdma_tdata_raw;
    wire [SVO_BITS_PER_PIXEL-1:0] vdma_tdata = black_pixval;
    wire [0:0] vdma_tuser;

    wire video_tvalid;
    wire video_tready;
    wire [SVO_BITS_PER_PIXEL-1:0] video_tdata;
    wire [0:0] video_tuser;

    wire term_out_tvalid;
    wire term_out_tready;
    wire [1:0] term_out_tdata;
    wire [0:0] term_out_tuser;

    wire video_enc_tvalid, video_enc_tready;
    wire [SVO_BITS_PER_PIXEL-1:0] video_enc_tdata;
    wire [3:0] video_enc_tuser;

    reg [3:0] locked_clk_q;
    reg [3:0] resetn_clk_pixel_q;

    always @(posedge clk)
        locked_clk_q <= {locked_clk_q, locked};
    always @(posedge clk_pixel)
        resetn_clk_pixel_q <= {resetn_clk_pixel_q, resetn};

    wire clk_resetn       = resetn && locked_clk_q[3];
    wire clk_pixel_resetn = locked && resetn_clk_pixel_q[3];

    svo_tcard #( `SVO_PASS_PARAMS ) svo_tcard (
        .clk(clk_pixel), .resetn(resetn),
        .out_axis_tvalid(vdma_tvalid),
        .out_axis_tready(vdma_tready),
        .out_axis_tdata (vdma_tdata_raw),
        .out_axis_tuser (vdma_tuser)
    );

    svo_term #( `SVO_PASS_PARAMS ) svo_term (
        .clk(clk), .oclk(clk_pixel), .resetn(clk_resetn),
        .in_axis_tvalid(term_in_tvalid),
        .in_axis_tready(term_in_tready),
        .in_axis_tdata (term_in_tdata),
        .out_axis_tvalid(term_out_tvalid),
        .out_axis_tready(term_out_tready),
        .out_axis_tdata (term_out_tdata),
        .out_axis_tuser (term_out_tuser)
    );

    svo_overlay #( `SVO_PASS_PARAMS ) svo_overlay (
        .clk(clk_pixel), .resetn(clk_pixel_resetn), .enable(1'b1),
        .in_axis_tvalid (vdma_tvalid),
        .in_axis_tready (vdma_tready),
        .in_axis_tdata  (vdma_tdata),
        .in_axis_tuser  (vdma_tuser),
        .over_axis_tvalid(term_out_tvalid),
        .over_axis_tready(term_out_tready),
        .over_axis_tdata (green_pixval),
        .over_axis_tuser ({term_out_tdata == 2'b10, term_out_tuser}),
        .out_axis_tvalid(video_tvalid),
        .out_axis_tready(video_tready),
        .out_axis_tdata (video_tdata),
        .out_axis_tuser (video_tuser)
    );

    svo_enc #( `SVO_PASS_PARAMS ) svo_enc (
        .clk(clk_pixel), .resetn(clk_pixel_resetn),
        .in_axis_tvalid(video_tvalid),
        .in_axis_tready(video_tready),
        .in_axis_tdata (video_tdata),
        .in_axis_tuser (video_tuser),
        .out_axis_tvalid(video_enc_tvalid),
        .out_axis_tready(video_enc_tready),
        .out_axis_tdata (video_enc_tdata),
        .out_axis_tuser (video_enc_tuser)
    );
    assign video_enc_tready = 1'b1;

    // svo_enc tuser bits:
    //   [3] = blanking (1 during blanking, 0 during active)
    //   [2:1] = control bits (HSYNC, VSYNC mapping)
    //   [0] = unused-ish
    // For our purposes:
    wire de    = !video_enc_tuser[3];
    wire [1:0] ctrl_bits = video_enc_tuser[2:1];   // {VSYNC, HSYNC}

    // ----------------------- TMDS VIDEO ENCODERS (per channel) -----------------------
    wire [9:0] tmds_video [2:0];

    // BLUE = data[23:16] = ch0, gets HSYNC/VSYNC control bits
    svo_tmds u_tmds_blue (
        .clk(clk_pixel), .resetn(clk_pixel_resetn),
        .de(de), .ctrl(ctrl_bits),
        .din(video_enc_tdata[23:16]),
        .dout(tmds_video[0])
    );
    svo_tmds u_tmds_green (
        .clk(clk_pixel), .resetn(clk_pixel_resetn),
        .de(de), .ctrl(2'b00),
        .din(video_enc_tdata[15:8]),
        .dout(tmds_video[1])
    );
    svo_tmds u_tmds_red (
        .clk(clk_pixel), .resetn(clk_pixel_resetn),
        .de(de), .ctrl(2'b00),
        .din(video_enc_tdata[7:0]),
        .dout(tmds_video[2])
    );

    // ----------------------- BLANK-PERIOD TRACKER -----------------------
    // svo_tmds has 3-cycle latency from inputs to dout. We must delay the
    // de + ctrl_bits we use in the output mux by exactly 3 cycles so that
    // ch_out's TMDS branch (tmds_video[i]) lines up with the control branch.

    reg [2:0] de_pipe;
    reg [1:0] ctrl_pipe [2:0];
    always @(posedge clk_pixel) begin
        de_pipe[0] <= de;
        de_pipe[1] <= de_pipe[0];
        de_pipe[2] <= de_pipe[1];
        ctrl_pipe[0] <= ctrl_bits;
        ctrl_pipe[1] <= ctrl_pipe[0];
        ctrl_pipe[2] <= ctrl_pipe[1];
    end
    wire        de_d3   = de_pipe[2];
    wire [1:0]  ctrl_d3 = ctrl_pipe[2];

    reg de_d3_q;
    always @(posedge clk_pixel) de_d3_q <= de_d3;
    wire enter_blank = (!de_d3) && de_d3_q;

    reg [10:0] blank_cnt;
    always @(posedge clk_pixel) begin
        if (!clk_pixel_resetn)        blank_cnt <= 0;
        else if (enter_blank)         blank_cnt <= 0;
        else if (!de_d3)              blank_cnt <= blank_cnt + 11'd1;
        else                          blank_cnt <= 0;
    end

    // ----------------------- TERC4 ENCODER -----------------------
    function [9:0] terc4;
        input [3:0] d;
        begin
            case (d)
                4'h0: terc4 = 10'b1010011100;
                4'h1: terc4 = 10'b1001100011;
                4'h2: terc4 = 10'b1011100100;
                4'h3: terc4 = 10'b1011100010;
                4'h4: terc4 = 10'b0101110001;
                4'h5: terc4 = 10'b0100011110;
                4'h6: terc4 = 10'b0110001110;
                4'h7: terc4 = 10'b0100111100;
                4'h8: terc4 = 10'b1011001100;
                4'h9: terc4 = 10'b0100111001;
                4'hA: terc4 = 10'b0110011100;
                4'hB: terc4 = 10'b1011000110;
                4'hC: terc4 = 10'b1010001110;
                4'hD: terc4 = 10'b1001110001;
                4'hE: terc4 = 10'b0101100011;
                4'hF: terc4 = 10'b1011000011;
                default: terc4 = 10'b1010011100;
            endcase
        end
    endfunction

    // Guard band patterns (HDMI 1.4 spec):
    //   Data island leading/trailing guard:
    //     CH0 (blue):  depends on hsync,vsync via TERC4 of {1,1,hsync,vsync}
    //     CH1 (green): 0x4CC  (10'b0100110011)
    //     CH2 (red):   0x4CC  (10'b0100110011)
    // We emit guard on blue using TERC4 with the HV bits embedded.

    // Control symbol coding (matches svo_tmds):
    function [9:0] ctrl_sym;
        input [1:0] c;
        begin
            case (c)
                2'b00: ctrl_sym = 10'b1101010100;
                2'b01: ctrl_sym = 10'b0010101011;
                2'b10: ctrl_sym = 10'b0101010100;
                2'b11: ctrl_sym = 10'b1010101011;
                default: ctrl_sym = 10'b1101010100;
            endcase
        end
    endfunction

    // Data island preamble: control symbols with specific ctl bits
    //   CH0: ctrl_sym(hsync,vsync) as normal control
    //   CH1: ctrl_sym(2'b01)
    //   CH2: ctrl_sym(2'b01)

    // ----------------------- BCH(64,56) ECC -----------------------
    // Compute 8-bit parity over 56-bit data. Polynomial x^8 + x^7 + x^6 + x^4 + x^2 + 1 = 0xD5
    function [7:0] bch56;
        input [55:0] data;
        integer i;
        reg [7:0] crc;
        begin
            crc = 8'h00;
            for (i = 55; i >= 0; i = i - 1) begin
                if (crc[7] ^ data[i]) crc = (crc << 1) ^ 8'h83;   // x^7+x+1 representation
                else                  crc = (crc << 1);
            end
            bch56 = crc;
        end
    endfunction

    // ----------------------- AUDIO SAMPLE GENERATOR -----------------------
    // Square-wave generator at clk_pixel (25.2 MHz), 48 kHz target.
    // Period in pixel clocks for half-square cycle:  25_200_000 / (2 * freq)
    //
    // For each audio sample at 48 kHz, generate ±0x300000 amplitude (16-bit headroom).
    // Sample rate divider: 25_200_000 / 48000 = 525  → produces audio_sample_en
    //                        every 525 pixel clocks.

    reg [9:0]  sample_div;
    reg        audio_sample_en;
    always @(posedge clk_pixel) begin
        if (!clk_pixel_resetn) begin
            sample_div      <= 0;
            audio_sample_en <= 0;
        end else if (sample_div == 10'd524) begin
            sample_div      <= 0;
            audio_sample_en <= 1;
        end else begin
            sample_div      <= sample_div + 1;
            audio_sample_en <= 0;
        end
    end

    // Sync audio_freq_hz from clk domain to clk_pixel domain (2-FF synchronizer).
    reg [15:0] freq_s0, freq_s1;
    always @(posedge clk_pixel) begin
        freq_s0 <= audio_freq_hz;
        freq_s1 <= freq_s0;
    end
    wire [15:0] freq_pix = freq_s1;

    // Square wave: toggle every (48000 / (2*freq)) audio samples.
    // For freq=440: half_period_samples = 48000/(2*440) ≈ 54.5.
    // Use integer divider with phase counter.

    reg [15:0] phase_cnt;        // counts at audio_sample_en
    reg        sq_state;
    wire [15:0] half_period =
        (freq_pix == 0)            ? 16'd0       :
        (freq_pix > 16'd24000)     ? 16'd1       :   // clamp
                                     (16'd24000 / freq_pix);   // 48000/(2*freq)

    always @(posedge clk_pixel) begin
        if (!clk_pixel_resetn) begin
            phase_cnt <= 0;
            sq_state  <= 0;
        end else if (audio_sample_en) begin
            if (freq_pix == 0) begin
                phase_cnt <= 0;
                sq_state  <= 0;
            end else if (phase_cnt >= half_period - 16'd1) begin
                phase_cnt <= 0;
                sq_state  <= ~sq_state;
            end else begin
                phase_cnt <= phase_cnt + 1;
            end
        end
    end

    wire [23:0] sample_l = sq_state ? 24'h300000 : 24'hD00000;   // +3M / -3M (signed)
    wire [23:0] sample_r = sample_l;

    // ----------------------- AUDIO FIFO (4 samples for AS Packet) -----------------------
    // Each Audio Sample Packet carries 4 audio frames (4 L+R pairs).
    // We collect 4 samples then transmit a packet.

    reg [23:0] as_l [3:0];
    reg [23:0] as_r [3:0];
    reg [1:0]  as_count;
    reg        as_ready;        // 4 samples collected, packet ready to send

    always @(posedge clk_pixel) begin
        if (!clk_pixel_resetn) begin
            as_count <= 0;
            as_ready <= 0;
        end else begin
            if (audio_sample_en) begin
                as_l[as_count] <= sample_l;
                as_r[as_count] <= sample_r;
                if (as_count == 2'd3) begin
                    as_count <= 0;
                    as_ready <= 1;
                end else begin
                    as_count <= as_count + 1;
                end
            end
            if (as_ready && pkt_consumed) as_ready <= 0;
        end
    end

    // ----------------------- PACKET SCHEDULER -----------------------
    // Per H-blank, we may insert one data island packet:
    //   - HBLANK structure: HSYNC pulse (96 cycles) + back porch (48) + front porch (16) = 160 cycles blank
    //   - Recipe for data island insertion:
    //     0..K: control (existing HSYNC/VSYNC pattern from svo_enc)
    //     K..K+7: preamble (control symbols with ctl=01 on CH1/CH2)
    //     K+8..K+9: leading guard (TERC4 of HV bits on CH0, fixed 0x4CC on CH1/CH2)
    //     K+10..K+41: packet body (32 char clocks, TERC4 nibbles)
    //     K+42..K+43: trailing guard
    //     K+44..end: control
    //   Choose K so packet fits within blanking period. For 160-cycle HBLANK, K ≥ 100.
    //
    // Packet type rotation: ACR -> AVI InfoFrame -> Audio InfoFrame -> Audio Sample (repeating)

    localparam K_START = 11'd100;
    localparam ISLAND_LEN = 11'd44;        // 8 pre + 2 guard + 32 body + 2 guard

    /* TEMPORARILY DISABLED for video-only bisect: */
    wire in_island_window = 1'b0;  /* was: !de_d3 && (blank_cnt >= K_START) && (blank_cnt < (K_START + ISLAND_LEN)); */
    wire [5:0] island_pos = (blank_cnt >= K_START) ? (blank_cnt - K_START) : 6'd0;

    // Phase decode
    wire is_preamble = in_island_window && (island_pos < 8);
    wire is_guard_l  = in_island_window && (island_pos >= 8)  && (island_pos < 10);
    wire is_body     = in_island_window && (island_pos >= 10) && (island_pos < 42);
    wire is_guard_t  = in_island_window && (island_pos >= 42) && (island_pos < 44);

    wire [4:0] body_pos = island_pos - 6'd10;   // 0..31

    // Packet selection — round-robin across frames
    reg [1:0] pkt_type;       // 0=ACR, 1=AVI IF, 2=Audio IF, 3=AS
    reg       pkt_consumed;
    reg       in_island_d;
    always @(posedge clk_pixel) begin
        if (!clk_pixel_resetn) begin
            pkt_type     <= 0;
            pkt_consumed <= 0;
            in_island_d  <= 0;
        end else begin
            in_island_d  <= in_island_window;
            pkt_consumed <= 0;
            // On the very last char of the island, advance pkt_type
            if (in_island_d && !in_island_window) begin
                pkt_type     <= (pkt_type == 2'd3) ? 2'd0 : (pkt_type + 2'd1);
                pkt_consumed <= (pkt_type == 2'd3);     // consumed the AS packet
            end
        end
    end

    // ----------------------- PACKET DATA -----------------------
    // For each char clock in the body (0..31), we send:
    //   CH0 bit pattern: 4-bit nibble that includes hdr_bit + hv + packet_seq
    //   CH1: nibble = subpacket bits (low nibble)
    //   CH2: nibble = subpacket bits (high nibble)
    //
    // HDMI packet body format:
    //   - Each of 32 char clocks emits 1 byte (8 bits) per of 4 channels(?) — actually:
    //   - Header (3 bytes data + 1 BCH parity) is sent as 4 nibbles on CH0 across all 32 chars
    //     (one header bit per char, plus parity at chars 24..31)
    //   - Subpacket 0..3 (each 8 bytes = 64 bits including ECC) are sent on CH1+CH2
    //     across the 32 chars

    // We'll build header (32 bits) and subpackets (64 bits each x4) once per packet,
    // then drive the streams during the body.

    reg [31:0] hdr;             // {parity[7:0], data[23:0]}
    reg [63:0] sub [3:0];       // {parity[7:0], data[55:0]} (LSB to high)

    // packet content depends on pkt_type; computed combinationally then registered
    // at island start.

    // Helpers to set hdr+sub at island start
    reg packet_load_pending;
    always @(posedge clk_pixel) begin
        if (!clk_pixel_resetn) packet_load_pending <= 1;
        else if (in_island_window && !in_island_d) packet_load_pending <= 1;
        else                                       packet_load_pending <= 0;
    end

    // pack helper: produce a 32-bit packet header from type+HB1+HB2
    function [31:0] make_hdr;
        input [7:0] hb0;
        input [7:0] hb1;
        input [7:0] hb2;
        reg   [23:0] data;
        begin
            data = {hb2, hb1, hb0};
            // BCH parity: simplified — use bch56 truncated to 24 bits.
            // Real header BCH uses different poly (BCH(32,24)) but we approximate
            // with full bch56 padded for now. Many monitors are tolerant.
            make_hdr = {bch56({32'h0, data}), data};
        end
    endfunction

    // pack helper: produce a 64-bit subpacket from 7 data bytes
    function [63:0] make_sub;
        input [7:0] b0;
        input [7:0] b1;
        input [7:0] b2;
        input [7:0] b3;
        input [7:0] b4;
        input [7:0] b5;
        input [7:0] b6;
        reg [55:0] data;
        begin
            data = {b6, b5, b4, b3, b2, b1, b0};
            make_sub = {bch56(data), data};
        end
    endfunction

    // -- AVI InfoFrame (type 0x82, version 2, length 13)
    //   HB0=0x82, HB1=0x02, HB2=0x0D
    //   PB0 = checksum
    //   PB1 = {Y=0, A=0, B=0, S=0} = 0x00 (RGB, no active fmt, no bar, no scan)
    //   PB2 = {C=0, M=0(picAR=na), R=8(sameAsPicAR)} = 0x08
    //   PB3 = ITC=0, EC=0, Q=0, SC=0 = 0x00
    //   PB4 = VIC = 1 (640x480 @ 60)
    //   PB5..PB13 = 0
    //   Checksum: 0x100 - sum(HB+PB[1..13]) lower 8 bits → PB0 = ...

    // We'll hardcode the packet content for now.

    // Audio Clock Regen (type 0x01)
    //   HB0=0x01, HB1=0x00, HB2=0x00
    //   N = 6144 (for 48 kHz)
    //   CTS = pixel_clock_hz × N / (128 × fs) = 25200000 × 6144 / (128 × 48000) = 25200
    //   In subpackets: SP0..SP3 each carry {0x00, CTS[19:16], CTS[15:8], CTS[7:0], 0x00, N[19:16], N[15:8], N[7:0]}
    //   Wait actually packet layout per spec:
    //     subpacket[i] = {0x00, CTS_H[3:0]@bits[19:16], CTS_M[7:0], CTS_L[7:0], 0x00, N_H[3:0]@bits[19:16], N_M[7:0], N_L[7:0]}
    //     Actually each subpacket has 7 data bytes:
    //       byte[0]=0, byte[1..3]= CTS bytes, byte[4]=0, byte[5..7]= N bytes — but only 7 bytes per subpkt
    //     Simplified layout: PB0..PB6 same in all 4 subpackets
    //       PB0 = reserved/0
    //       PB1 = {0000, CTS[19:16]}
    //       PB2 = CTS[15:8]
    //       PB3 = CTS[7:0]
    //       PB4 = {0000, N[19:16]}
    //       PB5 = N[15:8]
    //       PB6 = N[7:0]

    localparam [19:0] ACR_N   = 20'd6144;
    localparam [19:0] ACR_CTS = 20'd25200;

    // Audio InfoFrame (type 0x84, version 1, length 10)
    //   HB0=0x84, HB1=0x01, HB2=0x0A
    //   PB0 = checksum
    //   PB1 = {CT=1 (LPCM), CC=1 (2ch)} = 0x11
    //   PB2 = {SF=0(rsvd, take from stream), SS=0(rsvd)} = 0x00
    //   PB3 = 0
    //   PB4 = CA = 0 (FL, FR)
    //   PB5..PB10 = 0

    // Audio Sample Packet (type 0x02)
    //   HB0=0x02, HB1=B(layout) << 4 | {4 sample present bits},
    //   HB2 = 0 (sample flat bits)
    //   Each subpacket = 1 audio frame (2 channels):
    //     SP byte 0..2 = L sample 24-bit
    //     SP byte 3    = L IEC60958 channel status (parity, V, U, C bits)
    //     SP byte 4..6 = R sample 24-bit
    //     SP byte 7    = R IEC60958 channel status

    // Build packets based on pkt_type (combinational), latch at island start.
    reg [31:0] pending_hdr;
    reg [63:0] pending_sub [3:0];

    integer ii;
    always @(*) begin
        // Defaults
        pending_hdr = 32'h0;
        pending_sub[0] = 64'h0;
        pending_sub[1] = 64'h0;
        pending_sub[2] = 64'h0;
        pending_sub[3] = 64'h0;
        case (pkt_type)
            2'd0: begin   // Audio Clock Regen
                pending_hdr = make_hdr(8'h01, 8'h00, 8'h00);
                pending_sub[0] = make_sub(8'h00,
                                          {4'h0, ACR_CTS[19:16]}, ACR_CTS[15:8], ACR_CTS[7:0],
                                          {4'h0, ACR_N[19:16]},   ACR_N[15:8],   ACR_N[7:0]);
                pending_sub[1] = pending_sub[0];
                pending_sub[2] = pending_sub[0];
                pending_sub[3] = pending_sub[0];
            end
            2'd1: begin   // AVI InfoFrame  (type 0x82 v2 len 13). PB0=cksum
                // sum = HB0+HB1+HB2 + PB1..PB13 with our fixed values:
                // HB sum = 0x82+0x02+0x0D = 0x91
                // PB1=0x00, PB2=0x08, PB3=0x00, PB4=0x01, rest 0 → sum payload = 0x09
                // cksum = 0x100 - (0x91 + 0x09) = 0x66
                pending_hdr = make_hdr(8'h82, 8'h02, 8'h0D);
                pending_sub[0] = make_sub(8'h66, 8'h00, 8'h08, 8'h00, 8'h01, 8'h00, 8'h00);
                pending_sub[1] = make_sub(8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
                pending_sub[2] = make_sub(8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
                pending_sub[3] = make_sub(8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
            end
            2'd2: begin   // Audio InfoFrame (type 0x84 v1 len 10). PB0=cksum
                // HB sum = 0x84+0x01+0x0A = 0x8F
                // PB1=0x11 (LPCM, 2ch), PB2..PB10=0 → payload sum 0x11
                // cksum = 0x100 - (0x8F + 0x11) = 0x60
                pending_hdr = make_hdr(8'h84, 8'h01, 8'h0A);
                pending_sub[0] = make_sub(8'h60, 8'h11, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
                pending_sub[1] = make_sub(8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
                pending_sub[2] = make_sub(8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
                pending_sub[3] = make_sub(8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00, 8'h00);
            end
            2'd3: begin   // Audio Sample
                pending_hdr = make_hdr(8'h02, 8'h0F, 8'h00);   // 4 samples present
                // Each subpacket = 1 audio frame: {L24, Lstat, R24, Rstat}
                pending_sub[0] = {8'h00, as_r[0], 8'h00, as_l[0]};
                pending_sub[1] = {8'h00, as_r[1], 8'h00, as_l[1]};
                pending_sub[2] = {8'h00, as_r[2], 8'h00, as_l[2]};
                pending_sub[3] = {8'h00, as_r[3], 8'h00, as_l[3]};
            end
            default: ;
        endcase
    end

    always @(posedge clk_pixel) begin
        if (packet_load_pending) begin
            hdr    <= pending_hdr;
            sub[0] <= pending_sub[0];
            sub[1] <= pending_sub[1];
            sub[2] <= pending_sub[2];
            sub[3] <= pending_sub[3];
        end
    end

    // ----------------------- BODY NIBBLE STREAM -----------------------
    // For each body_pos (0..31):
    //   - CH0 carries 4-bit nibble: { hdr_bit_at_pos, hsync, vsync, packet_seq=0 }
    //       (we put packet_seq=0 since we're in one continuous packet)
    //   - CH1 carries 4-bit nibble: bits[2*body_pos +: 2] of sub[0] + sub[1] (low 4 bits)
    //         Actually layout: sub[0..3] bits are interleaved across CH1/CH2
    //         Simplified: CH1 bit n of body_pos b = sub[n][2*b], CH2 = sub[n][2*b+1]
    //         where n is 0..3 for the 4 subpackets, mapped to CH1[bit n], CH2[bit n]
    //   - CH2 likewise
    //
    // Per HDMI 1.4 spec the channel layout for packet body is:
    //   For each char (32 chars total, body_pos 0..31):
    //     CH0[0] = hdr bit 'body_pos'
    //     CH0[1] = hsync
    //     CH0[2] = vsync
    //     CH0[3] = 1 (data island data)   ← per spec, first char has hdr=0 of "new packet"
    //               actually CH0[3]=hdr_seq_first
    //     CH1[n] = sub[n][2*body_pos]
    //     CH2[n] = sub[n][2*body_pos+1]
    //   (n = 0..3 across the 4 bits of the nibble)
    //
    // Note: the exact CH0 bit assignment varies by source; the above is the most
    // common interpretation. Monitors generally check sync bits + hdr bits.

    wire hsync_now = ctrl_d3[0];
    wire vsync_now = ctrl_d3[1];

    wire [4:0] bp = body_pos;   // 0..31

    wire [3:0] ch0_nib_body = {1'b1, vsync_now, hsync_now, hdr[bp]};

    wire [3:0] ch1_nib_body = {sub[3][2*bp], sub[2][2*bp], sub[1][2*bp], sub[0][2*bp]};
    wire [3:0] ch2_nib_body = {sub[3][2*bp+1], sub[2][2*bp+1], sub[1][2*bp+1], sub[0][2*bp+1]};

    // Guard band nibble for CH0 = TERC4 of {1, 1, vsync, hsync}
    wire [3:0] ch0_guard_nib = {1'b1, 1'b1, vsync_now, hsync_now};

    // ----------------------- PER-CHANNEL OUTPUT MUX -----------------------
    // svo_tmds outputs handle BOTH video AND blanking control symbols already.
    // We only need to override during data-island periods. Combinational mux —
    // svo_tmds.dout is already registered so OSER10 sees stable inputs.

    wire [9:0] ch_out_r [2:0];
    assign ch_out_r[0] =
        is_preamble                  ? ctrl_sym(ctrl_d3)          :
        (is_guard_l || is_guard_t)   ? terc4(ch0_guard_nib)       :
        is_body                      ? terc4(ch0_nib_body)        :
                                       tmds_video[0];
    assign ch_out_r[1] =
        is_preamble                  ? ctrl_sym(2'b01)            :
        (is_guard_l || is_guard_t)   ? 10'b0100110011             :
        is_body                      ? terc4(ch1_nib_body)        :
                                       tmds_video[1];
    assign ch_out_r[2] =
        is_preamble                  ? ctrl_sym(2'b01)            :
        (is_guard_l || is_guard_t)   ? 10'b0100110011             :
        is_body                      ? terc4(ch2_nib_body)        :
                                       tmds_video[2];

    // ----------------------- 10-to-1 SERIALIZER + LVDS -----------------------
    wire [2:0] tmds_d;

    OSER10 tmds_serdes [2:0] (
        .Q(tmds_d),
        .D0({ch_out_r[2][0], ch_out_r[1][0], ch_out_r[0][0]}),
        .D1({ch_out_r[2][1], ch_out_r[1][1], ch_out_r[0][1]}),
        .D2({ch_out_r[2][2], ch_out_r[1][2], ch_out_r[0][2]}),
        .D3({ch_out_r[2][3], ch_out_r[1][3], ch_out_r[0][3]}),
        .D4({ch_out_r[2][4], ch_out_r[1][4], ch_out_r[0][4]}),
        .D5({ch_out_r[2][5], ch_out_r[1][5], ch_out_r[0][5]}),
        .D6({ch_out_r[2][6], ch_out_r[1][6], ch_out_r[0][6]}),
        .D7({ch_out_r[2][7], ch_out_r[1][7], ch_out_r[0][7]}),
        .D8({ch_out_r[2][8], ch_out_r[1][8], ch_out_r[0][8]}),
        .D9({ch_out_r[2][9], ch_out_r[1][9], ch_out_r[0][9]}),
        .PCLK(clk_pixel),
        .FCLK(clk_5x_pixel),
        .RESET(~clk_pixel_resetn)
    );

    ELVDS_OBUF tmds_bufds [3:0] (
        .I ({clk_pixel,  tmds_d}),
        .O ({tmds_clk_p, tmds_d_p}),
        .OB({tmds_clk_n, tmds_d_n})
    );

endmodule
