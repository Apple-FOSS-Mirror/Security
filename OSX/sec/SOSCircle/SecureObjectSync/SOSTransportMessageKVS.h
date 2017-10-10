

#ifndef sec_SOSTransportMessageKVS_h
#define sec_SOSTransportMessageKVS_h
#include <Security/SecureObjectSync/SOSAccountPriv.h>
#import <Security/SecureObjectSync/SOSTransportMessage.h>
@class SOSMessage;

@interface SOSMessageKVS : SOSMessage
{
    CFMutableDictionaryRef pending_changes;
}
@property (atomic) CFMutableDictionaryRef pending_changes;

-(CFIndex) SOSTransportMessageGetTransportType;
-(CFStringRef) SOSTransportMessageGetCircleName;
-(CFTypeRef) SOSTransportMessageGetEngine;
-(SOSAccount*) SOSTransportMessageGetAccount;
-(bool) SOSTransportMessageKVSAppendKeyInterest:(SOSMessageKVS*) transport ak:(CFMutableArrayRef) alwaysKeys firstUnlock:(CFMutableArrayRef) afterFirstUnlockKeys
                                       unlocked:(CFMutableArrayRef) unlockedKeys err:(CFErrorRef *)localError;

@end
#endif
