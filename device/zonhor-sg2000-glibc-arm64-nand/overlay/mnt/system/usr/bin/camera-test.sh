#!/bin/sh

export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd

# Release VI/ISP before face-detection demo (same as CviIspTool.sh).
killall sample_sensor_lcd sample_sensor_test sample_vio isp_tool_daemon sample_vi_fd \
	screen_demo.py zonhor-ota-ui 2>/dev/null || true
sleep 0.3

sample_vi_fd /mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel
