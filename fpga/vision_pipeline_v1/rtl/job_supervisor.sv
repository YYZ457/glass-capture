`timescale 1ns/1ps
// Single clock domain. This supervisor DOES NOT stop AXI transactions.
// A timeout quarantines the engine until external reset/drain is acknowledged.
module job_supervisor #(
    parameter integer CFG_W = 128,
    parameter integer TAG_W = 32
) (
    input wire clk, input wire rst,
    input wire req_valid, output wire req_ready,
    input wire [CFG_W-1:0] req_config,
    input wire [TAG_W-1:0] req_tag,
    input wire [31:0] req_timeout_cycles,
    output reg engine_start,
    output reg [CFG_W-1:0] engine_config,
    input wire engine_idle, input wire engine_done,
    input wire [31:0] engine_status,
    input wire abort_request,
    output reg rsp_valid, input wire rsp_ready,
    output reg [TAG_W-1:0] rsp_tag,
    output reg [31:0] rsp_status,
    output reg [31:0] rsp_cycles,
    output wire busy, output reg quarantined,
    input wire recovery_ack
);
    // Status 0: success; engine errors are passed through except reserved codes.
    // fffffffd: invalid zero timeout, fffffffe: abort, ffffffff: timeout.
    // engine_status must never use reserved codes fffffffd..ffffffff.
    reg running;
    reg [31:0] elapsed, timeout_latched;
    assign busy = running;
    // No same-cycle response consumption/request acceptance: one outstanding job.
    assign req_ready = !rst && !running && !rsp_valid && !quarantined && engine_idle;
    always @(posedge clk) begin
        if (rst) begin
            running <= 0; engine_start <= 0; engine_config <= 0;
            rsp_valid <= 0; rsp_tag <= 0; rsp_status <= 0; rsp_cycles <= 0;
            elapsed <= 0; timeout_latched <= 0; quarantined <= 0;
        end else begin
            engine_start <= 0;
            if (rsp_valid && rsp_ready) rsp_valid <= 0;
            // Caller must reset/drain engine AND its downstream bus first.
            // engine_idle alone is insufficient evidence of bus quiescence.
            if (quarantined && recovery_ack && engine_idle && !running && !rsp_valid)
                quarantined <= 0;
            if (req_valid && req_ready) begin
                engine_config <= req_config;
                rsp_tag <= req_tag;
                elapsed <= 0;
                timeout_latched <= req_timeout_cycles;
                if (req_timeout_cycles == 0) begin
                    rsp_valid <= 1; rsp_status <= 32'hfffffffd; rsp_cycles <= 0;
                end else begin
                    running <= 1; engine_start <= 1;
                end
            end else if (running) begin
                // First cycle counted is the edge at which engine observes start.
                elapsed <= elapsed + 1;
                // Abort wins; completion at deadline wins over timeout.
                if (abort_request || engine_done || elapsed == timeout_latched - 1) begin
                    running <= 0; rsp_valid <= 1; rsp_cycles <= elapsed + 1;
                    if (abort_request) begin
                        rsp_status <= 32'hfffffffe; quarantined <= 1;
                    end else if (engine_done) begin
                        rsp_status <= engine_status;
                    end else begin
                        rsp_status <= 32'hffffffff; quarantined <= 1;
                    end
                end
            end
        end
    end
endmodule
