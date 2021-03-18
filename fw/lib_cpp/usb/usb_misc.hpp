/*
 * usb_misc.hpp
 *
 *  Created on: 25 Feb 2021
 *      Author: Kamtar
 */

#ifndef LIB_CPP_USB_USB_MISC_HPP_
#define LIB_CPP_USB_USB_MISC_HPP_
#include <stdint.h>
#include <stddef.h>

namespace KamtarUSB {

enum DescType : uint8_t {
	Any = 0x00,    // Wildcard for searches
	Device = 0x01,
	Conf = 0x02,
	String = 0x03,
	Interface = 0x04,
	Endpoint = 0x05,
	DeviceQualifier = 0x06,
	OtherSpeedConf = 0x07,
	InterfacePower = 0x08,
	OnTheGo = 0x09,
	Debug = 0x0A,
	InterfaceAssociation = 0x0B,
	HID = 0x21,
	Report = 0x22,
	Physical = 0x23,
	ClassInterface = 0x24,
	HUB = 0x29
};


struct DescriptorItem {
	void* data;
	size_t len;
	DescriptorItem* next;
};

}

#endif /* LIB_CPP_USB_USB_MISC_HPP_ */
