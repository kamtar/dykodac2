/*
 * UsbDevice.cpp
 *
 *  Created on: 18 Mar 2021
 *      Author: Kamtar
 */

#include <usb/UsbDevice.hpp>

namespace KamtarUSB {

UsbDevice::UsbDevice()
{
	// TODO Auto-generated constructor stub

}

void UsbDevice::Init()
{
	if(m_init == true)
		return;

	 USB_DeviceInit(CONTROLLER_ID, s_device_callback, &m_dev_ptr);

	 s_instance_list[s_instance_num]._m = this;
	 s_instance_list[s_instance_num].dev = m_dev_ptr;
	 s_instance_num++;
	 m_init = true;
}

usb_status_t UsbDevice::s_device_callback(usb_device_handle handle, uint32_t callbackEvent, void *eventParam)
{
	for(int i=0;i<s_instance_num;i++)
	{
		if( s_instance_list[s_instance_num].dev == handle)
			return s_instance_list[s_instance_num]._m->device_callback(handle, callbackEvent, eventParam);

	}
}

} /* namespace KamtarUSB */
