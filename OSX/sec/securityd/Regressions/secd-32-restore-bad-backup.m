/*
 * Copyright (c) 2008-2010,2013 Apple Inc. All rights reserved.
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
 */


#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecBase.h>
#include <Security/SecItem.h>
#include <Security/SecItemPriv.h>
#include <Security/SecInternal.h>
#include <utilities/SecFileLocations.h>
#include <utilities/SecCFWrappers.h>

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>

#include "secd_regressions.h"

#include <securityd/SecItemServer.h>

#include "SecdTestKeychainUtilities.h"

/* Keybag and exported plist data. */
static const unsigned char keybag_data[] = {
    0x56, 0x45, 0x52, 0x53, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03,
    0x54, 0x59, 0x50, 0x45, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01,
    0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0xf9, 0xcb,
    0xd8, 0x76, 0x47, 0x01, 0xaa, 0xc5, 0xcf, 0xe5, 0x14, 0xf4, 0xf2, 0x98,
    0x48, 0x4d, 0x43, 0x4b, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x41, 0x4c, 0x54, 0x00, 0x00, 0x00, 0x14, 0xbb, 0xfd, 0xa3, 0x3e,
    0x32, 0xa7, 0x80, 0x48, 0xd1, 0x2a, 0x39, 0x4b, 0x78, 0x6b, 0x35, 0x11,
    0x27, 0x62, 0x38, 0xe4, 0x49, 0x54, 0x45, 0x52, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x27, 0x10, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0xba, 0x4e, 0xed, 0x78, 0x38, 0x4a, 0x41, 0x4c, 0x8a, 0x2f, 0x6d, 0x1c,
    0x3a, 0xc9, 0xc8, 0xad, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x0b, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0x75, 0x88, 0x12, 0x0f, 0xbf, 0x21, 0x6b, 0x65, 0x85, 0xd0, 0x67, 0xdf,
    0x9b, 0xd1, 0xb3, 0x40, 0x53, 0x03, 0xaf, 0xb8, 0x8f, 0xe3, 0x5c, 0x97,
    0x43, 0xdd, 0x71, 0x65, 0x27, 0xd3, 0x73, 0xeb, 0x37, 0x5b, 0x29, 0xe8,
    0xd1, 0x14, 0xfe, 0xa3, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x2b, 0x76, 0x49, 0x3c, 0xc2, 0x5c, 0x4e, 0xc8, 0x8d, 0xea, 0x9a, 0x59,
    0x11, 0x98, 0xdd, 0x40, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x0a, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0xee, 0x8f, 0x46, 0xd8, 0x10, 0x17, 0x6e, 0x6c, 0x63, 0xee, 0x04, 0x22,
    0xd4, 0xec, 0x7c, 0x53, 0x8f, 0x2c, 0x18, 0x8d, 0xf3, 0x86, 0xdb, 0xd6,
    0x19, 0xae, 0x1e, 0xe0, 0x45, 0xc7, 0x75, 0x13, 0x8c, 0xb3, 0x95, 0x6f,
    0x21, 0x60, 0xd2, 0x9e, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x3e, 0x91, 0xc1, 0x5d, 0xb9, 0x64, 0x44, 0xb4, 0x81, 0x0f, 0xe5, 0x12,
    0xae, 0x89, 0xb5, 0x73, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x09, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0xc1, 0x22, 0x1e, 0x92, 0x54, 0xc3, 0xd6, 0x04, 0xdc, 0x45, 0x3e, 0x24,
    0xf9, 0x0c, 0xbe, 0x46, 0x8a, 0x02, 0xf7, 0xfc, 0x32, 0x24, 0x6d, 0x21,
    0x57, 0x1a, 0x43, 0xd5, 0x5f, 0xda, 0x8a, 0x5a, 0x33, 0xc0, 0xc8, 0x67,
    0x37, 0x79, 0xfe, 0x57, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x4a, 0xb0, 0xd7, 0xc0, 0xfe, 0xf7, 0x42, 0x4f, 0xb2, 0xd9, 0xd8, 0x85,
    0x70, 0xea, 0x97, 0x74, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x08, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0x45, 0x26, 0xb9, 0xce, 0x3f, 0x7c, 0xd9, 0xbf, 0x92, 0xb0, 0x2e, 0x93,
    0xbc, 0x85, 0xf9, 0xd8, 0xec, 0x30, 0xd2, 0x42, 0x4c, 0x9d, 0x89, 0x77,
    0xbc, 0xe3, 0x66, 0xf2, 0x23, 0x61, 0xad, 0xc7, 0xc7, 0x02, 0xb9, 0x44,
    0x3d, 0x66, 0xd1, 0x6f, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x63, 0xdc, 0x85, 0xdd, 0x5b, 0xcb, 0x49, 0x43, 0xa2, 0x23, 0x93, 0xe7,
    0xbc, 0x88, 0x67, 0x2c, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x07, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0x2d, 0x80, 0xa8, 0xe6, 0x01, 0x32, 0x90, 0x06, 0x63, 0xb2, 0xaf, 0x23,
    0x29, 0xbb, 0x85, 0x2b, 0x8f, 0x03, 0x3c, 0x07, 0xf2, 0xc3, 0xff, 0x8c,
    0xe5, 0x61, 0xa0, 0xec, 0xc3, 0x53, 0x28, 0xd4, 0x98, 0x92, 0x30, 0x41,
    0xab, 0x2b, 0x7a, 0xc9, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x22, 0x05, 0x3e, 0xc4, 0x9c, 0x32, 0x48, 0x8e, 0xad, 0x25, 0xe5, 0xe1,
    0x1d, 0x05, 0xbf, 0x1c, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x06, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0x2a, 0x51, 0x5a, 0x8b, 0x5c, 0x2d, 0x67, 0x49, 0x59, 0xce, 0xf6, 0x77,
    0xb0, 0x22, 0x8b, 0x53, 0x22, 0xfd, 0x5d, 0x1b, 0x6e, 0x97, 0x0c, 0xed,
    0x3a, 0xb5, 0x52, 0xe7, 0x04, 0x31, 0xf6, 0x97, 0x5c, 0x55, 0xf5, 0xcc,
    0xa9, 0xce, 0x37, 0x8c, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0xb8, 0x54, 0xc8, 0xe5, 0x40, 0xc3, 0x4f, 0x15, 0x8d, 0xda, 0xfb, 0x82,
    0x24, 0xe4, 0x84, 0xf3, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x05, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0x97, 0x9d, 0xac, 0x94, 0xdc, 0x34, 0xbc, 0xea, 0x47, 0x1e, 0xf8, 0x9a,
    0x2e, 0xb9, 0x51, 0x60, 0xc7, 0xf3, 0x5f, 0x79, 0x43, 0x9e, 0xc8, 0x80,
    0xad, 0xdd, 0x86, 0x61, 0x73, 0xd1, 0xad, 0xd2, 0xc6, 0x39, 0xa6, 0x94,
    0x5f, 0x3d, 0x8e, 0x0e, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x60, 0x85, 0x58, 0x0e, 0xbb, 0x91, 0x4b, 0x47, 0x84, 0xdc, 0x5a, 0x81,
    0x75, 0x9a, 0xcd, 0x99, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x04, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x02, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0xba, 0x30, 0x0f, 0x71, 0x33, 0x72, 0x12, 0xeb, 0x2f, 0x30, 0x51, 0xd0,
    0x24, 0xfb, 0xba, 0x9b, 0xeb, 0x9b, 0x13, 0x22, 0xbe, 0x20, 0x1f, 0xe2,
    0xaa, 0xfe, 0x46, 0x6f, 0xe9, 0x24, 0x98, 0x74, 0x75, 0xe1, 0xe8, 0x78,
    0xe2, 0xdf, 0x1d, 0x79, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0xba, 0x02, 0xb1, 0xbf, 0x5a, 0x19, 0x47, 0xf9, 0x8e, 0x63, 0x61, 0xbb,
    0x29, 0x1b, 0x11, 0xd3, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x03, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x02, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0xaa, 0x5d, 0xb4, 0x84, 0x93, 0xe9, 0x58, 0xf9, 0xe1, 0xb2, 0xcc, 0xbd,
    0xb0, 0xb5, 0xa5, 0x17, 0xe1, 0x00, 0x86, 0xbc, 0x8c, 0x66, 0x68, 0x6e,
    0x70, 0x4d, 0x65, 0xda, 0x06, 0xb6, 0x1a, 0xc1, 0x63, 0x1d, 0x72, 0xcd,
    0x86, 0x73, 0xd2, 0x94, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x3b, 0x0e, 0x79, 0xc8, 0xc9, 0xbc, 0x4b, 0x75, 0x88, 0x16, 0x89, 0xb8,
    0x69, 0x9b, 0x5e, 0xce, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x02, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x02, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0xb3, 0x0a, 0x5d, 0xb2, 0x3e, 0x63, 0xb1, 0xc5, 0x02, 0xf2, 0x38, 0xbe,
    0x8b, 0xb9, 0xfa, 0x06, 0xcb, 0x41, 0x6f, 0x99, 0xe7, 0x69, 0x12, 0x5f,
    0x6e, 0xef, 0x17, 0x67, 0xe6, 0xf6, 0xe4, 0x61, 0x2b, 0x1d, 0xe7, 0x18,
    0x8a, 0x5d, 0x5f, 0x66, 0x55, 0x55, 0x49, 0x44, 0x00, 0x00, 0x00, 0x10,
    0x45, 0xc1, 0x1e, 0x42, 0x2f, 0xd4, 0x47, 0x56, 0xa6, 0x88, 0x3a, 0x38,
    0x07, 0x86, 0x74, 0xcd, 0x43, 0x4c, 0x41, 0x53, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x01, 0x57, 0x52, 0x41, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x02, 0x4b, 0x54, 0x59, 0x50, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x50, 0x4b, 0x59, 0x00, 0x00, 0x00, 0x28,
    0x92, 0xf6, 0xf2, 0xd3, 0x54, 0x02, 0xa9, 0xb3, 0x15, 0x19, 0x2a, 0x12,
    0x99, 0xb3, 0x81, 0xbc, 0x92, 0x7e, 0x5c, 0x47, 0xd3, 0x56, 0x92, 0x04,
    0xed, 0xbc, 0x5e, 0x22, 0x36, 0x6e, 0x51, 0xd4, 0xbb, 0xad, 0xaa, 0xa3,
    0xbd, 0x28, 0x90, 0x64
};

static const char export_plist[] = "\
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\
<plist version=\"1.0\">\
<dict>\
<key>genp</key>\
<array>\
<dict>\
<key>v_Data</key>\
<data>\
AwAAAAgAAAAoAAAAPjxrzgnoJYiuTfNV0OAii8Jgl8Zegkk93Dwm\
dJo27hIyxqzTT+twzAHn0qbb+uq7IIDFbLZt9ThLJmpGuwMzXKl0\
91YCqoT6d3zPAkSPhPwS29/LFE3hqeGsUyV9CSye3fW9A51b/+uA\
XVD7LQdM9Xv7Def8JO9abBKW42X+l38SW0sOq34/243Hyp3q0VWT\
XN+UojOkzAgsBxPsuHEOre0+9aOe+RzIO2R+s54YG3QaxSwhUOu/\
DcN6raIA37BF0eOFHOlP6ZUH+NzwTWi5ycRyX833b0bMhU4M24yx\
5Z88ysOPWZuD6oqycfo=\
</data>\
<key>v_PersistentRef</key>\
<data>\
Z2VucAAAAAAAAAA1\
</data>\
</dict>\
</array>\
</dict>\
</plist>\
";

/* Test backup-restore case, when item had inconsistently set pdmn attribute (due to another bug),
   and mobile restore restored item with inconsistent attributes and afterwards tried SecItemUpdate()
   on it, which failed, leading to the failure of the whole restore operation.
 */
static void tests(void)
{
    /* custom keychain dir */
    secd_test_setup_temp_keychain("secd_32_restore_bad_backup", ^{});

    /* Restore keychain from plist. */
    CFDataRef keybag = CFDataCreate(kCFAllocatorDefault, keybag_data, sizeof(keybag_data));
    CFDataRef backup = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)export_plist, sizeof(export_plist));
    ok_status(_SecKeychainRestoreBackup(backup, keybag, NULL));
    CFRelease(keybag);
    CFRelease(backup);

    /* The restored item is kind of malformed (pdmn and accc attributes are inconsistent).  Currently adopted way
     of handling of this item is to try to handle it gracefully, this is what this test does (i.e. it checks that
     it is possible to update such item).  Another possibility which might be adopted in the future is dropping such
     item during backup decoding.  In this case, the test should be modified to check that the item does not exist
     in the keychain at all. */

    /* Try to update item with inconsistent accc and pdmn attributes. */
    CFDictionaryRef query = CFDictionaryCreateMutableForCFTypesWith(kCFAllocatorDefault,
                                                                    kSecClass, kSecClassGenericPassword,
                                                                    kSecAttrAccessGroup, CFSTR("com.apple.security.sos"),
                                                                    kSecAttrService, CFSTR("test"),
                                                                    NULL);
    CFDictionaryRef update = CFDictionaryCreateMutableForCFTypesWith(kCFAllocatorDefault,
                                                                     kSecAttrService, CFSTR("updated-test"),
                                                                     NULL);

    ok_status(SecItemUpdate(query, update));
    diag("This still fails - don't be alarmed");
    CFRelease(update);
    CFRelease(query);
}

int secd_32_restore_bad_backup(int argc, char *const *argv)
{
    plan_tests(2 + kSecdTestSetupTestCount);
    tests();

    return 0;
}
