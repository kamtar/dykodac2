/*
 * USBManager.cpp
 *
 *  Created on: 27 Feb 2021
 *      Author: Kamtar
 */

#include "USBManager.hpp"
#include <assert.h>

USBManager::USBManager(uint8_t usb_id)

{
	m_usb_id = usb_id;
	m_list_n = 0;
}

bool USBManager::register_Descriptor(DescriptorItem& i)
{
	assert(m_list_n<DescriptorListMax);

	m_desc_list[m_list_n] = i;
	m_list_n++;
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
            if (descriptorIndex == 0)
            {
                *out_buffer = (uint8_t *)g_UsbDeviceLanguageList.languageString;
                *out_length = g_UsbDeviceLanguageList.stringLength;
            }
            else
            {
                uint8_t langId    = 0;
                uint8_t langIndex = USB_DEVICE_STRING_COUNT;

                for (; langId < USB_DEVICE_LANGUAGE_COUNT; langId++)
                {
                    if (setup->wIndex == g_UsbDeviceLanguageList.languageList[langId].languageId)
                    {
                        if (descriptorIndex < USB_DEVICE_STRING_COUNT)
                        {
                            langIndex = descriptorIndex;
                        }
                        break;
                    }
                }

                if (USB_DEVICE_STRING_COUNT == langIndex)
                {
                    langId = 0;
                }
                *out_buffer = (uint8_t *)g_UsbDeviceLanguageList.languageList[langId].string[langIndex];
                *out_length = g_UsbDeviceLanguageList.languageList[langId].length[langIndex];
            }
        }
        break;
        case USB_DESCRIPTOR_TYPE_DEVICE:
        {
            *out_buffer = g_UsbDeviceDescriptor;
            *out_length = USB_DESCRIPTOR_LENGTH_DEVICE;
        }
        break;
        case USB_DESCRIPTOR_TYPE_CONFIGURE:
        {
            *out_buffer = g_UsbDeviceConfigurationDescriptor;
            *out_length = USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL;
        }
        break;
        default:
            ret = kStatus_USB_InvalidRequest;
            break;
    } /* End Switch */
    return ret;
}

