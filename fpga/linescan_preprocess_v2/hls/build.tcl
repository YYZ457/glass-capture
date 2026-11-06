set root [file dirname [file dirname [file normalize [info script]]]]
if {![info exists ::env(LINESCAN_BUILD)]} {error "Set LINESCAN_BUILD"}
set project $::env(LINESCAN_BUILD)
set mode synth
if {[info exists ::env(LINESCAN_MODE)]} {set mode $::env(LINESCAN_MODE)}
if {$mode ni {verify synth cosim}} {error "Mode must be verify/synth/cosim"}
file mkdir [file dirname $project]
cd [file dirname $project]
open_project -reset [file tail $project]
set_top preprocess_tile
add_files "$root/hls/preprocess_tile.cpp" -cflags "-std=c++11"
if {$mode eq "cosim"} {
  add_files -tb "$root/tests/test_preprocess_cosim.cpp" -cflags "-std=c++11"
} else {
  add_files -tb "$root/tests/test_preprocess_tile.cpp" -cflags "-std=c++11"
}
open_solution -reset solution1 -flow_target vivado
set_part {xczu9eg-ffvb1156-2-e}
create_clock -period 10 -name default
config_interface -m_axi_addr64
csim_design -clean
if {$mode ne "verify"} {csynth_design}
if {$mode eq "cosim"} {cosim_design -rtl verilog}
exit
