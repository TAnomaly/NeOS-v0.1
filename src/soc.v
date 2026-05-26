module soc (
    input  wire       clk,
    input  wire       reset,
    input  wire       uart_rx,
    output wire       uart_tx,
    output reg  [5:0] led,

    // HDMI text terminal stream (consumed by external svo_hdmi)
    output reg        term_in_tvalid,
    input  wire       term_in_tready,
    output reg  [7:0] term_in_tdata,

    // HDMI hardware overlay: pong on/off. Bit[0] = enable.
    output reg        pong_enable,

    // HDMI framebuffer (PNG display). CPU-side BRAM port + enable register.
    output wire        fb_cpu_resetn,
    output wire        fb_cpu_sel,
    output wire [12:0] fb_cpu_addr_word,
    output wire        fb_cpu_we,
    output wire [31:0] fb_cpu_wdata,
    input  wire [31:0] fb_cpu_rdata,
    output wire        fb_cpu_enable_we,
    output wire        fb_cpu_enable_wdata,

    // SPI master pins (to W5500 ethernet)
    output wire        spi_sck,
    output wire        spi_mosi,
    input  wire        spi_miso,
    output wire        spi_cs_n,
    output wire        spi_rst_n
);

    // PicoRV32 mem interface
    wire        mem_valid;
    wire        mem_instr;
    wire        mem_ready;
    wire [31:0] mem_addr;
    wire [31:0] mem_wdata;
    wire [ 3:0] mem_wstrb;
    wire [31:0] mem_rdata;
    wire        mem_la_read;
    wire        mem_la_write;
    wire [31:0] mem_la_addr;
    wire [31:0] mem_la_wdata;
    wire [ 3:0] mem_la_wstrb;

    reg         mem_ready_r;

    picorv32 #(
        .ENABLE_COUNTERS     (0),
        .ENABLE_COUNTERS64   (0),
        .ENABLE_REGS_16_31   (1),
        .ENABLE_REGS_DUALPORT(1),
        .LATCHED_MEM_RDATA   (0),
        .TWO_STAGE_SHIFT     (1),
        .BARREL_SHIFTER      (0),
        .TWO_CYCLE_COMPARE   (0),
        .TWO_CYCLE_ALU       (0),
        .COMPRESSED_ISA      (1),
        .CATCH_MISALIGN      (0),
        .CATCH_ILLINSN       (0),
        .ENABLE_PCPI         (0),
        .ENABLE_MUL          (1),
        .ENABLE_FAST_MUL     (0),
        .ENABLE_DIV          (1),
        .ENABLE_IRQ          (1),
        .ENABLE_IRQ_QREGS    (1),
        .ENABLE_IRQ_TIMER    (1),
        .ENABLE_TRACE        (0),
        .REGS_INIT_ZERO      (0),
        .STACKADDR           (32'h0000_8000),
        .PROGADDR_RESET      (32'h0000_0000),
        .PROGADDR_IRQ        (32'h0000_0010)
    ) cpu (
        .clk         (clk),
        .resetn      (!reset),
        .trap        (),
        .mem_valid   (mem_valid),
        .mem_instr   (mem_instr),
        .mem_ready   (mem_ready),
        .mem_addr    (mem_addr),
        .mem_wdata   (mem_wdata),
        .mem_wstrb   (mem_wstrb),
        .mem_rdata   (mem_rdata),
        .mem_la_read (mem_la_read),
        .mem_la_write(mem_la_write),
        .mem_la_addr (mem_la_addr),
        .mem_la_wdata(mem_la_wdata),
        .mem_la_wstrb(mem_la_wstrb),
        .irq         (32'h0)
    );

    // Address decode
    wire sel_ram  = (mem_addr[31:28] == 4'h0);
    wire sel_uart = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h0);
    wire sel_led  = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h1);
    wire sel_term  = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h2);
    wire sel_pong   = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h3);
    wire sel_fb_en  = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h4);
    wire sel_fb     = (mem_addr[31:28] == 4'h2);
    wire sel_spi    = (mem_addr[31:28] == 4'h1) && (mem_addr[7:4] == 4'h6);

    // Framebuffer CPU port wires
    assign fb_cpu_resetn       = !reset;
    assign fb_cpu_sel          = mem_valid && sel_fb && !mem_ready_r;
    assign fb_cpu_addr_word    = mem_addr[14:2];
    assign fb_cpu_we           = mem_valid && sel_fb && (mem_wstrb != 4'b0000) && !mem_ready_r;
    assign fb_cpu_wdata        = mem_wdata;
    assign fb_cpu_enable_we    = mem_valid && sel_fb_en && (mem_wstrb != 4'b0000) && !mem_ready_r;
    assign fb_cpu_enable_wdata = mem_wdata[0];

    // BRAM: 32 KB, 8192 words, byte-write enables
    wire [31:0] ram_rdata;
    wire [ 3:0] ram_we = (mem_valid && sel_ram) ? mem_wstrb : 4'b0000;
    ram ram_i (
        .clk  (clk),
        .we   (ram_we),
        .addr (mem_addr[14:2]),
        .wdata(mem_wdata),
        .rdata(ram_rdata)
    );

    // UART TX — CPU-driven
    wire        uart_tx_we   = mem_valid && sel_uart && (mem_addr[3:2] == 2'b00)
                              && (mem_wstrb != 4'b0000) && !mem_ready_r;
    wire [ 7:0] uart_tx_data = mem_wdata[7:0];
    wire        uart_tx_busy;

    uart_tx #(.CLK_HZ(27_000_000), .BAUD(115200)) u_tx (
        .clk  (clk),
        .reset(reset),
        .we   (uart_tx_we),
        .data (uart_tx_data),
        .tx   (uart_tx),
        .busy (uart_tx_busy)
    );

    // SPI master (W5500 ethernet)
    wire        spi_cs_sel = mem_valid && sel_spi && !mem_ready_r;
    wire [31:0] spi_rdata;
    spi_master u_spi (
        .clk   (clk),
        .reset (reset),
        .cs    (spi_cs_sel),
        .we    (sel_spi ? mem_wstrb : 4'b0),
        .addr  (mem_addr[4:2]),
        .wdata (mem_wdata),
        .rdata (spi_rdata),
        .sck   (spi_sck),
        .mosi  (spi_mosi),
        .miso  (spi_miso),
        .cs_n  (spi_cs_n),
        .rst_n (spi_rst_n)
    );

    // UART RX — CPU polls; read clears valid
    wire [7:0] uart_rx_data;
    wire       uart_rx_valid;
    wire       uart_rx_ack = mem_valid && sel_uart && (mem_addr[3:2] == 2'b10)
                            && (mem_wstrb == 4'b0000) && !mem_ready_r;

    uart_rx #(.CLK_HZ(27_000_000), .BAUD(115200)) u_rx (
        .clk     (clk),
        .reset   (reset),
        .rx      (uart_rx),
        .data    (uart_rx_data),
        .valid   (uart_rx_valid),
        .read_ack(uart_rx_ack)
    );

    // HDMI text stream — CPU writes byte to 0x10000020.
    // term_in_tvalid stays high until svo_hdmi acks via term_in_tready.
    // STATUS bit 0 (sel_term, [3:2]=01) = term busy (still pending).
    wire term_we = mem_valid && sel_term && (mem_addr[3:2] == 2'b00)
                   && (mem_wstrb != 4'b0000) && !mem_ready_r;
    always @(posedge clk) begin
        if (reset) begin
            term_in_tvalid <= 1'b0;
            term_in_tdata  <= 8'h00;
            led            <= 6'b111111;
            pong_enable    <= 1'b0;
        end else begin
            // Handshake: drop tvalid the cycle svo_hdmi takes the byte.
            if (term_in_tvalid && term_in_tready)
                term_in_tvalid <= 1'b0;
            if (term_we) begin
                term_in_tdata  <= mem_wdata[7:0];
                term_in_tvalid <= 1'b1;
            end

            if (mem_valid && sel_led && (mem_wstrb != 4'b0000) && !mem_ready_r)
                led <= ~mem_wdata[5:0];

            if (mem_valid && sel_pong && (mem_wstrb != 4'b0000) && !mem_ready_r)
                pong_enable <= mem_wdata[0];
        end
    end

    // Single-cycle mem_ready
    always @(posedge clk) begin
        if (reset)                  mem_ready_r <= 1'b0;
        else if (mem_ready_r)       mem_ready_r <= 1'b0;
        else if (mem_valid)         mem_ready_r <= 1'b1;
    end
    assign mem_ready = mem_ready_r;

    // Read mux
    assign mem_rdata =
        sel_ram                              ? ram_rdata :
        (sel_uart && mem_addr[3:2] == 2'b00) ? 32'h0000_0000 :
        (sel_uart && mem_addr[3:2] == 2'b01) ? {30'b0, uart_rx_valid, uart_tx_busy} :
        (sel_uart && mem_addr[3:2] == 2'b10) ? {24'b0, uart_rx_data} :
        (sel_term && mem_addr[3:2] == 2'b00) ? 32'h0000_0000 :
        (sel_term && mem_addr[3:2] == 2'b01) ? {31'b0, term_in_tvalid} :
        sel_pong                             ? {31'b0, pong_enable} :
        sel_fb                               ? fb_cpu_rdata :
        sel_spi                              ? spi_rdata :
        32'h0000_0000;

endmodule
