// Pin 17 idle-high; bursts of 0 (null-byte frames) at 100 Hz to confirm path
module top (
    input  wire       clk,
    input  wire       rst_n,
    output reg        uart_tx,
    output reg  [5:0] led
);
    reg [23:0] c;
    always @(posedge clk) begin
        c <= c + 1;
        led <= ~c[23:18];
        // 115200 baud, DIV=234.
        // generate a 10-bit framed 0x00 every ~10 ms
        // bit time = 234 cycles. one frame = 2340 cycles.
        // duty: send frame, then idle until 270000 cycles total.
        if (c[19:0] < 20'd2340) uart_tx <= 1'b0; // continuous low for one frame time
        else uart_tx <= 1'b1;
    end
endmodule
