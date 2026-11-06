`timescale 1ns/1ps
`default_nettype none

module tb_gray_frame_framer;
    reg clk = 0;
    always #5 clk = ~clk;
    reg rst = 1, config_valid = 0, s_valid = 0, m_ready = 0;
    reg [31:0] config_width = 0, config_height = 0;
    reg [7:0] s_data = 0;
    wire config_ready, cfg_error, s_ready, m_valid;
    wire [7:0] m_data;
    wire m_sof, m_eol, m_eof, frame_done;
    gray_frame_framer dut (
        .clk(clk), .rst(rst), .config_valid(config_valid), .config_ready(config_ready),
        .config_width(config_width), .config_height(config_height), .cfg_error(cfg_error),
        .s_valid(s_valid), .s_ready(s_ready), .s_data(s_data),
        .m_valid(m_valid), .m_ready(m_ready), .m_data(m_data),
        .m_sof(m_sof), .m_eol(m_eol), .m_eof(m_eof), .frame_done(frame_done)
    );

    // Reference model has one queued word and independent frame coordinates.
    bit ref_active = 0, ref_closed = 0, ref_valid = 0;
    bit exp_error = 0, exp_done = 0, was_stalled = 0;
    reg [10:0] ref_word, stalled_word;
    wire [10:0] actual_word = {m_sof, m_eol, m_eof, m_data};
    integer ref_width = 0, ref_height = 0, ref_x = 0, ref_y = 0;
    integer pushes = 0, pops = 0, completed = 0, bad_configs = 0;
    integer stalls = 0, closed_cycles = 0, ignored_configs = 0, reset_frames = 0;
    integer discarded = 0;

    always @(posedge clk) begin
        if (rst) begin
            if (config_ready !== 0 || s_ready !== 0 || m_valid !== 0)
                $fatal(1, "Handshake enabled during reset");
            if (ref_active) reset_frames = reset_frames + 1;
            if (ref_valid) discarded = discarded + 1;
            ref_active = 0; ref_closed = 0; ref_valid = 0;
            ref_x = 0; ref_y = 0;
            exp_error = 0; exp_done = 0; was_stalled = 0;
        end else begin
            if (cfg_error !== exp_error || frame_done !== exp_done)
                $fatal(1, "Pulse mismatch error=%b/%b done=%b/%b",
                       cfg_error, exp_error, frame_done, exp_done);
            exp_error = 0; exp_done = 0;
            if (config_ready !== !ref_active || m_valid !== ref_valid)
                $fatal(1, "Config/output validity mismatch");
            if (s_ready !== (ref_active && !ref_closed && (!ref_valid || m_ready)))
                $fatal(1, "Input readiness mismatch");
            if (was_stalled && (m_valid !== 1 || actual_word !== stalled_word))
                $fatal(1, "Output metadata/data changed under backpressure");
            if (ref_valid && actual_word !== ref_word)
                $fatal(1, "Output mismatch expected=%h got=%h", ref_word, actual_word);
            was_stalled = m_valid && !m_ready;
            stalled_word = actual_word;
            if (was_stalled) stalls = stalls + 1;
            if (ref_closed && ref_active) closed_cycles = closed_cycles + 1;
            if (config_valid && !config_ready) ignored_configs = ignored_configs + 1;

            if (m_valid && m_ready) begin
                ref_valid = 0;
                pops = pops + 1;
                if (m_eof) begin
                    ref_active = 0;
                    exp_done = 1;
                    completed = completed + 1;
                end
            end
            if (s_valid && s_ready) begin
                if (!ref_active || ref_closed || ref_valid)
                    $fatal(1, "Input accepted outside frame/capacity");
                ref_word = {(ref_x == 0 && ref_y == 0),
                            (ref_x == ref_width-1),
                            (ref_x == ref_width-1 && ref_y == ref_height-1), s_data};
                ref_valid = 1;
                pushes = pushes + 1;
                if (ref_x == ref_width-1) begin
                    ref_x = 0;
                    if (ref_y == ref_height-1) ref_closed = 1;
                    else ref_y = ref_y + 1;
                end else ref_x = ref_x + 1;
            end
            if (config_valid && config_ready) begin
                if (config_width == 0 || config_height == 0 ||
                    config_width > 65536 || config_height > 65536) begin
                    exp_error = 1;
                    bad_configs = bad_configs + 1;
                end else begin
                    ref_width = config_width; ref_height = config_height;
                    ref_x = 0; ref_y = 0; ref_closed = 0; ref_active = 1;
                end
            end
        end
    end

    bit pending = 0;
    integer serial = 0;
    reg [31:0] rng = 32'h4eca1239;
    task automatic tick(input bit want_valid, input bit want_ready, input bit reset_now,
                        input bit cv, input reg [31:0] cw, input reg [31:0] ch);
        begin
            @(negedge clk);
            rst = reset_now;
            config_valid = cv; config_width = cw; config_height = ch;
            m_ready = want_ready;
            if (reset_now) begin pending = 0; s_valid = 0; end
            else begin
                if (!pending && want_valid) begin
                    serial = serial + 1;
                    s_data = 8'((serial * 37) ^ (serial >> 2));
                    pending = 1;
                end
                s_valid = pending;
            end
            @(posedge clk);
            if (!reset_now && s_valid && s_ready) pending = 0;
            #1;
        end
    endtask

    task automatic configure(input reg [31:0] w, input reg [31:0] h);
        begin
            if (ref_active || pending) $fatal(1, "Test attempted config while busy");
            tick(0, 0, 0, 1, w, h);
            // Mutate config pins immediately after handshake to test snapshot.
            tick(0, 0, 0, 0, 32'hffffffff, 0);
        end
    endtask

    task automatic run_frame(input integer w, input integer h, input bit random_stalls);
        integer n, count_before, cycles;
        begin
            configure(w, h);
            count_before = pushes; n = completed; cycles = 0;
            while (ref_active) begin
                rng = rng ^ (rng << 13);
                rng = rng ^ (rng >> 17);
                rng = rng ^ (rng << 5);
                tick(!ref_closed && (!random_stalls || rng[2:0] != 0),
                     !random_stalls || rng[7:4] < 11, 0, 0, rng, ~rng);
                cycles = cycles + 1;
                if (cycles > w * h * 10 + 40) $fatal(1, "Per-frame timeout");
            end
            if (pushes - count_before != w * h || completed != n + 1)
                $fatal(1, "Frame count mismatch %0dx%0d", w, h);
            if (!random_stalls && cycles != w * h + 1)
                $fatal(1, "Continuous throughput has a bubble");
            tick(0, 1, 0, 0, 0, 0); // Verify done pulse then its deassertion.
            tick(0, 1, 0, 0, 0, 0);
        end
    endtask

    integer i, pre_push, pre_pop;
    initial begin
        tick(0, 0, 1, 1, 1, 1);
        tick(0, 1, 1, 0, 0, 0);
        tick(0, 1, 0, 0, 0, 0);
        configure(0, 1); configure(1, 0);
        configure(65537, 1); configure(1, 65537);
        configure(32'hffffffff, 1);

        // 1x1: all flags set; completion waits for the final OUTPUT handshake.
        configure(1, 1);
        tick(1, 0, 0, 0, 0, 0);
        pre_push = pushes; pre_pop = pops;
        repeat (12) tick(1, 0, 0, 1, 9, 9);
        if (pushes != pre_push || pops != pre_pop || frame_done !== 0)
            $fatal(1, "Final stalled pixel did not close input / defer completion");
        tick(0, 0, 1, 0, 0, 0); // Discard final pending output and extra source word.
        tick(0, 1, 0, 0, 0, 0);

        run_frame(1, 1, 0);
        run_frame(1, 17, 1); // Every pixel is EOL, EOF only on final row.
        run_frame(19, 1, 1); // One row; SOF and EOF still distinct.
        run_frame(32, 7, 0);
        for (i = 0; i < 60; i = i + 1)
            run_frame(1 + (i * 7) % 17, 1 + (i * 11) % 13, 1);

        configure(7, 9);
        repeat (13) tick(1, 1, 0, 0, 0, 0);
        tick(0, 0, 1, 1, 1, 1); // Mid-frame reset drops buffered word/state.
        tick(0, 1, 0, 0, 0, 0);
        run_frame(3, 2, 1);

        // Boundary coordinates: full rollover of each independent 16-bit axis.
        run_frame(65536, 1, 0);
        run_frame(1, 65536, 0);
        // This legal config would overflow a 32-bit product. Accept a prefix,
        // then reset; it must stay active with no premature EOF/done.
        configure(65536, 65536);
        repeat (20) tick(1, 1, 0, 0, 1, 1);
        if (!ref_active || frame_done !== 0 || m_eof !== 0)
            $fatal(1, "Maximum 2D frame terminated prematurely");
        tick(0, 1, 1, 0, 0, 0);
        tick(0, 1, 0, 0, 0, 0);
        run_frame(2, 3, 1);
        if (bad_configs != 5 || completed != 68 || reset_frames < 3 ||
            ignored_configs < 10 || stalls < 10 || closed_cycles < 12 || pops < 1000)
            $fatal(1, "Insufficient test coverage frames=%0d errors=%0d", completed, bad_configs);
        if (pushes != pops + discarded)
            $fatal(1, "Pixel accounting mismatch accepted=%0d emitted=%0d discarded=%0d",
                   pushes, pops, discarded);
        $display("PASS tb_gray_frame_framer frames=%0d pixels=%0d cfg_errors=%0d stalls=%0d resets=%0d discarded=%0d",
                 completed, pops, bad_configs, stalls, reset_frames, discarded);
        $finish;
    end

    initial begin
        if ($test$plusargs("FRAMER_INJECT_FAILURE")) begin
            #1;
            $fatal(1, "Intentional framer failure-path self-test");
        end
    end
    initial begin
        #20000000;
        $fatal(1, "tb_gray_frame_framer global timeout");
    end
endmodule

`default_nettype wire
