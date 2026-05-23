module uart_rx #(
    parameter CLK_HZ = 27_000_000,
    parameter BAUD   = 115200
)(
    input  wire       clk,
    input  wire       reset,
    input  wire       rx,
    output reg  [7:0] data,
    output reg        valid,
    input  wire       read_ack
);
    localparam DIV  = CLK_HZ / BAUD;
    localparam HALF = DIV / 2;

    reg rx_s0, rx_s1;
    always @(posedge clk) begin
        rx_s0 <= rx;
        rx_s1 <= rx_s0;
    end
    wire rx_sync = rx_s1;

    reg [15:0] cnt;
    reg [ 3:0] bit_idx;
    reg [ 7:0] shift;
    reg [ 1:0] state;
    localparam S_IDLE=2'd0, S_START=2'd1, S_DATA=2'd2, S_STOP=2'd3;

    always @(posedge clk) begin
        if (reset) begin
            state <= S_IDLE; cnt <= 0; bit_idx <= 0;
            valid <= 1'b0;   data <= 8'h00;
        end else begin
            if (read_ack) valid <= 1'b0;
            case (state)
            S_IDLE:  if (!rx_sync) begin cnt <= 0; state <= S_START; end
            S_START: if (cnt == HALF - 1) begin
                         cnt <= 0;
                         if (!rx_sync) begin state <= S_DATA; bit_idx <= 0; end
                         else            state <= S_IDLE;
                     end else cnt <= cnt + 1;
            S_DATA:  if (cnt == DIV - 1) begin
                         cnt <= 0;
                         shift <= {rx_sync, shift[7:1]};
                         if (bit_idx == 7) state <= S_STOP;
                         else              bit_idx <= bit_idx + 1;
                     end else cnt <= cnt + 1;
            S_STOP:  if (cnt == DIV - 1) begin
                         cnt <= 0; state <= S_IDLE;
                         if (rx_sync) begin
                             data  <= shift;
                             valid <= 1'b1;
                         end
                     end else cnt <= cnt + 1;
            endcase
        end
    end
endmodule
