/*
 * USBManager.cpp
 *
 *  Created on: 27 Feb 2021
 *      Author: Kamtar
 */

#include "USBManager.hpp"
#include <assert.h>
#include <string.h>

//LANGID descriptor with only English(0x0409) supported
__attribute__ ((aligned (4))) uint8_t USBManager::s_string0[4] = {4, (uint8_t)DescType::String, 0x09, 0x04};

void USBManager::Init(uint8_t usb_id)
{
	s_usb_id = usb_id;
	s_list_n = 0;
	s_ConfigDescLen = 0;

	s_dev_desc = DeviceDesc {
			.Len = sizeof(DeviceDesc),
			.DeviceType = 1,
			.USBVersion = 0x200,
			.DeviceClass = DeviceClassCodes::CDC,
			.DeviceSubclass = 0x00,
			.DeviceProtocol = 0x00,
			.MaxControlPckLen = 64,
			.VendorID = 0x1FC9,
			.ProductID = 0x0094,
			.DeviceVersion = 100,
			.CompanyStringID = VendorNameIndex,
			.ProductStringID = DeviceNameIndex,
			.SerialNumStringID = 0,
			.ConfigurationCount = 1
	};

	SetVendorString("Kamtar");
	SetDeviceString("DykoDAC2");
}

void USBManager::SetVendorString(const char* str)
{
	assert(str[0] != '\0');

	int i = 2;
	int e = 0;

	while(str[e] != '\0')
	{
		s_VendorString[i] = str[e];
		i++;
		s_VendorString[i] = 0;
		i++;
		e++;
	}

	s_VendorString[0] = i;
	s_VendorString[1] = (uint8_t)DescType::String;
}

void USBManager::SetDeviceString(const char* str)
{
	assert(str[0] != '\0');

	int i = 2;
	int e = 0;

	while(str[e] != '\0')
	{
		s_DeviceString[i] = str[e];
		i++;
		s_DeviceString[i] = 0;
		i++;
		e++;
	}

	s_DeviceString[0] = i;
	s_DeviceString[1] = (uint8_t)DescType::String;
}

bool USBManager::register_Descriptor(uint8_t* data, size_t len)
{
	for(int i=s_ConfigDescLen; i<(s_ConfigDescLen+len); i++)
	{
		s_ConfigDestriptors[i] = data[i];
	}

	s_ConfigDescLen += len;
	return true;
}

usb_status_t USBManager::USB_DeviceGetDescriptor(usb_device_handle handle,  usb_setup_struct_t *setup, uint32_t *out_length, uint8_t **out_buffer)
{
    uint8_t descriptorType  = (uint8_t)((setup->wValue & 0xFF00U) >> 8U);
    uint8_t descriptorIndex = (uint8_t)((setup->wValue & 0x00FFU));
    usb_status_t ret        = kStatus_USB_Success;

    if (USB_REQUEST_STANDARD_GET_DESCRIPTOR != setup->bRequest)
    {
        return kStatus_USB_InvalidRequest;
    }

    switch (descriptorType)
    {
        case USB_DESCRIPTOR_TYPE_STRING:
        {
        	switch(descriptorIndex)
        	{
        	case 0:
        		 *out_buffer = s_string0;
        		 *out_length = 4;
        		 break;
        	case VendorNameIndex:
        	    *out_buffer = s_VendorString;
        	    *out_length = s_VendorString[0]-2;
        	    break;
        	case DeviceNameIndex:
        	     *out_buffer = s_DeviceString;
        	     *out_length = s_DeviceString[0]-2;
        	     break;
        	}

        }
        break;
        case USB_DESCRIPTOR_TYPE_DEVICE:
        {
            *out_buffer = (uint8_t*)(&s_dev_desc);
            *out_length = s_dev_desc.Len;
        }
        break;
        case USB_DESCRIPTOR_TYPE_CONFIGURE:
        {
            *out_buffer = s_ConfigDestriptors;
            *out_length = s_ConfigDescLen;
        }
        break;
        default:
            ret = kStatus_USB_InvalidRequest;
            break;
    } /* End Switch */
    return ret;
}

