#pragma once

// Preprocessor-visible portion of the single AudioFormat/FormatConfig table.
// TinyUSB's compile-time configuration consumes these same values.
#define DYKODAC_RATE_44K1 44100U
#define DYKODAC_RATE_48K 48000U
#define DYKODAC_OSC_44K1 22579200U
#define DYKODAC_OSC_48K 24576000U
#define DYKODAC_CHANNELS 2U
#define DYKODAC_SUBSLOT_BYTES 4U
#define DYKODAC_VALID_BITS 24U
#define DYKODAC_USB_HS_MAX_PACKET 56U
