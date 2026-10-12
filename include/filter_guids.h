#pragma once

#include <windows.h>
#include <unknwn.h>
#include <dshow.h>
#include <initguid.h>

// GUID for the LAN Phone Camera Filter: {708B7B2B-191B-4E3F-8C7E-F037A32F5112}
DEFINE_GUID(CLSID_LanCamFilter, 
0x708b7b2b, 0x191b, 0x4e3f, 0x8c, 0x7e, 0xf0, 0x37, 0xa3, 0x2f, 0x51, 0x12);

// GUID for the Output Pin: {9443A3A6-C5A1-4F44-9382-7494519967A2}
DEFINE_GUID(CLSID_LanCamPin,
0x9443a3a6, 0xc5a1, 0x4f44, 0x93, 0x82, 0x74, 0x94, 0x51, 0x99, 0x67, 0xa2);
