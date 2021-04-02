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

#include <USBManager.hpp>

namespace KamtarUSB {

class UsbDevice {
public:
	UsbDevice(USBManager& usb_man);

	static void Init();

	void InitBaseClass();
	virtual void InitClass() = 0;

	static usb_status_t s_device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam);
	virtual usb_status_t device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam) = 0;

	static usb_status_t s_endpoint_callback(usb_device_handle handle,  usb_device_endpoint_callback_message_struct_t *message, void *callbackParam);
	virtual usb_status_t endpoint_callback(usb_device_handle handle,  usb_device_endpoint_callback_message_struct_t *message, uint8_t ep) = 0;

	bool init_endpoint(usb_device_endpoint_init_struct_t& ep);

	static USBManager* s_usb_man;

private:

	struct instance_entry{ UsbDevice* _m;  usb_device_handle dev;};
	static instance_entry s_instance_list[4];
	static uint8_t		  s_instance_num;

	static usb_device_handle s_dev_ptr;
	static bool s_init;
	static bool s_created;

	struct endpoint_entry{ uint16_t magic; UsbDevice* _m;  uint8_t e_addr;};
	static endpoint_entry s_ep_list[16];
	static uint8_t s_ep_len;


};

} /* namespace KamtarUSB */

#endif /* LIB_CPP_USB_USBDEVICE_HPP_ */
