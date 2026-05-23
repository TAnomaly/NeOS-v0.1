module ram (
    input  wire        clk,
    input  wire [ 3:0] we,
    input  wire [12:0] addr,   // word address, 8192 words = 32 KB
    input  wire [31:0] wdata,
    output reg  [31:0] rdata
);

    reg [31:0] mem [0:8191];

    initial $readmemh("/home/tugmirk/tangnano9k_picorv32/picorv32/firmware.hex", mem);

    always @(posedge clk) begin
        if (we[0]) mem[addr][ 7: 0] <= wdata[ 7: 0];
        if (we[1]) mem[addr][15: 8] <= wdata[15: 8];
        if (we[2]) mem[addr][23:16] <= wdata[23:16];
        if (we[3]) mem[addr][31:24] <= wdata[31:24];
        rdata <= mem[addr];
    end

endmodule
