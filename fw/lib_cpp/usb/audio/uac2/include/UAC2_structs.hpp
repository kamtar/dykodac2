/*
 * UAC2_structs.hpp
 *
 *  Created on: 1 Feb 2021
 *      Author: Kamtar
 */

#ifndef LIB_CPP_USB_AUDIO_UAC2_INCLUDE_UAC2_STRUCTS_HPP_
#define LIB_CPP_USB_AUDIO_UAC2_INCLUDE_UAC2_STRUCTS_HPP_
#include "usb_misc.hpp"


struct empty {

} PACKED;
namespace KamtarUSB {
namespace UAC2 {

struct chClusterDesc { //Table 4-1: Audio Channel Cluster Descriptor
	uint8_t bNrChannels;
	uint32_t bmChannelConfig;
	uint8_t iChannelNames;
} PACKED;

struct stIfAssocDesc { //Table 4-3: Standard Interface Association Descriptor
	uint8_t bLength;
	const uint8_t bDescriptorType = DescType::InterfaceAssociation;
	uint8_t bFirstInterface;	//Interface number of the first interface that is associated with this function.
	uint8_t bInterfaceCount;	//Number of contiguous interfaces that are associated with this function.
	uint8_t bFunctionClass;		//AUDIO_FUNCTION Function Class code (assigned by this specification). See Appendix A.1, “Audio Function Class Code”.
	uint8_t bFunctionSubClass;	//FUNCTION_SUBCLASS_UNDEFINED Function Subclass code. Currently not used. See Appendix A.2, “Audio Function Subclass Codes”.
	uint8_t bFunctionProtocol;	//AF_VERSION_02_00 Function Protocol code. Indicates the current version of the specification. See Appendix A.3, “Audio Function Protocol Codes”
	uint8_t iFunction;			//Index of a string descriptor that describes this interface.
} PACKED;

struct stACIfDesc { //Table 4-4: Standard AC Interface Descriptor
	uint8_t bLength;
	const uint8_t bDescriptorType = DescType::Interface;
	uint8_t bInterfaceNumber;		//Number of interface. A zero-based value identifying the index in the array of concurrent interfaces supported by this configuration.
	uint8_t	bAlternateSetting;		//Value used to select an Alternate Setting for the interface identified in the prior field. Must be set to 0.
	uint8_t bNumEndpoints;			//Number of endpoints used by this interface (excluding endpoint 0). This number is either 0 or 1 if the optional interrupt endpoint is present.
	uint8_t bInterfaceClass;		//AUDIO. Audio Interface Class code (assigned by the USB). See Appendix A.4, “Audio Interface Class Code.”
	uint8_t bInterfaceSubClass;		//AUDIOCONTROL. Audio Interface Subclass code. Assigned by this specification. See Appendix A.5, “Audio Interface Subclass Codes.”
	uint8_t bInterfaceProtocol;		//IP_VERSION_02_00 Interface Protocol code. Indicates the current version of the specification. See Appendix A.6, “Audio Interface Protocol Codes”
	uint8_t iInterface;				//Index of a string descriptor that describes this interface.
} PACKED;

struct csACInterfaceHeadDesc { //Table 4-5: Class-Specific AC Interface Header Descriptor
	uint8_t bLength;
	const uint8_t bDescriptorType = DescType::ClassInterface;
	const uint8_t bDescriptorSubtype = 0;//TODO
	uint16_t bcdADC;		//Audio Device Class Specification Release Number in Binary-Coded Decimal.
	uint8_t bCategory;		//Constant, indicating the primary use of this audio function, as intended by the manufacturer. See Appendix A.7, “Audio Function Category Codes.”
	uint16_t wTotalLength;	//Total number of bytes returned for the class-specific AudioControl interface descriptor. Includes the combined length of this descriptor header and all Clock Source, Unit and Terminal descriptors.

	union {					//bmControls - D1..0: Latency Control D7..2: Reserved. Must be set to 0.
	    struct {
	    	uint8_t latency_control: 2;
	    	uint8_t _reserved: 6;
	    };
	    uint8_t bmControls;
	};
} PACKED;

}
}

#endif /* LIB_CPP_USB_AUDIO_UAC2_INCLUDE_UAC2_STRUCTS_HPP_ */
