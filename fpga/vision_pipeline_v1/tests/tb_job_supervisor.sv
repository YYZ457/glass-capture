`timescale 1ns/1ps
module tb_job_supervisor;
    reg clk=0; always #5 clk=~clk;
    reg rst=1, req_valid=0, engine_idle=1, engine_done=0, abort_request=0;
    reg rsp_ready=0, recovery_ack=0;
    reg [127:0] req_config=0;
    reg [31:0] req_tag=0, req_timeout_cycles=0, engine_status=0;
    wire req_ready, engine_start, rsp_valid, busy, quarantined;
    wire [127:0] engine_config;
    wire [31:0] rsp_tag,rsp_status,rsp_cycles;
    job_supervisor dut(.*);
    task tick; begin @(posedge clk); #1; end endtask
    task submit(input [31:0] tag, input [31:0] limit);
        begin
            @(negedge clk);
            if(!req_ready) $fatal(1,"not ready");
            req_tag=tag; req_timeout_cycles=limit; req_config={96'b0,tag}; req_valid=1;
            tick();
            if(limit != 0 && (!engine_start || !busy)) $fatal(1,"start missing");
            @(negedge clk); req_valid=0; req_config='1;
        end
    endtask
    task consume;
        begin @(negedge clk); rsp_ready=1; tick(); @(negedge clk); rsp_ready=0; end
    endtask
    integer i, delay_cycles;
    initial begin
        repeat(3) tick(); @(negedge clk); rst=0;
        submit(1,8); tick();
        if(engine_start || engine_config != 1) $fatal(1,"snapshot/pulse");
        @(negedge clk); engine_done=1; engine_status=7; tick();
        @(negedge clk); engine_done=0;
        if(!rsp_valid || rsp_status!=7 || rsp_tag!=1 || rsp_cycles!=2) $fatal(1,"completion");
        repeat(6) begin
            tick(); if(!rsp_valid || rsp_status!=7 || rsp_tag!=1 || rsp_cycles!=2 || req_ready)
                $fatal(1,"backpressure response unstable");
        end
        consume();
        submit(2,0);
        if(!rsp_valid || engine_start || busy || rsp_status!=32'hfffffffd) $fatal(1,"zero limit");
        consume();
        submit(3,1); tick();
        if(!quarantined || rsp_status!=32'hffffffff || rsp_cycles!=1) $fatal(1,"timeout");
        consume(); tick(); if(req_ready) $fatal(1,"reuse after timeout");
        @(negedge clk); recovery_ack=1; engine_idle=0; tick();
        if(!quarantined) $fatal(1,"recovered while busy");
        @(negedge clk); engine_idle=1; tick();
        @(negedge clk); recovery_ack=0;
        if(quarantined || !req_ready) $fatal(1,"recovery failed");
        submit(4,1); engine_done=1; tick();
        if(rsp_status!=7 || quarantined) $fatal(1,"deadline completion precedence");
        @(negedge clk); engine_done=0; consume();
        submit(5,100); abort_request=1; engine_done=1; tick();
        if(rsp_status!=32'hfffffffe || !quarantined) $fatal(1,"abort precedence");
        @(negedge clk); abort_request=0; engine_done=0; rst=1; tick();
        @(negedge clk); rst=0;
        // Repeated distinct jobs catch stale completion, stale tags and snapshots.
        for(i=10;i<210;i=i+1) begin
            delay_cycles=1+(i%11);
            submit(i,20);
            repeat(delay_cycles-1) tick();
            if(delay_cycles > 1) @(negedge clk);
            engine_done=1; engine_status=0; tick();
            @(negedge clk); engine_done=0;
            if(!rsp_valid || rsp_status || rsp_tag!=i || rsp_cycles!=delay_cycles || engine_config!=i)
                $fatal(1,"repeated job mismatch %0d",i);
            consume();
        end
        submit(300,10); tick(); @(negedge clk); rst=1; tick();
        if(busy || rsp_valid || engine_start || quarantined) $fatal(1,"reset flush");
        $display("PASS tb_job_supervisor"); $finish;
    end
    initial begin #1000000; $fatal(1,"watchdog"); end
endmodule
