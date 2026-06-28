#pragma once
#include <Arduino.h>

/**
 * PAPERBIT USB-serial file transfer (no Wi-Fi).
 *
 * Extends the main.cpp "CMD:" dispatcher with an "FT:" verb namespace so a host
 * (the Paperbit desktop client) can manage files on the SD card over the USB
 * cable alone. All SD access goes through HalStorage. Line-based control with
 * OK:/ERR: framing; binary payloads are length-prefixed (logging is muted during
 * a transfer so log lines can't corrupt the byte stream). WRITE is stop-and-wait
 * ACK flow-controlled — which costs nothing here because the SD write is ~3x
 * slower than the USB link, so the link is idle while each chunk is written.
 *
 * See docs (paperbit/docs/usb-serial-file-transfer.md) for the full protocol.
 */
namespace SerialFileTransfer {

// Handle a command line with the leading "CMD:" already stripped (e.g. "FT:LIST:/").
// Returns true if it was an FT command (handled), false otherwise.
bool handle(const String& cmd);

// True while a host session is active and not timed out. The main loop ORs this
// into its activity check to suspend auto-sleep during a transfer session.
bool keepAwake();

}  // namespace SerialFileTransfer
