/*
 * Copyright (c) 2010, Sangoma Technologies
 * Moises Silva <moy@sangoma.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __FTDM_DECLARE_H__
#define __FTDM_DECLARE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ######## start utility macros not to be used by customers, but needed in this header, may be move to another header ############ */

#ifndef __WINDOWS__
#if defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32) || defined(_WIN64)
#define __WINDOWS__
#endif
#endif

#ifdef _MSC_VER
#if defined(FT_DECLARE_STATIC)
#define FT_DECLARE(type)			type __stdcall
#define FT_DECLARE_NONSTD(type)		type __cdecl
#define FT_DECLARE_DATA
#elif defined(FREETDM_EXPORTS)
#define FT_DECLARE(type)			__declspec(dllexport) type __stdcall
#define FT_DECLARE_NONSTD(type)		__declspec(dllexport) type __cdecl
#define FT_DECLARE_DATA				__declspec(dllexport)
#else
#define FT_DECLARE(type)			__declspec(dllimport) type __stdcall
#define FT_DECLARE_NONSTD(type)		__declspec(dllimport) type __cdecl
#define FT_DECLARE_DATA				__declspec(dllimport)
#endif
#define EX_DECLARE_DATA				__declspec(dllexport)
#else
#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(HAVE_VISIBILITY)
#define FT_DECLARE(type)		__attribute__((visibility("default"))) type
#define FT_DECLARE_NONSTD(type)	__attribute__((visibility("default"))) type
#define FT_DECLARE_DATA		__attribute__((visibility("default")))
#else
#define FT_DECLARE(type)		type
#define FT_DECLARE_NONSTD(type)	type
#define FT_DECLARE_DATA
#endif
#define EX_DECLARE_DATA
#endif

#define FTDM_STR2ENUM_P(_FUNC1, _FUNC2, _TYPE) FT_DECLARE(_TYPE) _FUNC1 (const char *name); FT_DECLARE(const char *) _FUNC2 (_TYPE type);
#define FTDM_STR2ENUM(_FUNC1, _FUNC2, _TYPE, _STRINGS, _MAX)	\
	FT_DECLARE(_TYPE) _FUNC1 (const char *name)							\
	{														\
		int i;												\
		_TYPE t = _MAX ;									\
															\
		for (i = 0; i < _MAX ; i++) {						\
			if (!strcasecmp(name, _STRINGS[i])) {			\
				t = (_TYPE) i;								\
				break;										\
			}												\
		}													\
															\
		return t;											\
	}														\
	FT_DECLARE(const char *) _FUNC2 (_TYPE type)						\
	{														\
		if (type > _MAX) {									\
			type = _MAX;									\
		}													\
		return _STRINGS[(int)type];							\
	}														\

#ifdef WIN32
#include <windows.h>
#define FTDM_INVALID_SOCKET INVALID_HANDLE_VALUE
typedef HANDLE ftdm_socket_t;
typedef unsigned __int64 uint64_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int8 uint8_t;
typedef __int64 int64_t;
typedef __int32 int32_t;
typedef __int16 int16_t;
typedef __int8 int8_t;
#else
#define FTDM_INVALID_SOCKET -1
typedef int ftdm_socket_t;
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#endif

typedef struct ftdm_channel ftdm_channel_t;
typedef struct ftdm_span ftdm_span_t;
typedef struct ftdm_event ftdm_event_t;
typedef struct ftdm_conf_node ftdm_conf_node_t;
typedef struct ftdm_group ftdm_group_t;
typedef size_t ftdm_size_t;
typedef struct ftdm_sigmsg ftdm_sigmsg_t;
typedef struct ftdm_io_interface ftdm_io_interface_t;
typedef struct ftdm_stream_handle ftdm_stream_handle_t;
typedef struct ftdm_queue ftdm_queue_t;

#ifdef __cplusplus
} /* extern C */
#endif

#endif

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
