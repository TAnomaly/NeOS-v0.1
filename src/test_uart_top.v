// Continuous UART send: we tied high so uart_tx restarts immediately after each frame
module top (
    input  wire       clk,
    input  wire       rst_n,
    output wire       uart_tx,
    output reg  [5:0] led
);
    wire busy;
    reg [22:0] tick;
    always @(posedge clk) begin
        if (!rst_n) begin tick<=0; led<=6'b111110; end
        else begin
            tick <= tick + 1;
            if (tick == 23'h7FFFFF) led <= ~led;
        end
    end
    uart_tx #(.CLK_HZ(27_000_000), .BAUD(115200)) u (
        .clk(clk), .reset(!rst_n), .we(1'b1),
        .data(8'h41),
        .tx(uart_tx), .busy(busy)
    );
endmodule
