//
//  SOSAccountTrustClassic+Identity.h
//  Security
//

#ifndef SOSAccountTrustClassic_Identity_h
#define SOSAccountTrustClassic_Identity_h

#import "Security/SecureObjectSync/SOSAccountTrustClassic.h"

@class SOSAccountTrustClassic;

@interface SOSAccountTrustClassic (Identity)
//FullPeerInfo
-(bool) updateFullPeerInfo:(SOSAccount*)account minimum:(CFSetRef)minimumViews excluded:(CFSetRef)excludedViews;
-(SOSFullPeerInfoRef) getMyFullPeerInfo;
-(bool) fullPeerInfoVerify:(SecKeyRef) privKey err:(CFErrorRef *)error;
-(bool) hasFullPeerInfo:(CFErrorRef*) error;
-(SOSFullPeerInfoRef) CopyAccountIdentityPeerInfo;
-(bool) ensureFullPeerAvailable:(CFDictionaryRef)gestalt deviceID:(CFStringRef)deviceID backupKey:(CFDataRef)backup err:(CFErrorRef *) error;
-(bool) isMyPeerActive:(CFErrorRef*) error;
-(void) purgeIdentity;

- (void)ensureOctagonPeerKeys:(SOSKVSCircleStorageTransport*)circleTransport;

@end


#endif /* SOSAccountTrustClassic_Identity_h */
