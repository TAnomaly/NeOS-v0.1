module top (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       uart_rx,
    output wire       uart_tx,
    output wire [5:0] led,

    output            tmds_clk_p,
    output            tmds_clk_n,
    output      [2:0] tmds_d_p,
    output      [2:0] tmds_d_n
);

    // ---------------- Reset (auto-release after rst_n high, plus PLL lock) ----------------
    wire pll_lock;
    wire clk_pixel;
    wire clk_5x;

    reg [15:0] rst_cnt = 16'h0000;
    always @(posedge clk) begin
        if (!rst_n)
            rst_cnt <= 16'h0000;
        else if (rst_cnt != 16'hFFFF)
            rst_cnt <= rst_cnt + 16'h0001;
    end
    wire por_done = (rst_cnt == 16'hFFFF);
    wire reset    = !por_done;            // active-high for soc
    wire sys_resetn = por_done && pll_lock;

    // ---------------- HDMI pixel clocks ----------------
    // rPLL: 27 MHz → 126 MHz  (FBDIV+1=14, IDIV+1=3 → 27*14/3 = 126 MHz)
    // CLKDIV /5 → 25.2 MHz pixel clock for 640x480 @ 60 Hz
    Gowin_rPLL u_pll (
        .clkin (clk),
        .clkout(clk_5x),
        .lock  (pll_lock)
    );

    Gowin_CLKDIV u_div5 (
        .hclkin(clk_5x),
        .resetn(pll_lock),
        .clkout(clk_pixel)
    );

    // ---------------- SoC ----------------
    wire        term_in_tvalid;
    wire        term_in_tready;
    wire  [7:0] term_in_tdata;
    wire        pong_enable;

    wire        fb_cpu_resetn;
    wire        fb_cpu_sel;
    wire [12:0] fb_cpu_addr_word;
    wire        fb_cpu_we;
    wire [31:0] fb_cpu_wdata;
    wire [31:0] fb_cpu_rdata;
    wire        fb_cpu_enable_we;
    wire        fb_cpu_enable_wdata;

    soc soc_i (
        .clk                 (clk),
        .reset               (reset),
        .uart_rx             (uart_rx),
        .uart_tx             (uart_tx),
        .led                 (led),
        .term_in_tvalid      (term_in_tvalid),
        .term_in_tready      (term_in_tready),
        .term_in_tdata       (term_in_tdata),
        .pong_enable         (pong_enable),
        .fb_cpu_resetn       (fb_cpu_resetn),
        .fb_cpu_sel          (fb_cpu_sel),
        .fb_cpu_addr_word    (fb_cpu_addr_word),
        .fb_cpu_we           (fb_cpu_we),
        .fb_cpu_wdata        (fb_cpu_wdata),
        .fb_cpu_rdata        (fb_cpu_rdata),
        .fb_cpu_enable_we    (fb_cpu_enable_we),
        .fb_cpu_enable_wdata (fb_cpu_enable_wdata)
    );

    // ---------------- HDMI video + hardware pong overlay + framebuffer ----------------
    svo_hdmi_top u_hdmi (
        .clk            (clk),
        .resetn         (sys_resetn),
        .clk_pixel      (clk_pixel),
        .clk_5x_pixel   (clk_5x),
        .locked         (pll_lock),

        .term_in_tvalid (term_in_tvalid),
        .term_in_tready (term_in_tready),
        .term_in_tdata  (term_in_tdata),

        .pong_enable    (pong_enable),

        .fb_cpu_clk           (clk),
        .fb_cpu_resetn        (fb_cpu_resetn),
        .fb_cpu_sel           (fb_cpu_sel),
        .fb_cpu_addr_word     (fb_cpu_addr_word),
        .fb_cpu_we            (fb_cpu_we),
        .fb_cpu_wdata         (fb_cpu_wdata),
        .fb_cpu_rdata         (fb_cpu_rdata),
        .fb_cpu_enable_we     (fb_cpu_enable_we),
        .fb_cpu_enable_wdata  (fb_cpu_enable_wdata),

        .tmds_clk_p     (tmds_clk_p),
        .tmds_clk_n     (tmds_clk_n),
        .tmds_d_p       (tmds_d_p),
        .tmds_d_n       (tmds_d_n)
    );

endmodule
