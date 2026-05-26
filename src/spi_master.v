/*
 * Simple 8-bit SPI master, MMIO controlled.
 *
 * MMIO map (base = 0x10000060):
 *   +0x00  DATA   (R/W) 8-bit. Write to start transfer, read to get RX byte.
 *   +0x04  CTRL   (R/W) bit0 = busy (RO), bit1 = CS_N (RW, manual control),
 *                       bit2 = reset (W1), bits[15:8] = clkdiv (1..255)
 *
 * Mode 0 (CPOL=0, CPHA=0): clock idle low, sample on rising edge.
 * MSB first. Clock = sys_clk / (2 * (clkdiv+1)).
 * Default clkdiv=2 -> sys_clk/6. At 27 MHz sys clk: SPI clk = 4.5 MHz.
 *
 * Usage:
 *   1. Write CTRL with desired clkdiv and CS_N=0 to assert.
 *   2. Write DATA with byte to send. Hardware starts transfer.
 *   3. Poll CTRL bit0 (busy) until 0.
 *   4. Read DATA for received byte.
 *   5. Repeat 2-4 for multi-byte transfers.
 *   6. Write CTRL with CS_N=1 to deassert.
 */
module spi_master(
    input  wire        clk,
    input  wire        reset,

    /* MMIO interface (CPU side) */
    input  wire        cs,           /* select (mem_valid && sel_spi) */
    input  wire [3:0]  we,           /* write byte strobes */
    input  wire [2:0]  addr,         /* word offset (0=DATA, 1=CTRL) */
    input  wire [31:0] wdata,
    output reg  [31:0] rdata,

    /* SPI pins (to W5500) */
    output reg         sck,
    output reg         mosi,
    input  wire        miso,
    output reg         cs_n,
    output reg         rst_n         /* W5500 RESET, active low */
);
    reg [7:0] tx_shift;
    reg [7:0] rx_shift;
    reg [3:0] bit_cnt;
    reg       busy;
    reg [7:0] clkdiv;
    reg [7:0] tick_cnt;
    reg       sck_phase;             /* 0 = low half, 1 = high half */

    wire write_data = cs && we[0] && (addr == 3'd0);
    wire write_ctrl = cs && we[0] && (addr == 3'd1);
    wire read_data  = cs && (we == 4'b0) && (addr == 3'd0);
    wire read_ctrl  = cs && (we == 4'b0) && (addr == 3'd1);

    always @(posedge clk) begin
        if (reset) begin
            sck       <= 1'b0;
            mosi      <= 1'b0;
            cs_n      <= 1'b1;
            rst_n     <= 1'b0;       /* hold W5500 in reset on power-up */
            busy      <= 1'b0;
            bit_cnt   <= 4'd0;
            clkdiv    <= 8'd2;       /* default = sys/6 */
            tick_cnt  <= 8'd0;
            sck_phase <= 1'b0;
            tx_shift  <= 8'd0;
            rx_shift  <= 8'd0;
            rdata     <= 32'd0;
        end else begin
            /* --- CPU writes --- */
            if (write_ctrl) begin
                cs_n     <= wdata[1];
                rst_n    <= wdata[3];
                if (wdata[2]) begin
                    /* soft reset */
                    busy     <= 1'b0;
                    bit_cnt  <= 4'd0;
                    sck      <= 1'b0;
                    sck_phase<= 1'b0;
                    tick_cnt <= 8'd0;
                end
                if (wdata[15:8] != 8'd0)
                    clkdiv <= wdata[15:8];
            end
            if (write_data && !busy) begin
                tx_shift  <= wdata[7:0];
                mosi      <= wdata[7];
                bit_cnt   <= 4'd0;
                busy      <= 1'b1;
                sck       <= 1'b0;
                sck_phase <= 1'b0;
                tick_cnt  <= 8'd0;
            end

            /* --- transfer state machine (Mode 0) --- */
            if (busy) begin
                if (tick_cnt == clkdiv) begin
                    tick_cnt  <= 8'd0;
                    sck_phase <= ~sck_phase;
                    if (sck_phase == 1'b0) begin
                        /* About to enter HIGH phase: sample MISO on rising edge */
                        sck <= 1'b1;
                        rx_shift <= {rx_shift[6:0], miso};
                    end else begin
                        /* About to enter LOW phase: shift TX */
                        sck <= 1'b0;
                        if (bit_cnt == 4'd7) begin
                            busy    <= 1'b0;
                            bit_cnt <= 4'd0;
                        end else begin
                            bit_cnt  <= bit_cnt + 4'd1;
                            tx_shift <= {tx_shift[6:0], 1'b0};
                            mosi     <= tx_shift[6];
                        end
                    end
                end else begin
                    tick_cnt <= tick_cnt + 8'd1;
                end
            end

            /* --- read mux --- */
            if (read_data) begin
                rdata <= {24'd0, rx_shift};
            end else if (read_ctrl) begin
                rdata <= {16'd0, clkdiv, 4'd0, rst_n, 1'b0, cs_n, busy};
            end else begin
                rdata <= 32'd0;
            end
        end
    end
endmodule
