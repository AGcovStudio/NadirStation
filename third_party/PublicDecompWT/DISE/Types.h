/*
 * Copyright 2011-2019, European Organisation for the Exploitation of Meteorological Satellites (EUMETSAT)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _TYPES_H_
#define _TYPES_H_

// NADIR Station modification (2026-11-15): with PUBLICDECOMPWT_NO_TIME only the integer types are
// set up. The decompressors need nothing else, and the time classes would pull <windows.h> (and its
// min/max macros) into every file that includes CDataField.h under MinGW.
#if !defined(PUBLICDECOMPWT_NO_TIME)
#ifdef WIN32
#pragma message("WINDOWS BUILD")
#include "MSGTime.h"
typedef Util::CMSGTime SYSTIME;
typedef Util::CMSGTimeSpan SYSTIMESPAN;
#else
#include "UTCTime.h"
typedef  Util::CUTCTime SYSTIME;
typedef  Util::CTimeSpan SYSTIMESPAN;
#endif
#endif
#if !defined(_MSC_VER)
#ifndef __int64
#define __int64 long long
#endif
#ifndef __int32
#define __int32 int
#endif
#ifndef __int16
#define __int16 short
#endif
#ifndef __int8
#define __int8 char
#endif
#endif

#endif
