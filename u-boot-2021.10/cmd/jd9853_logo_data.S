// SPDX-License-Identifier: GPL-2.0+
/*
 * Embedded RGB565 boot logo for SPI JD9853 (172x320).
 * Generated at build time from logo/boot_logo.png via build/scripts/gen_spi_bootlogo.py
 */

.section .rodata
.align 2
.global jd9853_logo_data
jd9853_logo_data:
.incbin "../../build/generated/spi_bootlogo/logo_172x320.rgb565"
.global jd9853_logo_end
jd9853_logo_end:
