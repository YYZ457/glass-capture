# Vitis HLS 2022.1. Usage: vitis_hls -f build.tcl
# Target part supplied explicitly through SG_HLS_PART; no board-address guesses.
if {![info exists ::env(SG_HLS_PART)]} {error "Set SG_HLS_PART to the verified board part number"}
set here [file dirname [file normalize [info script]]]
open_project [file join $here output image_stats]
set_top sg_image_stats
add_files [file join $here sg_image_stats.cpp]
open_solution solution1 -flow_target vivado
set_part $::env(SG_HLS_PART)
create_clock -period 10
csynth_design
exit
