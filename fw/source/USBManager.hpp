/*
 * USBManager.hpp
 *
 *  Created on: 27 Feb 2021
 *      Author: Kamtar
 */

#ifndef SOURCE_USBMANAGER_HPP_
#define SOURCE_USBMANAGER_HPP_
#include <usb/usb_misc.hpp>
#include <stdint.h>

using namespace KamtarUSB;

class USBManager
{
	constexpr static size_t DescriptorListMax = 8;

public:
	USBManager(uint8_t usb_id);

	static bool register_Descriptor(DescriptorItem& i);
	static bool init_USB();

	//Get descriptors callback - should be called from Chapter9 stack
	static usb_status_t USB_DeviceGetDescriptor(usb_device_handle handle, usb_setup_struct_t *setup, uint32_t *out_length, uint8_t **out_buffer);

private:

	static uint8_t m_usb_id;
	static uint8_t m_list_n;
	static DescriptorItem m_desc_list[DescriptorListMax];
};



#endif /* SOURCE_USBMANAGER_HPP_ */
