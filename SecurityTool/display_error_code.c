/*
 * Copyright (c) 2003-2009,2012,2014 Apple Inc. All Rights Reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 * leaks.c
 */

#include "display_error_code.h"
#include "security_tool.h"
#include <Security/cssmapple.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <libkern/OSByteOrder.h>

// cssmErrorString
#include <Security/SecBasePriv.h>


int display_error_code(int argc, char *const *argv)
{
	CSSM_RETURN error;
	int ix = 0;

	for (ix = 0; ix < argc; ix++)
	{
		if (strcmp("error", argv[ix])==0)
			continue;
		// set base to 0 to have it interpret radix automatically
		error = (CSSM_RETURN) strtoul(argv[ix], NULL, 0);
		printf("Error: 0x%08X %d %s\n", error, error, cssmErrorString(error));
	}

	return 1;
}
