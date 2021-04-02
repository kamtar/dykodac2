/*
 * ControlDevice.cpp
 *
 *  Created on: 2 Apr 2021
 *      Author: Kamtar
 */

#include <usb/ControlDevice.hpp>
#include <usb_device_ch9.h>

namespace KamtarUSB {

ControlDevice::ControlDevice(USBManager& usb_man) : UsbDevice(usb_man)
{
	// TODO Auto-generated constructor stub

}

void ControlDevice::InitClass()
{
	InitBaseClass();
	setup_control_ep();
}

void ControlDevice::setup_control_ep()
{
	usb_device_endpoint_init_struct_t ep;

	ep.zlt = 1U;
	ep.transferType = USB_ENDPOINT_CONTROL;
	ep.interval = 0;
	ep.endpointAddress = USB_CONTROL_ENDPOINT | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT);
	ep.maxPacketSize = USB_CONTROL_MAX_PACKET_SIZE;

	init_endpoint(ep);

	ep.endpointAddress = USB_CONTROL_ENDPOINT | (USB_OUT << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT);

	init_endpoint(ep);
}

usb_status_t ControlDevice::endpoint_callback(usb_device_handle handle,  usb_device_endpoint_callback_message_struct_t *message, uint8_t ep)
{

	uint8_t* buffer_ptr = 0;
	uint32_t buffer_len = 0;

	if (message->isSetup)
	{
		usb_setup_struct_t *setup = (usb_setup_struct_t *)(message->buffer);

		UsbDevice::s_usb_man->USB_DeviceGetDescriptor(handle, setup, &buffer_len, &buffer_ptr);
		USB_DeviceSendRequest(handle, ep, buffer_ptr, buffer_len);
	}
}

usb_status_t ControlDevice::device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam)
{

}

} /* namespace KamtarUSB */
