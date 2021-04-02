/*
 * ControlDevice.hpp
 *
 *  Created on: 2 Apr 2021
 *      Author: Kamtar
 */

#ifndef LIB_CPP_USB_CONTROLDEVICE_HPP_
#define LIB_CPP_USB_CONTROLDEVICE_HPP_
#include <UsbDevice.hpp>

namespace KamtarUSB {

class ControlDevice : UsbDevice {
public:
	ControlDevice(USBManager& usb_man);

	virtual void InitClass() override;
	virtual usb_status_t device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam) override;

	virtual usb_status_t endpoint_callback(usb_device_handle handle,  usb_device_endpoint_callback_message_struct_t *message, uint8_t ep) override;

private:
	void setup_control_ep();

	usb_setup_struct_t m_setup_buff;
};

} /* namespace KamtarUSB */

#endif /* LIB_CPP_USB_CONTROLDEVICE_HPP_ */
