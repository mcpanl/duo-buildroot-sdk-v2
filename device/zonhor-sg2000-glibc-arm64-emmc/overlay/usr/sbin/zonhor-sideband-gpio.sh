#!/bin/sh
# Hardware V1.1 sideband: drive GPIOB17 (linux gpio 465) low.
# Must run before AIC8800 power-on. Idempotent; logs to /tmp/sideband-gpio.log.

GPIO_NUM=465
LOG=/tmp/sideband-gpio.log

log() {
	echo "$*" >> "${LOG}"
}

set_output_low() {
	gpio_path="/sys/class/gpio/gpio${GPIO_NUM}"

	if [ ! -d "${gpio_path}" ]; then
		if ! echo "${GPIO_NUM}" > /sys/class/gpio/export 2>>"${LOG}"; then
			log "export gpio${GPIO_NUM} failed (may be kernel-owned); skip"
			return 0
		fi
		sleep 0.05
	fi

	if [ ! -d "${gpio_path}" ]; then
		log "gpio${GPIO_NUM} unavailable"
		return 1
	fi

	echo out > "${gpio_path}/direction" 2>>"${LOG}" || true
	echo 0 > "${gpio_path}/value" 2>>"${LOG}" || true
	log "gpio${GPIO_NUM}=0"
}

: > "${LOG}"
set_output_low
