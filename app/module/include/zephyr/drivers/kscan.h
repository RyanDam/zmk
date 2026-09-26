/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ZMK compatibility shim for the kscan subsystem.
 *
 * The kscan subsystem was removed from Zephyr in 4.2. ZMK's kscan drivers
 * (app/module/drivers/kscan) and their consumers still use this legacy API.
 * This header restores the API surface on top of Zephyr 4.4 so the tree
 * builds until the matrix-input migration (backlog a01) removes the last
 * kscan users. It is intentionally a plain header: no syscalls, no
 * subsystem registration.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_KB_SCAN_H_
#define ZEPHYR_INCLUDE_DRIVERS_KB_SCAN_H_

#include <errno.h>
#include <zephyr/types.h>
#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief KSCAN APIs
 * @defgroup kscan_interface Keyboard Scan Driver APIs
 * @deprecated
 * @ingroup io_interfaces
 * @{
 */

/**
 * @deprecated
 * @brief Keyboard scan callback called when user press/release
 * a key on a matrix keyboard.
 *
 * @param dev Pointer to the device structure for the driver instance.
 * @param row Describes row change.
 * @param column Describes column change.
 * @param pressed Describes the kind of key event.
 */
typedef void (*kscan_callback_t)(const struct device *dev, uint32_t row,
				 uint32_t column,
				 bool pressed);

/**
 * @deprecated
 * @cond INTERNAL_HIDDEN
 *
 * Keyboard scan driver API definition and system call entry points.
 *
 * (Internal use only.)
 */
typedef int (*kscan_config_t)(const struct device *dev,
			      kscan_callback_t callback);
typedef int (*kscan_disable_callback_t)(const struct device *dev);
typedef int (*kscan_enable_callback_t)(const struct device *dev);

struct kscan_driver_api {
	kscan_config_t config;
	kscan_disable_callback_t disable_callback;
	kscan_enable_callback_t enable_callback;
};
/**
 * @endcond
 */

/**
 * @deprecated
 * @brief Configure a Keyboard scan instance.
 *
 * @param dev Pointer to the device structure for the driver instance.
 * @param callback called when keyboard devices reply to a keyboard
 * event such as key pressed/released.
 *
 * @retval 0 If successful.
 * @retval Negative errno code if failure.
 */
static inline int kscan_config(const struct device *dev,
			       kscan_callback_t callback)
{
	const struct kscan_driver_api *api =
			(struct kscan_driver_api *)dev->api;

	return api->config(dev, callback);
}

/**
 * @deprecated
 * @brief Enables callback.
 * @param dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 If successful.
 * @retval Negative errno code if failure.
 */
static inline int kscan_enable_callback(const struct device *dev)
{
	const struct kscan_driver_api *api =
			(const struct kscan_driver_api *)dev->api;

	if (api->enable_callback == NULL) {
		return -ENOSYS;
	}

	return api->enable_callback(dev);
}

/**
 * @deprecated
 * @brief Disables callback.
 * @param dev Pointer to the device structure for the driver instance.
 *
 * @retval 0 If successful.
 * @retval Negative errno code if failure.
 */
static inline int kscan_disable_callback(const struct device *dev)
{
	const struct kscan_driver_api *api =
			(const struct kscan_driver_api *)dev->api;

	if (api->disable_callback == NULL) {
		return -ENOSYS;
	}

	return api->disable_callback(dev);
}

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* ZEPHYR_INCLUDE_DRIVERS_KB_SCAN_H_ */
