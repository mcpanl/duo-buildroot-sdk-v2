/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __HAL_SPI3_H__
#define __HAL_SPI3_H__

#include <stddef.h>
#include <stdint.h>

int hal_spi3_init(uint32_t hz);
int hal_spi3_xfer(const void *tx, size_t len);

#endif
