#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Asks for the PC's address and downloads every video it shares into sdmc:/videos/.
// Returns true if anything was downloaded; first = full path of the first video.
bool net_download(char *first, size_t firstLen);
void net_exit(void);
