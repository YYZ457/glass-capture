`timescale 1ns/1ps
`default_nettype none

// Self-contained XSim test; top tb_stream_fifo exercises DEPTH 3 and 4.
// All mismatches and the independent watchdog use $fatal(1).
module fifo_test_case #(
    parameter integer DEPTH = 3
) (
    input wire clk,
    output reg done
);
    localparam integer DATA_W = 19;
    localparam integer USER_W = 5;
    localparam integer WORD_W = DATA_W + USER_W + 1;
    reg rst = 1'b1;
    reg s_valid = 1'b0, m_ready = 1'b0;
    reg [DATA_W-1:0] s_data = 0;
    reg [USER_W-1:0] s_user = 0;
    reg s_last = 0;
    wire s_ready, m_valid, m_last;
    wire [DATA_W-1:0] m_data;
    wire [USER_W-1:0] m_user;

    stream_fifo #(.DATA_W(DATA_W), .USER_W(USER_W), .DEPTH(DEPTH)) dut (
        .clk(clk), .rst(rst),
        .s_valid(s_valid), .s_ready(s_ready), .s_data(s_data),
        .s_user(s_user), .s_last(s_last),
        .m_valid(m_valid), .m_ready(m_ready), .m_data(m_data),
        .m_user(m_user), .m_last(m_last)
    );

    reg [WORD_W-1:0] expected [0:16383];
    integer head = 0, tail = 0, occupancy = 0;
    integer pushes = 0, pops = 0, discarded = 0;
    integer full_swaps = 0, stalls = 0, empty_cycles = 0;
    integer nonempty_resets = 0, checks = 0;
    reg was_stalled = 0;
    reg [WORD_W-1:0] stalled_word;
    reg pending = 0;
    integer serial = 0;
    reg [31:0] rng = 32'hcafe0123 ^ DEPTH;
    wire [WORD_W-1:0] output_word = {m_last, m_user, m_data};

    // Reference queue observes pre-edge handshakes. Driver changes only at
    // falling edges, then waits #1 after rising edges to avoid testbench races.
    always @(posedge clk) begin
        if (rst) begin
            if (s_ready !== 1'b0 || m_valid !== 1'b0)
                $fatal(1, "DEPTH=%0d transfer enabled during reset", DEPTH);
            if (occupancy != 0) nonempty_resets = nonempty_resets + 1;
            discarded = discarded + occupancy;
            head = 0; tail = 0; occupancy = 0;
            was_stalled = 0;
        end else begin
            checks = checks + 1;
            if (m_valid !== (occupancy != 0))
                $fatal(1, "DEPTH=%0d valid mismatch occupancy=%0d", DEPTH, occupancy);
            if (s_ready !== ((occupancy < DEPTH) || ((occupancy != 0) && m_ready)))
                $fatal(1, "DEPTH=%0d ready mismatch occupancy=%0d", DEPTH, occupancy);
            if (was_stalled && (m_valid !== 1'b1 || output_word !== stalled_word))
                $fatal(1, "DEPTH=%0d output changed under backpressure", DEPTH);
            if (m_valid && output_word !== expected[head])
                $fatal(1, "DEPTH=%0d payload mismatch head=%0d expected=%h got=%h",
                       DEPTH, head, expected[head], output_word);
            if (occupancy == 0) empty_cycles = empty_cycles + 1;
            if (occupancy == DEPTH && s_valid && s_ready && m_valid && m_ready)
                full_swaps = full_swaps + 1;
            was_stalled = m_valid && !m_ready;
            stalled_word = output_word;
            if (was_stalled) stalls = stalls + 1;
            // Pop first: full simultaneous pop/push must return the old head.
            if (m_valid && m_ready) begin
                head = head + 1;
                occupancy = occupancy - 1;
                pops = pops + 1;
            end
            if (s_valid && s_ready) begin
                if (tail >= 16384) $fatal(1, "Reference queue capacity exceeded");
                expected[tail] = {s_last, s_user, s_data};
                tail = tail + 1;
                occupancy = occupancy + 1;
                pushes = pushes + 1;
            end
            if (occupancy < 0 || occupancy > DEPTH)
                $fatal(1, "DEPTH=%0d occupancy out of range", DEPTH);
        end
    end

    task automatic tick(input bit want_valid, input bit want_ready, input bit do_reset);
        begin
            @(negedge clk);
            rst = do_reset;
            m_ready = want_ready;
            if (do_reset) begin
                pending = 0;
                s_valid = 0;
            end else begin
                if (!pending && want_valid) begin
                    serial = serial + 1;
                    s_data = DATA_W'((serial * 32'h137b9) ^ (serial >> 3));
                    s_user = USER_W'((serial * 7) ^ (serial >> 2));
                    s_last = ((serial % 7) == 0) || ((serial % 11) == 0);
                    pending = 1;
                end
                s_valid = pending;
            end
            @(posedge clk);
            if (!do_reset && s_valid && s_ready) pending = 0;
            #1;
        end
    endtask

    task automatic drain;
        integer guard;
        begin
            guard = 0;
            // tick preserves a previously blocked source until its handshake.
            while (occupancy != 0 || pending) begin
                tick(0, 1, 0);
                guard = guard + 1;
                if (guard > DEPTH + 4) $fatal(1, "DEPTH=%0d drain timeout", DEPTH);
            end
            tick(0, 1, 0);
        end
    endtask

    integer i, before_pushes, before_pops;
    initial begin
        done = 0;
        tick(0, 0, 1);
        tick(0, 1, 1);
        repeat (4) tick(0, 1, 0); // Empty with consumer ready: no phantom words.
        tick(1, 1, 0);            // Empty simultaneous request: no bypass pop.
        drain();

        repeat (DEPTH) tick(1, 0, 0); // Fill and then block source and sink.
        repeat (9) tick(1, 0, 0);
        before_pushes = pushes;
        before_pops = pops;
        repeat (DEPTH * 13) tick(1, 1, 0); // Repeated full swaps and pointer wraps.
        if (pushes - before_pushes != DEPTH * 13 || pops - before_pops != DEPTH * 13)
            $fatal(1, "DEPTH=%0d full FIFO failed one-word-per-cycle throughput", DEPTH);
        drain();

        repeat (DEPTH) tick(1, 0, 0);
        tick(0, 0, 1); // Reset while full/stalled discards every queued word.
        repeat (3) tick(0, 1, 0);
        tick(1, 0, 0);
        tick(0, 1, 1); // Reset also overrides an otherwise possible pop.
        repeat (3) tick(0, 1, 0);

        // Deterministic per-instance random generator: randomized source gaps,
        // prolonged and isolated sink stalls, plus mid-stream resets.
        for (i = 0; i < 5000; i = i + 1) begin
            rng = rng ^ (rng << 13);
            rng = rng ^ (rng >> 17);
            rng = rng ^ (rng << 5);
            tick((rng[2:0] != 0), (rng[6:4] < 5),
                 (i == 733 || i == 2341 || i == 4007));
        end
        drain();
        if (pushes < 1000 || pops < 1000 || full_swaps < DEPTH * 13 ||
            stalls < 10 || empty_cycles < 5 || nonempty_resets < 2)
            $fatal(1, "DEPTH=%0d insufficient scenario coverage", DEPTH);
        if (pushes != pops + discarded)
            $fatal(1, "DEPTH=%0d word accounting mismatch", DEPTH);
        $display("PASS stream_fifo DEPTH=%0d pushes=%0d pops=%0d discarded=%0d full_swaps=%0d stalls=%0d checks=%0d",
                 DEPTH, pushes, pops, discarded, full_swaps, stalls, checks);
        done = 1;
    end
endmodule

module tb_stream_fifo;
    reg clk = 0;
    always #5 clk = ~clk;
    wire done3, done4;
    fifo_test_case #(.DEPTH(3)) depth3 (.clk(clk), .done(done3));
    fifo_test_case #(.DEPTH(4)) depth4 (.clk(clk), .done(done4));
    // Optional harness check: -testplusarg FIFO_INJECT_FAILURE must fail.
    initial begin
        if ($test$plusargs("FIFO_INJECT_FAILURE")) begin
            #1;
            $fatal(1, "Intentional FIFO failure-path self-test");
        end
    end
    initial begin
        wait(done3 && done4);
        $display("PASS tb_stream_fifo: all depth configurations completed");
        $finish;
    end
    initial begin
        #200000;
        $fatal(1, "tb_stream_fifo global timeout");
    end
endmodule

`default_nettype wire
