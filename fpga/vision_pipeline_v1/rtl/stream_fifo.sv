`timescale 1ns/1ps
`default_nettype none

// Single-clock ready/valid FIFO. This is NOT a clock-domain crossing FIFO.
// rst is synchronous, active high, and discards all queued words. Transfers
// are disabled while rst is asserted. DATA_W and USER_W must be >= 1;
// DEPTH must be >= 2 and need not be a power of two.
// No empty bypass: a word accepted into an empty FIFO appears next cycle.
// m_ready feeds s_ready combinationally when full; avoid handshake loops.
module stream_fifo #(
    parameter integer DATA_W = 8,
    parameter integer USER_W = 1,
    parameter integer DEPTH = 4
) (
    input  wire                  clk,
    input  wire                  rst,
    input  wire                  s_valid,
    output wire                  s_ready,
    input  wire [DATA_W-1:0]     s_data,
    input  wire [USER_W-1:0]     s_user,
    input  wire                  s_last,
    output wire                  m_valid,
    input  wire                  m_ready,
    output wire [DATA_W-1:0]     m_data,
    output wire [USER_W-1:0]     m_user,
    output wire                  m_last
);
    localparam integer WORD_W = DATA_W + USER_W + 1;
    localparam integer PTR_W = (DEPTH > 2) ? $clog2(DEPTH) : 1;
    localparam integer COUNT_W = $clog2(DEPTH + 1);

    reg [WORD_W-1:0] memory [0:DEPTH-1];
    reg [PTR_W-1:0] read_ptr, write_ptr;
    reg [COUNT_W-1:0] count;
    wire pop = m_valid && m_ready;
    wire push = s_valid && s_ready;

    assign m_valid = !rst && (count != 0);
    assign s_ready = !rst && ((count < DEPTH) || pop);
    // Asynchronous head read; memory need not be reset. Invalid outputs are 0.
    assign {m_last, m_user, m_data} = m_valid ? memory[read_ptr] : {WORD_W{1'b0}};

    always @(posedge clk) begin
        if (rst) begin
            read_ptr <= {PTR_W{1'b0}};
            write_ptr <= {PTR_W{1'b0}};
            count <= {COUNT_W{1'b0}};
        end else begin
            if (push) begin
                memory[write_ptr] <= {s_last, s_user, s_data};
                write_ptr <= (write_ptr == PTR_W'(DEPTH-1))
                           ? {PTR_W{1'b0}} : write_ptr + 1'b1;
            end
            if (pop)
                read_ptr <= (read_ptr == PTR_W'(DEPTH-1))
                          ? {PTR_W{1'b0}} : read_ptr + 1'b1;
            case ({push, pop})
                2'b10: count <= count + 1'b1;
                2'b01: count <= count - 1'b1;
                default: count <= count;
            endcase
        end
    end

    // synthesis translate_off
    initial begin
        if (DATA_W < 1 || USER_W < 1 || DEPTH < 2)
            $fatal(1, "stream_fifo requires DATA_W>=1 USER_W>=1 DEPTH>=2");
    end
    // synthesis translate_on
endmodule

`default_nettype wire
