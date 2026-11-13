// AppLink.h - the desktop app's side of the USB serial port (docs/APP.md, protocol/PROTOCOL.md).
//
// The app (Volfovich NAM Forge) talks in binary frames on the same port as the typed command
// shell. SerialUI hands every received byte to appLinkFeed() first; what is not part of a
// frame comes back as text for the shell.
//
// Runs in loop() only. It calls the same functions the shell calls (setParam, ...), and
// finds out about changes, whoever made them, by comparing every setting and the loaded
// files with what it last reported.
#pragma once

#include <stdint.h>

// One byte from the serial port. True: it belonged to a frame (which may now have been
// answered). False: it is text, give it to the shell.
bool appLinkFeed(uint8_t byte);

// Call often from loop(): the line-idle and session timeouts, and the change events.
void serviceAppLink();
