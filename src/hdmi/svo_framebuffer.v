/*
 *  svo_framebuffer — 160x120 @ 4 bpp, 16-color palette framebuffer.
 *
 *  - CPU writes to a dedicated dual-clock BRAM (port A @ clk, port B @ clk_pixel).
 *  - Pixel pipeline overrides the upstream video stream's tdata when
 *    fb_enable is high; tvalid/tready/tuser pass through unchanged so timing
 *    stays locked to the upstream pixel generator.
 *  - 160x120 source → 640x480 screen via integer 4x pixel-doubling.
 *
 *  Memory map (CPU side, 16 KiB region starting at base):
 *    +0x0000 .. +0x003F   palette  (16 entries, one 32-bit word each,
 *                                   low 24 bits = 0x00_RR_GG_BB)
 *    +0x1000 .. +0x357F   pixel buffer (2400 words, 8 pixels/word, 4 bpp,
 *                                       low nibble = leftmost pixel)
 */

`timescale 1ns / 1ps
`include "svo_defines.vh"

module svo_framebuffer #( `SVO_DEFAULT_PARAMS ) (
    // ---------- CPU side (clk domain) ----------
    input  wire        cpu_clk,
    input  wire        cpu_resetn,

    // word-addressed window: addr[12] selects palette (0) vs pixels (1)
    input  wire        cpu_sel,         // strobe: this cycle's access is for FB
    input  wire [12:0] cpu_addr_word,   // 13-bit word address
    input  wire        cpu_we,          // 1=write, 0=read
    input  wire [31:0] cpu_wdata,
    output reg  [31:0] cpu_rdata,

    // fb_enable register (separate addr, written by SoC)
    input  wire        cpu_enable_we,
    input  wire        cpu_enable_wdata,
    output reg         fb_enable,

    // ---------- HDMI pixel-clock side ----------
    input  wire        clk_pixel,
    input  wire        resetn,

    // upstream video stream (text/pong output)
    input  wire                                 in_axis_tvalid,
    output wire                                 in_axis_tready,
    input  wire [SVO_BITS_PER_PIXEL-1:0]        in_axis_tdata,
    input  wire [0:0]                           in_axis_tuser,

    // downstream
    output wire                                 out_axis_tvalid,
    input  wire                                 out_axis_tready,
    output wire [SVO_BITS_PER_PIXEL-1:0]        out_axis_tdata,
    output wire [0:0]                           out_axis_tuser
);
    `SVO_DECLS

    // ---------------- Palette (16 × 32-bit registers) ----------------
    reg [31:0] palette [0:15];
    integer pi;
    initial for (pi = 0; pi < 16; pi = pi + 1) palette[pi] = 32'h0;

    // ---------------- Pixel buffer (dual-clock BRAM, 2400 words = 9.6 KiB) ----------------
    // Exact depth matters: synth rounds depth up to BSRAM granularity, so 2400
    // packs into ~5 BSRAM blocks vs ~16 for 4096-deep.
    (* ram_style = "block" *) reg [31:0] pix_mem [0:2399];

    // ---------------- CPU write port (cpu_clk) ----------------
    //
    // Pixel BRAM is SDP: write-only on CPU side (port A), read-only on pixel
    // side (port B). Readback of pixel memory is not supported; reads in the
    // pixel region return 0. Palette is in regs and is freely readable.
    reg [31:0] cpu_rdata_pal;

    // Region select: bit[12] of word address.
    //   palette: cpu_addr_word[12]=0  (CPU base 0x20000000, addr_word 0..15)
    //   pixels:  cpu_addr_word[12]=1  (CPU base 0x20004000, addr_word 0x1000+i,
    //                                  low 12 bits = pixel index 0..2399)
    wire pix_write_en   = cpu_resetn && cpu_sel &&  cpu_addr_word[12] && cpu_we;
    wire pal_write_en   = cpu_resetn && cpu_sel && !cpu_addr_word[12] && cpu_we;
    wire [11:0] pix_addr = cpu_addr_word[11:0];
    wire [3:0]  pal_addr = cpu_addr_word[3:0];

    // Pixel BRAM write port — kept in its own always block to give Yosys the
    // cleanest possible SDP inference (single WE, single ADDR, single DATA).
    always @(posedge cpu_clk) begin
        if (pix_write_en) pix_mem[pix_addr] <= cpu_wdata;
    end

    // Palette write + readback + fb_enable
    always @(posedge cpu_clk) begin
        if (!cpu_resetn) begin
            fb_enable <= 1'b0;
        end else begin
            if (cpu_enable_we) fb_enable <= cpu_enable_wdata;
            if (pal_write_en)  palette[pal_addr] <= cpu_wdata;
            cpu_rdata_pal <= palette[pal_addr];
        end
    end

    reg cpu_was_pix;
    always @(posedge cpu_clk) cpu_was_pix <= cpu_sel && cpu_addr_word[12];

    always @* cpu_rdata = cpu_was_pix ? 32'h0 : cpu_rdata_pal;

    // ---------------- Pixel-clock side ----------------
    // 2-FF sync of fb_enable from cpu_clk to clk_pixel
    reg [1:0] fb_en_sync;
    always @(posedge clk_pixel) fb_en_sync <= {fb_en_sync[0], fb_enable};
    wire fb_en_pix = fb_en_sync[1];

    // Scan x,y derived from beats on the upstream stream.
    // tuser[0] marks start-of-frame: at that beat we treat it as pixel (0,0).
    wire beat = in_axis_tvalid && out_axis_tready;
    reg [9:0] x;   // 0..639
    reg [8:0] y;   // 0..479

    always @(posedge clk_pixel) begin
        if (!resetn) begin
            x <= 10'd0;
            y <= 9'd0;
        end else if (beat) begin
            if (in_axis_tuser[0]) begin
                x <= 10'd1;
                y <= 9'd0;
            end else if (x == 10'd639) begin
                x <= 10'd0;
                y <= y + 9'd1;
            end else begin
                x <= x + 10'd1;
            end
        end
    end

    // FB coords (pixel-doubled 4×)
    wire [7:0] fb_x = x[9:2];   // 0..159
    wire [6:0] fb_y = y[8:2];   // 0..119

    // Word address: each row has 20 words (160 px / 8 px-per-word).
    //   word_idx = fb_y * 20 + fb_x[7:3]   (range 0..2399)
    wire [11:0] pix_word_addr = (fb_y * 7'd20) + {7'd0, fb_x[7:3]};
    wire [2:0]  pix_nibble    = fb_x[2:0];  // which of 8 nibbles in word

    // BRAM read (sync, 1-cycle latency)
    reg [31:0] pix_word_r;
    reg [2:0]  pix_nibble_r;
    always @(posedge clk_pixel) begin
        pix_word_r   <= pix_mem[pix_word_addr];
        pix_nibble_r <= pix_nibble;
    end

    // Extract nibble (comb)
    reg [3:0] cur_nibble;
    always @* begin
        case (pix_nibble_r)
            3'd0: cur_nibble = pix_word_r[ 3: 0];
            3'd1: cur_nibble = pix_word_r[ 7: 4];
            3'd2: cur_nibble = pix_word_r[11: 8];
            3'd3: cur_nibble = pix_word_r[15:12];
            3'd4: cur_nibble = pix_word_r[19:16];
            3'd5: cur_nibble = pix_word_r[23:20];
            3'd6: cur_nibble = pix_word_r[27:24];
            3'd7: cur_nibble = pix_word_r[31:28];
        endcase
    end

    // Palette lookup (comb), reorder to {B,G,R} — pipeline already SVO_BITS_PER_PIXEL=24 = {B,G,R}
    wire [31:0] pal_word = palette[cur_nibble];
    wire [7:0]  pix_r = pal_word[23:16];
    wire [7:0]  pix_g = pal_word[15: 8];
    wire [7:0]  pix_b = pal_word[ 7: 0];
    wire [23:0] fb_color = {pix_b, pix_g, pix_r};

    // Output — passthrough timing, override tdata when enabled
    assign in_axis_tready  = out_axis_tready;
    assign out_axis_tvalid = in_axis_tvalid;
    assign out_axis_tuser  = in_axis_tuser;
    assign out_axis_tdata  = fb_en_pix ? fb_color : in_axis_tdata;

endmodule
