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

#define PACKED __attribute__((packed))

namespace KamtarUSB {

enum class DescType : uint8_t {
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

enum class ClassCodes : uint8_t {
	Device = 0x00,
	Audio = 0x01,
	CDC_Control = 0x02,
	HID = 0x03,
	Physical = 0x05,
	Image = 0x06,
	Printer = 0x07,
	MassStorage = 0x08,
	Hub = 0x09,
	CDC_Data = 0x0A,
	SmartCard = 0x0B,
	ContentSecurity = 0x0D,
	Video = 0x0E,
	PersonalHealthcare = 0x0F,
	AVDevices = 0x10,
	BillboardDevice = 0x11,
	USBCBridge = 0x12,
	DiagDevice = 0xDC,
	WirelessController = 0xE0,
	Micellaneous = 0xEF,
	AppSpecific = 0xFE,
	VendorSpecific = 0xFF
};


enum class DeviceClassCodes : uint8_t {
	Device = (uint8_t)ClassCodes::Device,
	CDC = (uint8_t)ClassCodes::CDC_Control,
	Hub = (uint8_t)ClassCodes::Hub,
	BillboardDevice = (uint8_t)ClassCodes::BillboardDevice,
	DiagDevice = (uint8_t)ClassCodes::DiagDevice,
	Micellaneous = (uint8_t)ClassCodes::Micellaneous,
	VendorSpecific = (uint8_t)ClassCodes::VendorSpecific,
};

struct DescriptorItem {
	void* data;
	size_t len;
	DescriptorItem* next;
};

struct DeviceDesc
{
	uint8_t Len;
	uint8_t DeviceType;
	uint16_t USBVersion;
	DeviceClassCodes DeviceClass;
	uint8_t DeviceSubclass;
	uint8_t DeviceProtocol;
	uint8_t MaxControlPckLen;
	uint16_t VendorID;
	uint16_t ProductID;
	uint16_t DeviceVersion;
	uint8_t CompanyStringID;
	uint8_t ProductStringID;
	uint8_t SerialNumStringID;
	uint8_t ConfigurationCount;

} PACKED;

}

#endif /* LIB_CPP_USB_USB_MISC_HPP_ */
