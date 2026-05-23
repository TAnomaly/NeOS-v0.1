module uart_tx #(
    parameter CLK_HZ = 27_000_000,
    parameter BAUD   = 115200
)(
    input  wire       clk,
    input  wire       reset,
    input  wire       we,
    input  wire [7:0] data,
    output reg        tx,
    output wire       busy
);

    localparam DIV = CLK_HZ / BAUD;

    reg [15:0] cnt;
    reg [ 3:0] bit_idx;
    reg [ 9:0] shift;   // {stop, 8 data, start}
    reg        active;

    assign busy = active;

    always @(posedge clk) begin
        if (reset) begin
            tx      <= 1'b1;
            cnt     <= 0;
            bit_idx <= 0;
            active  <= 0;
        end else if (!active) begin
            tx <= 1'b1;
            if (we) begin
                shift   <= {1'b1, data, 1'b0};
                cnt     <= 0;
                bit_idx <= 0;
                active  <= 1;
            end
        end else begin
            tx <= shift[0];
            if (cnt == DIV - 1) begin
                cnt   <= 0;
                shift <= {1'b1, shift[9:1]};
                if (bit_idx == 9) active <= 0;
                else              bit_idx <= bit_idx + 1;
            end else cnt <= cnt + 1;
        end
    end

endmodule
