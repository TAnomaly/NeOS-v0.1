/*
 * HyperRAM (HyperBus) controller — Tang Nano 9K on-die PSRAM, channel 0.
 *
 *  DDR via Gowin ODDR/IDDR primitives.  Runs the HyperBus CK at clk_ddr/2,
 *  i.e. 2 bytes per clk_ddr cycle (one byte per CK half-cycle).
 *
 *  Single 32-bit word read/write transactions.  Linear burst type, fixed
 *  initial latency = 6 CK (default-config HyperRAM).
 *
 *  Bring-up status:
 *    - init_done   : pulled high after RESET_n hold + a short post-reset delay
 *    - last_read[31:0] / last_addr / etc available for debug via memory-map
 *
 *  NOT proven on hardware. Expect multiple iterations to land timings.
 */

`default_nettype none

module hyperram (
    input  wire        clk,            // controller clock (e.g. 25 MHz)
    input  wire        reset,          // active high

    // user command interface (single 32-bit word)
    input  wire        cmd_valid,
    input  wire        cmd_we,
    input  wire [21:0] cmd_addr,       // word address (each word = 4 bytes)
    input  wire [31:0] cmd_wdata,
    output reg         busy,
    output reg         rdata_valid,
    output reg  [31:0] rdata,
    output reg         init_done,

    // HyperBus physical (channel 0)
    output wire        O_psram_ck,
    output wire        O_psram_ck_n,
    output reg         O_psram_cs_n,
    output reg         O_psram_reset_n,
    inout  wire  [7:0] IO_psram_dq,
    inout  wire        IO_psram_rwds
);

    // ---------- Reset / init timing ----------
    // RESET# hold ~ 150 us min after VCC stable.  We hold for 16384 controller
    // clocks (~650 us @ 25 MHz) — comfortable margin.
    localparam [13:0] INIT_HOLD = 14'h3FFF;
    reg [13:0] init_cnt;
    always @(posedge clk) begin
        if (reset) begin
            init_cnt        <= 14'd0;
            O_psram_reset_n <= 1'b0;
            init_done       <= 1'b0;
        end else if (init_cnt != INIT_HOLD) begin
            init_cnt        <= init_cnt + 14'd1;
            O_psram_reset_n <= 1'b0;
        end else begin
            O_psram_reset_n <= 1'b1;
            init_done       <= 1'b1;
        end
    end

    // ---------- DDR clock output (CK = clk/1 toggling each edge) ----------
    // Output a clock that is 1 when controller drives clk-rising bytes, 0 on
    // clk-falling.  CK toggles every clk → CK period = 2/clk_freq.  Wait — we
    // need CK to toggle at clk frequency itself (one CK period per clk cycle)
    // so 2 bytes per CK cycle equals 2 bytes per clk cycle.
    //
    // Trick: drive ODDR with D0=1, D1=0 only while CS# low.  When CS# high,
    // hold CK low.

    reg ck_enable;
    wire ck_d0 = ck_enable;     // rising-edge value: 1
    wire ck_d1 = 1'b0;          // falling-edge value: 0
    ODDR u_oddr_ck (
        .Q0  (O_psram_ck),
        .Q1  (),
        .D0  (ck_d0),
        .D1  (ck_d1),
        .TX  (1'b0),
        .CLK (clk)
    );
    assign O_psram_ck_n = ~O_psram_ck;

    // ---------- DDR data path (DQ[7:0]) ----------
    reg  [7:0] dq_d0;       // rising-edge byte (controller drives)
    reg  [7:0] dq_d1;       // falling-edge byte
    reg        dq_oe;       // 1 = controller drives, 0 = release
    wire [7:0] dq_pad_drive;
    wire [7:0] dq_pad_in_r; // sampled at rising
    wire [7:0] dq_pad_in_f; // sampled at falling

    // ODDR drives pad; IOBUF tri-stated by dq_oe; IDDR samples from pad.
    // We use one ODDR + one IOBUF + one IDDR per DQ bit.
    genvar gi;
    generate for (gi = 0; gi < 8; gi = gi + 1) begin : g_dq
        wire pad_out;
        wire pad_in;
        ODDR u_oddr (
            .Q0  (pad_out),
            .Q1  (),
            .D0  (dq_d0[gi]),
            .D1  (dq_d1[gi]),
            .TX  (1'b0),
            .CLK (clk)
        );
        IOBUF u_iobuf (
            .O  (pad_in),
            .IO (IO_psram_dq[gi]),
            .I  (pad_out),
            .OEN(~dq_oe)
        );
        IDDR u_iddr (
            .Q0 (dq_pad_in_r[gi]),
            .Q1 (dq_pad_in_f[gi]),
            .D  (pad_in),
            .CLK(clk)
        );
    end endgenerate

    // ---------- RWDS ----------
    reg  rwds_oe;
    reg  rwds_drive;
    wire rwds_in_pad;
    wire rwds_pad_out;
    ODDR u_oddr_rwds (
        .Q0  (rwds_pad_out),
        .Q1  (),
        .D0  (rwds_drive),
        .D1  (rwds_drive),
        .TX  (1'b0),
        .CLK (clk)
    );
    IOBUF u_iobuf_rwds (
        .O  (rwds_in_pad),
        .IO (IO_psram_rwds),
        .I  (rwds_pad_out),
        .OEN(~rwds_oe)
    );

    // ---------- Transaction FSM ----------
    // Each state = 1 clk cycle = 1 HyperBus CK cycle = 2 bytes transferred.
    //
    // CA bytes are 6 total, transferred in 3 CK cycles (3 states).
    // Latency = 6 CK cycles → 6 states.
    // Data = 4 bytes → 2 states.

    localparam [3:0]
        S_IDLE = 4'd0,
        S_CA01 = 4'd1,    // bytes 0/1 of CA on D0/D1
        S_CA23 = 4'd2,
        S_CA45 = 4'd3,
        S_LAT  = 4'd4,    // 6 CK cycles latency (waste 6 states)
        S_RD01 = 4'd5,    // sample bytes 0/1
        S_RD23 = 4'd6,    // sample bytes 2/3
        S_WR01 = 4'd7,
        S_WR23 = 4'd8,
        S_FIN  = 4'd9;

    reg [3:0] state;
    reg [2:0] lat_cnt;   // 0..5

    reg        is_write;
    reg [21:0] xact_addr;
    reg [31:0] xact_wdata;

    // CA assembly (48 bits, MSB-first byte order)
    //   CA[47]   = ~is_write  (1=read)
    //   CA[46]   = 0          (memory)
    //   CA[45]   = 1          (linear burst)
    //   CA[44:16]= upper addr (we put word_addr[21:3] in low 19 bits)
    //   CA[15:3] = reserved 0
    //   CA[2:0]  = word_addr[2:0]
    wire [47:0] CA = {
        ~is_write,
        1'b0,
        1'b1,
        16'b0,
        xact_addr[21:3],
        7'b0,
        xact_addr[2:0]
    };

    // For each CK cycle we send 2 bytes: D0 = MSByte, D1 = next byte.
    // State S_CA01 → D0=CA[47:40], D1=CA[39:32]
    // State S_CA23 → D0=CA[31:24], D1=CA[23:16]
    // State S_CA45 → D0=CA[15:8],  D1=CA[7:0]

    reg [7:0] rd_b0, rd_b1, rd_b2, rd_b3;

    always @(posedge clk) begin
        if (reset) begin
            state        <= S_IDLE;
            busy         <= 1'b0;
            rdata_valid  <= 1'b0;
            rdata        <= 32'h0;
            O_psram_cs_n <= 1'b1;
            ck_enable    <= 1'b0;
            dq_oe        <= 1'b0;
            dq_d0        <= 8'h00;
            dq_d1        <= 8'h00;
            rwds_oe      <= 1'b0;
            rwds_drive   <= 1'b0;
            lat_cnt      <= 3'd0;
            is_write     <= 1'b0;
            xact_addr    <= 22'h0;
            xact_wdata   <= 32'h0;
            rd_b0 <= 0; rd_b1 <= 0; rd_b2 <= 0; rd_b3 <= 0;
        end else begin
            rdata_valid <= 1'b0;
            case (state)
                S_IDLE: begin
                    O_psram_cs_n <= 1'b1;
                    ck_enable    <= 1'b0;
                    dq_oe        <= 1'b0;
                    rwds_oe      <= 1'b0;
                    busy         <= 1'b0;
                    if (cmd_valid && init_done) begin
                        is_write   <= cmd_we;
                        xact_addr  <= cmd_addr;
                        xact_wdata <= cmd_wdata;
                        O_psram_cs_n <= 1'b0;
                        ck_enable    <= 1'b1;
                        dq_oe        <= 1'b1;
                        dq_d0        <= CA[47:40];
                        dq_d1        <= CA[39:32];
                        state        <= S_CA23;
                        busy         <= 1'b1;
                    end
                end
                S_CA01: begin
                    /* Not reached — initial CA01 setup happens in IDLE→busy. */
                    state <= S_CA23;
                end
                S_CA23: begin
                    dq_d0 <= CA[31:24];
                    dq_d1 <= CA[23:16];
                    state <= S_CA45;
                end
                S_CA45: begin
                    dq_d0 <= CA[15:8];
                    dq_d1 <= CA[7:0];
                    state <= S_LAT;
                    lat_cnt <= 3'd0;
                end
                S_LAT: begin
                    dq_oe <= 1'b0;                 // release for read latency
                    if (lat_cnt == 3'd5) begin
                        if (is_write) begin
                            dq_oe      <= 1'b1;
                            rwds_oe    <= 1'b1;
                            rwds_drive <= 1'b0;    // no byte mask
                            dq_d0      <= xact_wdata[7:0];
                            dq_d1      <= xact_wdata[15:8];
                            state      <= S_WR23;
                        end else begin
                            state <= S_RD01;
                        end
                    end else lat_cnt <= lat_cnt + 3'd1;
                end
                S_RD01: begin
                    rd_b0 <= dq_pad_in_r;
                    rd_b1 <= dq_pad_in_f;
                    state <= S_RD23;
                end
                S_RD23: begin
                    rd_b2 <= dq_pad_in_r;
                    rd_b3 <= dq_pad_in_f;
                    rdata <= {dq_pad_in_f, dq_pad_in_r, rd_b1, rd_b0};
                    rdata_valid <= 1'b1;
                    state <= S_FIN;
                end
                S_WR23: begin
                    dq_d0 <= xact_wdata[23:16];
                    dq_d1 <= xact_wdata[31:24];
                    state <= S_FIN;
                end
                S_FIN: begin
                    O_psram_cs_n <= 1'b1;
                    ck_enable    <= 1'b0;
                    dq_oe        <= 1'b0;
                    rwds_oe      <= 1'b0;
                    busy         <= 1'b0;
                    state        <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
