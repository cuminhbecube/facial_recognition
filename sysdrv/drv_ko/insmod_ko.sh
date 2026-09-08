#!/bin/sh
cmd=`realpath $0`
_DIR=`dirname $cmd`
cd $_DIR

udevadm control --stop-exec-queue

__insmod()
{
	if [ -f "$1" ];then
		insmod $@
	fi
}

__rmmod_camera_sensor()
{
	for item in `echo "imx415 os04a10 sc4336 sc3336 sc530ai gc2053 sc200ai sc401ai sc450ai techpoint"`
	do
		if lsmod | grep $item | awk '{print $3}' |grep -w 0;then
			rmmod $item
		fi
	done
}

__chk_camera_sensors()
{
	sensor_count=0
	sensor_width=0
	sensor_height=0
	for item in /proc/rkisp-vir0 /proc/rkisp-vir1
	do
		if [ -f "$item" ] && grep -q "Input.*Format" "$item"; then
			msg_sen=`grep "Input.*Format" "$item" | head -n 1`
			msg_size=${msg_sen##*Size:}
			msg_size=${msg_size%%@*}
			msg_width=${msg_size%%x*}
			msg_height=${msg_size##*x}
			[ "$msg_width" -gt "$sensor_width" ] && sensor_width=$msg_width
			[ "$msg_height" -gt "$sensor_height" ] && sensor_height=$msg_height
			sensor_count=$((sensor_count + 1))
		fi
	done
	[ "$sensor_count" -eq 0 ] && sensor_count=1
}

__insmod rtc-pcf8563.ko

__insmod rk_dvbm.ko

__insmod videobuf2-memops.ko
__insmod videobuf2-common.ko
__insmod videobuf2-v4l2.ko
__insmod videobuf2-vmalloc.ko
__insmod videobuf2-cma-sg.ko

# CAM DRV CHOOSE
#__insmod sc431hai.ko
__insmod sc235hai.ko

__insmod techpoint.ko

__insmod video_rkcif.ko
__insmod video_rkisp.ko
__insmod phy-rockchip-csi2-dphy-hw.ko
__insmod phy-rockchip-csi2-dphy.ko

__rmmod_camera_sensor

echo 1 > /sys/module/video_rkcif/parameters/clr_unready_dev
echo 1 > /sys/module/video_rkisp/parameters/clr_unready_dev

__insmod rga3.ko

__insmod mpp_vcodec.ko

__insmod rknpu.ko
__insmod snd-soc-rv1106.ko

__insmod motor.ko

__chk_camera_sensors

__insmod rockit.ko mcu_fw_path="./hpmcu_wrap.bin" mcu_fw_addr=0xff6fe000 \
	isp_max_w=$sensor_width isp_max_h=$sensor_height rk_cam_num=$sensor_count

__insmod rve.ko

__insmod goodix.ko

udevadm control --start-exec-queue

# Ethernet-only products do not package this optional loader.
if [ -x "$(pwd)/insmod_wifi.sh" ]; then
	"$(pwd)/insmod_wifi.sh" &
fi
