set root [file dirname [file dirname [file normalize [info script]]]]
if {![info exists ::env(VISION_RTL_BUILD)]} { error "Set VISION_RTL_BUILD" }
set out $::env(VISION_RTL_BUILD)
file mkdir $out
foreach top {stream_fifo gray_frame_framer job_supervisor} {
    create_project -in_memory -part xczu9eg-ffvb1156-2-e
    read_verilog -sv "$root/rtl/$top.sv"
    synth_design -top $top -part xczu9eg-ffvb1156-2-e -mode out_of_context
    create_clock -name clk -period 10 [get_ports clk]
    report_utilization -file "$out/${top}_utilization.rpt"
    write_checkpoint -force "$out/${top}_synth.dcp"
    close_project
}
puts "PASS RTL synthesis: three modules"
exit
