//
//  der_dictionary.c
//  utilities
//
//  Created by Mitch Adler on 6/18/12.
//  Copyright (c) 2012 Apple Inc. All rights reserved.
//

#include <stdio.h>

#include "utilities/SecCFRelease.h"
#include "utilities/der_plist.h"
#include "utilities/der_plist_internal.h"
#include "utilities/SecCFWrappers.h"

#include <corecrypto/ccder.h>
#include <CoreFoundation/CoreFoundation.h>

static const uint8_t* der_decode_key_value(CFAllocatorRef allocator, CFOptionFlags mutability,
                                           CFPropertyListRef* key, CFPropertyListRef* value, CFErrorRef *error,
                                           const uint8_t* der, const uint8_t *der_end)
{
    const uint8_t *payload_end = 0;
    const uint8_t *payload = ccder_decode_constructed_tl(CCDER_CONSTRUCTED_SEQUENCE, &payload_end, der, der_end);
    
    if (NULL == payload) {
        SecCFDERCreateError(kSecDERErrorUnknownEncoding, CFSTR("Unknown data encoding, expected CCDER_CONSTRUCTED_SEQUENCE"), NULL, error);
        return NULL;
    }
    
    CFTypeRef keyObject = NULL;
    CFTypeRef valueObject = NULL;
    
    
    payload = der_decode_plist(allocator, mutability, &keyObject, error, payload, payload_end);
    payload = der_decode_plist(allocator, mutability, &valueObject, error, payload, payload_end);

    if (payload != NULL) {
        *key = keyObject;
        *value = valueObject;
    } else {
        CFReleaseNull(keyObject);
        CFReleaseNull(valueObject);
    }
    return payload;
}

const uint8_t* der_decode_dictionary(CFAllocatorRef allocator, CFOptionFlags mutability,
                                     CFDictionaryRef* dictionary, CFErrorRef *error,
                                     const uint8_t* der, const uint8_t *der_end)
{
    if (NULL == der)
        return NULL;

    const uint8_t *payload_end = 0;
    const uint8_t *payload = ccder_decode_constructed_tl(CCDER_CONSTRUCTED_SET, &payload_end, der, der_end);

    if (NULL == payload) {
        SecCFDERCreateError(kSecDERErrorUnknownEncoding, CFSTR("Unknown data encoding, expected CCDER_CONSTRUCTED_SET"), NULL, error);
        return NULL;
    }
    
    
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(allocator, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    
    if (NULL == dict) {
        SecCFDERCreateError(kSecDERErrorUnderlyingError, CFSTR("Failed to create data"), NULL, error);
        payload = NULL;
        goto exit;
    }

    while (payload != NULL && payload < payload_end) {
        CFTypeRef key = NULL;
        CFTypeRef value = NULL;
        
        payload = der_decode_key_value(allocator, mutability, &key, &value, error, payload, payload_end);
        
        if (payload) {
            CFDictionaryAddValue(dict, key, value);
            CFReleaseNull(key);
            CFReleaseNull(value);
        }
    }
    
    
exit:
    if (payload == payload_end) {
        *dictionary = dict;
        dict = NULL;
    }

    CFReleaseNull(dict);

    return payload;
}

struct size_context {
    bool   success;
    size_t size;
    CFErrorRef *error;
};

static size_t der_sizeof_key_value(CFTypeRef key, CFTypeRef value, CFErrorRef *error) {
    size_t key_size = der_sizeof_plist(key, error);
    if (key_size == 0) {
        return 0;
    }
    size_t value_size = der_sizeof_plist(value, error);
    if (value_size == 0) {
        return 0;
    }
    return ccder_sizeof(CCDER_CONSTRUCTED_SEQUENCE, key_size + value_size);
}

static void add_key_value_size(const void *key_void, const void *value_void, void *context_void)
{
    CFTypeRef key = (CFTypeRef) key_void;
    CFTypeRef value = (CFTypeRef) value_void;
    struct size_context *context = (struct size_context*) context_void;

    if (!context->success)
        return;

    size_t kv_size = der_sizeof_key_value(key, value, context->error);
    if (kv_size == 0) {
        context->success = false;
        return;
    }

    context->size += kv_size;
}

size_t der_sizeof_dictionary(CFDictionaryRef dict, CFErrorRef *error)
{
    struct size_context context = { .success = true, .size = 0, .error = error };

    
    CFDictionaryApplyFunction(dict, add_key_value_size, &context);
    
    if (!context.success)
        return 0;

    return ccder_sizeof(CCDER_CONSTRUCTED_SET, context.size);
}

static uint8_t* der_encode_key_value(CFPropertyListRef key, CFPropertyListRef value, CFErrorRef *error,
                                     const uint8_t* der, uint8_t *der_end)
{
    return ccder_encode_constructed_tl(CCDER_CONSTRUCTED_SEQUENCE, der_end, der,
           der_encode_plist(key, error, der,
           der_encode_plist(value, error, der, der_end)));
}

struct encode_context {
    bool         success;
    CFErrorRef * error;
    CFMutableArrayRef list;
    CFAllocatorRef allocator;
};

static void add_sequence_to_array(const void *key_void, const void *value_void, void *context_void)
{
    struct encode_context *context = (struct encode_context *) context_void;
    if (context->success) {
        CFTypeRef key = (CFTypeRef) key_void;
        CFTypeRef value = (CFTypeRef) value_void;

        size_t der_size = der_sizeof_key_value(key, value, context->error);
        if (der_size == 0) {
            context-> success = false;
        } else {
            CFMutableDataRef encoded_kv = CFDataCreateMutable(context->allocator, der_size);
            CFDataSetLength(encoded_kv, der_size);

            uint8_t* const encode_begin = CFDataGetMutableBytePtr(encoded_kv);
            uint8_t* encode_end = encode_begin + der_size;

            encode_end = der_encode_key_value(key, value, context->error, encode_begin, encode_end);

            if (encode_end != NULL) {
                CFDataDeleteBytes(encoded_kv, CFRangeMake(0, (encode_end - encode_begin)));
                CFArrayAppendValue(context->list, encoded_kv);
            } else {
                context-> success = false;
            }

            CFReleaseNull(encoded_kv);
        }
    }
}

static CFComparisonResult cfdata_compare_contents(const void *val1, const void *val2, void *context __unused)
{
    return CFDataCompare((CFDataRef) val1, (CFDataRef) val2);
}


uint8_t* der_encode_dictionary(CFDictionaryRef dictionary, CFErrorRef *error,
                               const uint8_t *der, uint8_t *der_end)
{
    CFMutableArrayRef elements = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    
    struct encode_context context = { .success = true, .error = error, .list = elements };
    CFDictionaryApplyFunction(dictionary, add_sequence_to_array, &context);
    
    if (!context.success) {
        CFReleaseNull(elements);
        return NULL;
    }
    
    CFRange allOfThem = CFRangeMake(0, CFArrayGetCount(elements));

    CFArraySortValues(elements, allOfThem, cfdata_compare_contents, NULL);

    uint8_t* original_der_end = der_end;

    for(CFIndex position = CFArrayGetCount(elements); position > 0;) {
        --position;
        CFDataRef data = CFArrayGetValueAtIndex(elements, position);
        der_end = ccder_encode_body(CFDataGetLength(data), CFDataGetBytePtr(data), der, der_end);
    }

    CFReleaseNull(elements);

    return ccder_encode_constructed_tl(CCDER_CONSTRUCTED_SET, original_der_end, der, der_end);

}
