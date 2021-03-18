/*
 * UsbDevice.h
 *
 *  Created on: 18 Mar 2021
 *      Author: Kamtar
 */

#ifndef LIB_CPP_USB_USBDEVICE_HPP_
#define LIB_CPP_USB_USBDEVICE_HPP_

#include "usb_device_config.h"
#include "usb.h"

#include "usb_device.h"
#include "usb_device_dci.h"

#include "fsl_device_registers.h"

#include "usb_device_ehci.h"

namespace KamtarUSB {

class UsbDevice {
public:
	UsbDevice();

	void Init();

	static usb_status_t s_device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam);
	usb_status_t device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam);

private:

	struct instance_entry{ UsbDevice* _m;  usb_device_handle dev;};
	static instance_entry s_instance_list[4];
	static uint8_t		  s_instance_num;

	usb_device_handle m_dev_ptr;
	bool m_init;
};

} /* namespace KamtarUSB */

#endif /* LIB_CPP_USB_USBDEVICE_HPP_ */
