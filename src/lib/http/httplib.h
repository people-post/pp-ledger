//
//  httplib.h
//
//  Copyright (c) 2026 Yuji Hirose. All rights reserved.
//  MIT License
//
//  Umbrella header for the pp-ledger fork of cpp-httplib 0.32.
//  Prefer including server.h / client.h when possible.
//

#ifndef CPPHTTPLIB_HTTPLIB_H
#define CPPHTTPLIB_HTTPLIB_H

/*
 * Platform compatibility check
 */

#if defined(_WIN32) && !defined(_WIN64)
#if defined(_MSC_VER)
#pragma message(                                                               \
    "cpp-httplib doesn't support 32-bit Windows. Please use a 64-bit compiler.")
#else
#warning                                                                       \
    "cpp-httplib doesn't support 32-bit Windows. Please use a 64-bit compiler."
#endif
#elif defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ < 8
#warning                                                                       \
    "cpp-httplib doesn't support 32-bit platforms. Please use a 64-bit compiler."
#elif defined(__SIZEOF_SIZE_T__) && __SIZEOF_SIZE_T__ < 8
#warning                                                                       \
    "cpp-httplib doesn't support platforms where size_t is less than 64 bits."
#endif

#ifdef _WIN32
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0A00
#error                                                                         \
    "cpp-httplib doesn't support Windows 8 or lower. Please use Windows 10 or later."
#endif
#endif

#include "ws.h"

#endif // CPPHTTPLIB_HTTPLIB_H
