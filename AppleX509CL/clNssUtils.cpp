/*
 * Copyright (c) 2003 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */

/*
 * clNssUtils.cpp - support for libnssasn1-based ASN1 encode/decode
 */

#include "clNssUtils.h"
#include "clNameUtils.h"
#include "CSPAttacher.h"
#include <SecurityNssAsn1/secasn1.h>  
#include <SecurityNssAsn1/SecNssCoder.h>
#include <SecurityNssAsn1/nssUtils.h>
#include <SecurityNssAsn1/keyTemplates.h>
#include <SecurityNssAsn1/certExtensionTemplates.h>
#include <Security/oidsalg.h>
#include <Security/cssmapple.h>
#include <string.h>

#pragma mark ----- ArenaAllocator -----

/* 
 * Avoid inlining this for debuggability 
 */
void *ArenaAllocator::malloc(size_t len) throw(std::bad_alloc)
{
	try {
		return mCoder.malloc(len);
	}
	catch (...) {
		throw std::bad_alloc();
	}
}

/* intentionally not implemented, should never be called */
void ArenaAllocator::free(void *p) throw()
{
	throw std::bad_alloc();
}
	
void *ArenaAllocator::realloc(void *p, size_t len) throw(std::bad_alloc)
{
	throw std::bad_alloc();
}

#pragma mark ----- Malloc/Copy/Compare CSSM_DATA -----

/* 
 * Misc. alloc/copy with arbitrary CssmAllocator 
 */
/* malloc d.Data, set d.Length */
void clAllocData(
	CssmAllocator	&alloc,
	CSSM_DATA		&dst,
	size_t			len)
{
	if(len == 0) {
		dst.Data = NULL;
	}
	else {
		dst.Data = (uint8 *)alloc.malloc(len);
	}
	dst.Length = len;
}

/* malloc and copy */
void clAllocCopyData(
	CssmAllocator	&alloc,
	const CSSM_DATA	&src,
	CSSM_DATA		&dst)
{
	clAllocData(alloc, dst, src.Length);
	if(dst.Length != 0) {
		memmove(dst.Data, src.Data, src.Length);
	}
}

/*
 * Compare two CSSM_DATAs (or two CSSM_OIDs), return true if identical.
 */
bool clCompareCssmData(
	const CSSM_DATA *data1,
	const CSSM_DATA *data2)
{	
	if((data1 == NULL) || (data1->Data == NULL) || 
	   (data2 == NULL) || (data2->Data == NULL) ||
	   (data1->Length != data2->Length)) {
		return false;
	}
	if(data1->Length != data2->Length) {
		return false;
	}
	if(memcmp(data1->Data, data2->Data, data1->Length) == 0) {
		return true;
	}
	else {
		return false;
	}
}

#pragma mark ----- CSSM_DATA <--> uint32 -----

uint32 clDataToInt(
	const CSSM_DATA &cdata, 
	CSSM_RETURN toThrow)	/* = CSSMERR_CL_INVALID_CERT_POINTER */
{
	if((cdata.Length == 0) || (cdata.Data == NULL)) {
		return 0;
	}
	uint32 len = cdata.Length;
	if(len > sizeof(uint32)) {
		CssmError::throwMe(toThrow);
	}
	
	uint32 rtn = 0;
	uint8 *cp = cdata.Data;
	for(uint32 i=0; i<len; i++) {
		rtn = (rtn << 8) | *cp++;
	}
	return rtn;
}

void clIntToData(
	uint32 num,
	CSSM_DATA &cdata,
	CssmAllocator &alloc)
{
	uint32 len = 0;
	
	if(num < 0x100) {
		len = 1;
	}
	else if(num < 0x10000) {
		len = 2;
	}
	else if(num < 0x1000000) {
		len = 3;
	}
	else {
		len = 4;
	}
	clAllocData(alloc, cdata, len);
	uint8 *cp = &cdata.Data[len - 1];
	for(unsigned i=0; i<len; i++) {
		*cp-- = num & 0xff;
		num >>= 8;
	}
}

#pragma mark ----- CSSM_BOOL <--> CSSM_DATA -----
/*
 * A Bool is encoded as one byte of either 0 or 0xff
 * Default of NSS boolean not present is false
 */
CSSM_BOOL clNssBoolToCssm(
	const CSSM_DATA	&nssBool)
{
	if((nssBool.Data != NULL) && (nssBool.Data[0] == 0xff)) {
		return CSSM_TRUE;
	}
	else {
		return CSSM_FALSE;
	}
}

void clCssmBoolToNss(
	CSSM_BOOL cBool,
	CSSM_DATA &nssBool,
	CssmAllocator &alloc)
{
	uint32 num = cBool ? 0xff : 0;
	clIntToData(num, nssBool, alloc);
}

#pragma mark ----- Bit String manipulation -----

/*
 * Adjust the length of a CSSM_DATA representing a pre-encoded 
 * bit string. On entry the length field is the number of bytes
 * of data; en exit, the number if bits. Trailing zero bits 
 * are counted as unused (which is how KeyUsage and NetscapeCertType
 * extensions are encoded).
 */
void clCssmBitStringToNss(
	CSSM_DATA &b)
{
	int numBits = b.Length * 8;
	
	/* start at end of bit array, scanning backwards looking
	 * for the first set bit */
	bool foundSet = false;
	for(int dex=b.Length-1; dex>=0; dex--) {
		unsigned bitMask = 0x01;
		uint8 byte = b.Data[dex];
		for(unsigned bdex=0; bdex<8; bdex++) {
			if(byte & bitMask) {
				foundSet = true;
				break;
			}
			else {
				bitMask <<= 1;
				numBits--;
			}
		}
		if(foundSet) {
			break;
		}
	}
	/* !foundSet --> numBits = 0 */
	assert(((numBits > 0) & foundSet) || ((numBits == 0) && !foundSet));
	b.Length = (uint32)numBits;
}

/*
 * On entry, Length is bit count; on exit, a byte count.
 * The job here is to ensure that bits marked as "unused" in the 
 * BER encoding are cleared. Encoding rules say they are undefined in
 * the actual encoding.
 */
void clNssBitStringToCssm(
	CSSM_DATA &b)
{
	uint32 byteCount = (b.Length + 7) / 8;
	unsigned partialBits = b.Length & 0x7;
	b.Length = byteCount;
	if(partialBits == 0) {
		return;
	}
	
	/* mask off unused bits */
	unsigned unusedBits = 8 - partialBits;
	uint8 *bp = b.Data + b.Length - 1;
	/* mask = (2 ** unusedBits) - 1 */
	unsigned mask = (1 << unusedBits) - 1;
	*bp &= ~mask;
}

#pragma mark ----- NSS array manipulation -----
/*
 * How many items in a NULL-terminated array of pointers?
 */
unsigned clNssArraySize(
	const void **array)
{
    unsigned count = 0;
    if (array) {
		while (*array++) {
			count++;
		}
    }
    return count;
}

/* malloc a NULL-ed array of pointers of size num+1 */
void **clNssNullArray(
	uint32 num,
	SecNssCoder &coder)
{
	unsigned len = (num + 1) * sizeof(void *);
	void **p = (void **)coder.malloc(len);
	memset(p, 0, len);
	return p;
}

/*
 * GIven a CSSM_DATA containing a decoded BIT_STRING, 
 * convert to a KeyUsage.
 */
CE_KeyUsage clBitStringToKeyUsage(
	const CSSM_DATA &cdata)
{
	unsigned toCopy = (cdata.Length + 7) / 8;
	if(toCopy > 2) {
		/* I hope I never see this... */
		clErrorLog("clBitStringToKeyUsage: KeyUsage larger than 2 bytes!");
		toCopy = 2;
	}
	unsigned char bits[2] = {0, 0};
	memmove(bits, cdata.Data, toCopy);
	CE_KeyUsage usage = (((unsigned)bits[0]) << 8) | bits[1];
	return usage;
}

CSSM_ALGORITHMS CL_oidToAlg(
	const CSSM_OID &oid)
{
	CSSM_ALGORITHMS alg;
	bool found = cssmOidToAlg(&oid, &alg);
	if(!found) {
		clErrorLog("CL_oidToAlg: unknown alg\n");
		CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
	}
	return alg;
}

#pragma mark ----- copy CSSM_X509_ALGORITHM_IDENTIFIER -----

/*
 * Copy CSSM_X509_ALGORITHM_IDENTIFIER, same format (NSS and CSSM).
 */
void CL_copyAlgId(
	const CSSM_X509_ALGORITHM_IDENTIFIER &srcAlgId, 
	CSSM_X509_ALGORITHM_IDENTIFIER &dstAlgId, 
	CssmAllocator &alloc)
{
	clAllocCopyData(alloc, srcAlgId.algorithm, dstAlgId.algorithm);
	clAllocCopyData(alloc, srcAlgId.parameters, dstAlgId.parameters);
}

void CL_freeCssmAlgId(
	CSSM_X509_ALGORITHM_IDENTIFIER	*cdsaObj,		// optional
	CssmAllocator 					&alloc)
{
	if(cdsaObj == NULL) {
		return;
	}
	alloc.free(cdsaObj->algorithm.Data);
	alloc.free(cdsaObj->parameters.Data);
	memset(cdsaObj, 0, sizeof(CSSM_X509_ALGORITHM_IDENTIFIER));
}


#pragma mark ----- CSSM_X509_TIME <--> NSS format -----

/*
 * Map the tag associated with a choice of DirectoryString elements to 
 * a template array for encoding/decoding that string type.
 * Contrary to RFC2459, we allow the IA5String type, which is actually 
 * used in the real world (cf. the email address in Thawte's serverbasic
 * cert).
 */

/* The template chooser does the work here */

bool CL_nssTimeToCssm(
	const NSS_TaggedItem 	&nssTime,
	CSSM_X509_TIME			&cssmObj,
	CssmAllocator 			&alloc)	
{
	cssmObj.timeType = nssTime.tag;
	clAllocCopyData(alloc, nssTime.item, cssmObj.time);
	return true;
}

/* 
 * CSSM time to NSS time. 
 */
void CL_cssmTimeToNss(
	const CSSM_X509_TIME &cssmTime, 
	NSS_TaggedItem &nssTime, 
	SecNssCoder &coder)
{
	nssTime.tag = cssmTime.timeType;
	coder.allocCopyItem(cssmTime.time, nssTime.item);
}

void CL_freeCssmTime(
	CSSM_X509_TIME	*cssmTime,
	CssmAllocator	&alloc)
{
	if(cssmTime == NULL) {
		return;
	}
	if(cssmTime->time.Data) {
		alloc.free(cssmTime->time.Data);
	}
	memset(cssmTime, 0, sizeof(CSSM_X509_TIME));
}


#pragma mark ----- CSSM_X509_SUBJECT_PUBLIC_KEY_INFO <--> CSSM_KEY  -----

/*
 * Copy a CSSM_X509_SUBJECT_PUBLIC_KEY_INFO.
 *
 * Same format (NSS and CSSM), EXCEPT:
 *
 *   Objects which have just been NSS decoded or are about to be
 *   NSS encoded have the subjectPublicKey.Length field in BITS
 *   since this field is wrapped in a BIT STRING upon encoding. 
 * 
 *   Caller tells us which format (bits or bytes)
 *   to use for each of {src, dst}.
 */
void CL_copySubjPubKeyInfo(
	const CSSM_X509_SUBJECT_PUBLIC_KEY_INFO &srcInfo, 
	bool srcInBits,
	CSSM_X509_SUBJECT_PUBLIC_KEY_INFO &dstInfo, 
	bool dstInBits,
	CssmAllocator &alloc)
{
	CL_copyAlgId(srcInfo.algorithm, dstInfo.algorithm, alloc);
	
	CSSM_DATA srcKey = srcInfo.subjectPublicKey;
	if(srcInBits) {
		srcKey.Length = (srcKey.Length + 7) / 8;
	}
	clAllocCopyData(alloc, srcKey, dstInfo.subjectPublicKey);
	if(dstInBits) {
		dstInfo.subjectPublicKey.Length *= 8;
	}
}

/*
 * Obtain a CSSM_KEY from a CSSM_X509_SUBJECT_PUBLIC_KEY_INFO, 
 * inferring as much as we can from required fields 
 * (CSSM_X509_SUBJECT_PUBLIC_KEY_INFO) and extensions (for 
 * KeyUse, obtained from the optional DecodedCert).
 */
CSSM_KEY_PTR CL_extractCSSMKeyNSS(
	const CSSM_X509_SUBJECT_PUBLIC_KEY_INFO	&keyInfo,
	CssmAllocator			&alloc,
	const DecodedCert		*decodedCert)			// optional
{
	CSSM_KEY_PTR cssmKey = (CSSM_KEY_PTR) alloc.malloc(sizeof(CSSM_KEY));
	memset(cssmKey, 0, sizeof(CSSM_KEY));
	CSSM_KEYHEADER &hdr = cssmKey->KeyHeader;
	CssmRemoteData keyData(alloc, cssmKey->KeyData);
	try {
		hdr.HeaderVersion = CSSM_KEYHEADER_VERSION;
		/* CspId blank */
		hdr.BlobType = CSSM_KEYBLOB_RAW;
		hdr.AlgorithmId = CL_oidToAlg(keyInfo.algorithm.algorithm);
		hdr.KeyAttr = CSSM_KEYATTR_MODIFIABLE | CSSM_KEYATTR_EXTRACTABLE;
		
		/* 
		 * Format inferred from AlgorithmId. I have never seen these defined
		 * anywhere, e.g., what's the format of an RSA public key in a cert?
		 * X509 certainly doesn't say. However. the following two cases are 
		 * known to be correct. 
		 */
		switch(hdr.AlgorithmId) {
			case CSSM_ALGID_RSA:
				hdr.Format = CSSM_KEYBLOB_RAW_FORMAT_PKCS1;
				break;
			case CSSM_ALGID_DSA:
			case CSSM_ALGID_DH:
				hdr.Format = CSSM_KEYBLOB_RAW_FORMAT_X509;
				break;
			case CSSM_ALGID_FEE:
				/* CSSM_KEYBLOB_RAW_FORMAT_NONE --> DER encoded */
				hdr.Format = CSSM_KEYBLOB_RAW_FORMAT_NONE;
				break;
			default:
				/* punt */
				hdr.Format = CSSM_KEYBLOB_RAW_FORMAT_NONE;
		}
		hdr.KeyClass = CSSM_KEYCLASS_PUBLIC_KEY;
		
		/* KeyUsage inferred from extensions */
		if(decodedCert) {
			hdr.KeyUsage = decodedCert->inferKeyUsage();
		}
		else {
			hdr.KeyUsage = CSSM_KEYUSE_ANY;
		}
		
		/* start/end date unknown, leave zero */
		hdr.WrapAlgorithmId = CSSM_ALGID_NONE;
		hdr.WrapMode = CSSM_ALGMODE_NONE;
		
		switch(hdr.AlgorithmId) {
			case CSSM_ALGID_DSA:
			case CSSM_ALGID_DH:
			{
				/* 
				 * Just encode the whole subject public key info blob.
				 * NOTE we're assuming that the keyInfo.subjectPublicKey
				 * field is in the NSS_native BITSTRING format, i.e., 
				 * its Length field is in bits and we don't have to adjust.
				 */
				PRErrorCode prtn = SecNssEncodeItemOdata(&keyInfo, 
					NSS_SubjectPublicKeyInfoTemplate, keyData);
				if(prtn) {
					clErrorLog("extractCSSMKey: error on reencode\n");
					CssmError::throwMe(CSSMERR_CL_MEMORY_ERROR);
				}
				break;
			}
			default:
				/*
				 * RSA, FEE for now.
				 * keyInfo.subjectPublicKey (in BITS) ==> KeyData
				 */
				keyData.copy(keyInfo.subjectPublicKey.Data,
					(keyInfo.subjectPublicKey.Length + 7) / 8);
		}
		keyData.release();

		/*
		 * LogicalKeySizeInBits - ask the CSP
		 */
		CSSM_CSP_HANDLE cspHand = getGlobalCspHand(true);
		CSSM_KEY_SIZE keySize;
		CSSM_RETURN crtn;
		crtn = CSSM_QueryKeySizeInBits(cspHand, CSSM_INVALID_HANDLE, cssmKey,
			&keySize);
		switch(crtn) {
			default:
				CssmError::throwMe(crtn);
			case CSSMERR_CSP_APPLE_PUBLIC_KEY_INCOMPLETE:
				/*
			 	 * This is how the CSP indicates a "partial" public key,
				 * with a valid public key value but no alg-specific
				 * parameters (currently, DSA only). 
				 */
				hdr.KeyAttr |= CSSM_KEYATTR_PARTIAL;
				/* and drop thru */
			case CSSM_OK:
				cssmKey->KeyHeader.LogicalKeySizeInBits = 
					keySize.LogicalKeySizeInBits;
				break;
		}
	}
	catch (...) {
		alloc.free(cssmKey);
		throw;
	}
	return cssmKey;
}

/* 
 * Set up a encoded NULL for CSSM_X509_ALGORITHM_IDENTIFIER.parameters.
 */
void CL_nullAlgParams(
	CSSM_X509_ALGORITHM_IDENTIFIER	&algId)
{
	static const uint8 encNull[2] = { SEC_ASN1_NULL, 0 };
	CSSM_DATA encNullData;
	encNullData.Data = (uint8 *)encNull;
	encNullData.Length = 2;

	algId.parameters = encNullData;
}

/* 
 * Convert a CSSM_KEY to a CSSM_X509_SUBJECT_PUBLIC_KEY_INFO. The
 * CSSM key must be in raw format and with a specific blob format.
 *  	-- RSA keys have to be CSSM_KEYBLOB_RAW_FORMAT_PKCS1
 * 		-- DSA keys have to be CSSM_KEYBLOB_RAW_FORMAT_X509
 */
void CL_CSSMKeyToSubjPubKeyInfoNSS(
	const CSSM_KEY 						&cssmKey,
	CSSM_X509_SUBJECT_PUBLIC_KEY_INFO	&nssKeyInfo,
	SecNssCoder							&coder)
{
	const CSSM_KEYHEADER &hdr = cssmKey.KeyHeader;
	if(hdr.BlobType != CSSM_KEYBLOB_RAW) {
		clErrorLog("CL SetField: must specify RAW key blob\n");
		CssmError::throwMe(CSSMERR_CSP_KEY_BLOB_TYPE_INCORRECT);
	}
	memset(&nssKeyInfo, 0, sizeof(nssKeyInfo));
	
	/* algorithm and format dependent from here... */
	switch(hdr.AlgorithmId) {
		case CSSM_ALGID_RSA:
			if(hdr.Format != CSSM_KEYBLOB_RAW_FORMAT_PKCS1) {
				clErrorLog("CL SetField: RSA key must be in PKCS1 format\n");
				CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_FORMAT);
			}
			/* and fall thru */
		default:
		{
			/* Key header's algorithm --> OID */
			const CSSM_OID *oid = cssmAlgToOid(hdr.AlgorithmId);
			if(oid == NULL) {
				clErrorLog("CL SetField: Unknown key algorithm\n");
				CssmError::throwMe(CSSMERR_CSP_INVALID_ALGORITHM);
			}
			CSSM_X509_ALGORITHM_IDENTIFIER &algId = nssKeyInfo.algorithm;
			coder.allocCopyItem(*oid, algId.algorithm);

			/* NULL algorithm parameters, always in this case */
			CL_nullAlgParams(algId);
			
			/* Copy key bits, destination is a BIT STRING */
			coder.allocCopyItem(cssmKey.KeyData, nssKeyInfo.subjectPublicKey);
			nssKeyInfo.subjectPublicKey.Length *= 8;
			break;
		}	
		case CSSM_ALGID_DSA:
			if(hdr.Format != CSSM_KEYBLOB_RAW_FORMAT_X509) {
				clErrorLog("CL SetField: DSA key must be in X509 format\n");
				CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_FORMAT);
			}
			
			/* 
			 * All we do is decode the whole key blob into the 
			 * SubjectPublicKeyInfo.
			 */
			if(coder.decodeItem(cssmKey.KeyData, 
					NSS_SubjectPublicKeyInfoTemplate, 
					&nssKeyInfo)) {
				clErrorLog("CL SetField: Error decoding DSA public key\n");
				CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_FORMAT);
			}
			break;
	}
}

void CL_freeCSSMKey(
	CSSM_KEY_PTR		cssmKey,
	CssmAllocator		&alloc,
	bool				freeTop)
{
	if(cssmKey == NULL) {
		return;
	}
	alloc.free(cssmKey->KeyData.Data);
	memset(cssmKey, 0, sizeof(CSSM_KEY));
	if(freeTop) {
		alloc.free(cssmKey);
	}
}

#pragma mark ----- CE_AuthorityKeyID <--> NSS_AuthorityKeyId -----

void CL_cssmAuthorityKeyIdToNss(
	const CE_AuthorityKeyID 	&cdsaObj,
	NSS_AuthorityKeyId 			&nssObj,
	SecNssCoder 				&coder) 
{
	memset(&nssObj, 0, sizeof(nssObj));
	if(cdsaObj.keyIdentifierPresent) {
		nssObj.keyIdentifier = (CSSM_DATA_PTR)coder.malloc(sizeof(CSSM_DATA));
		coder.allocCopyItem(cdsaObj.keyIdentifier, *nssObj.keyIdentifier);
	}
	if(cdsaObj.generalNamesPresent ) {
		/* GeneralNames, the hard one */
		CL_cssmGeneralNamesToNss(*cdsaObj.generalNames,
			nssObj.genNames, coder);
	}
	if(cdsaObj.serialNumberPresent) {
		coder.allocCopyItem(cdsaObj.serialNumber,nssObj.serialNumber);
	}
}

void CL_nssAuthorityKeyIdToCssm(
	const NSS_AuthorityKeyId 		&nssObj,
	CE_AuthorityKeyID 				&cdsaObj,
	SecNssCoder 					&coder,	// for temp decoding
	CssmAllocator					&alloc)
{
	if(nssObj.keyIdentifier != NULL) {
		cdsaObj.keyIdentifierPresent = CSSM_TRUE;
		clAllocCopyData(alloc, *nssObj.keyIdentifier, cdsaObj.keyIdentifier);
	}
	if(nssObj.genNames.names != NULL) {
		/* GeneralNames, the hard one */
		cdsaObj.generalNamesPresent = CSSM_TRUE;
		cdsaObj.generalNames = 
			(CE_GeneralNames *)alloc.malloc(sizeof(CE_GeneralNames));
		CL_nssGeneralNamesToCssm(nssObj.genNames, 
			*cdsaObj.generalNames,
			coder,
			alloc);
	}
	if(nssObj.serialNumber.Data != NULL) {
		cdsaObj.serialNumberPresent = CSSM_TRUE;
		clAllocCopyData(alloc, nssObj.serialNumber, cdsaObj.serialNumber);
	}
}

#pragma mark ----- decode/encode CE_DistributionPointName -----

/* This is always a DER-encoded blob at the NSS level */
void CL_decodeDistributionPointName(
	const CSSM_DATA				&nssBlob,
	CE_DistributionPointName	&cssmDpn,
	SecNssCoder					&coder,
	CssmAllocator				&alloc)
{
	memset(&cssmDpn, 0, sizeof(CE_DistributionPointName));
	if(nssBlob.Length == 0) {
		clErrorLog("***CL_decodeDistributionPointName: bad PointName\n");
		CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
	}
	unsigned char tag = nssBlob.Data[0] & SEC_ASN1_TAGNUM_MASK;
	switch(tag) {
		case NSS_DIST_POINT_FULL_NAME_TAG:
		{
			/* decode to temp coder memory */
			NSS_GeneralNames gnames;
			gnames.names = NULL;
			if(coder.decodeItem(nssBlob, NSS_DistPointFullNameTemplate,
					&gnames)) {
				clErrorLog("***Error decoding DistPointFullName\n");
				CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
			}
			
			cssmDpn.nameType = CE_CDNT_FullName;
			cssmDpn.fullName = (CE_GeneralNames *)alloc.malloc(
				sizeof(CE_GeneralNames));
				
			/* copy out to caller */
			CL_nssGeneralNamesToCssm(gnames, 
				*cssmDpn.fullName, coder, alloc);
			break;
		}
		case NSS_DIST_POINT_RDN_TAG:
		{
			/* decode to temp coder memory */
			NSS_RDN rdn;
			memset(&rdn, 0, sizeof(rdn));
			if(coder.decodeItem(nssBlob, NSS_DistPointRDNTemplate,
					&rdn)) {
				clErrorLog("***Error decoding DistPointRDN\n");
				CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
			}
			
			cssmDpn.nameType = CE_CDNT_NameRelativeToCrlIssuer;
			cssmDpn.rdn = (CSSM_X509_RDN_PTR)alloc.malloc(
				sizeof(CSSM_X509_RDN));
			
			/* copy out to caller */
			CL_nssRdnToCssm(rdn, *cssmDpn.rdn, alloc, coder);
			break;
		}
		default:
			clErrorLog("***Bad CE_DistributionPointName tag\n");
			CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
	}
}

void CL_encodeDistributionPointName(
	CE_DistributionPointName &cpoint,
	CSSM_DATA &npoint,
	SecNssCoder &coder)
{
	const SEC_ASN1Template *templ = NULL;
	NSS_GeneralNames gnames;
	NSS_RDN rdn;
	void *encodeSrc = NULL;
	
	/* 
	 * Our job is to convert one of two incoming aggregate types
	 * into NSS format, then encode the result into npoint.
	 */
	switch(cpoint.nameType) {
		case CE_CDNT_FullName:
			CL_cssmGeneralNamesToNss(*cpoint.fullName,
				gnames, coder);
			encodeSrc = &gnames;
			templ = NSS_DistPointFullNameTemplate;
			break;
			
		case CE_CDNT_NameRelativeToCrlIssuer:
			CL_cssmRdnToNss(*cpoint.rdn, rdn, coder);
			encodeSrc = &rdn;
			templ = NSS_DistPointRDNTemplate;
			break;
		default:
			clErrorLog("CL_encodeDistributionPointName: bad nameType\n");
			CssmError::throwMe(CSSMERR_CL_UNKNOWN_TAG);
	}
	if(coder.encodeItem(encodeSrc, templ, npoint)) {
		clErrorLog("CL_encodeDistributionPointName: encode error\n");
		CssmError::throwMe(CSSMERR_CL_MEMORY_ERROR);
	}
}


#pragma mark --- CE_CRLDistPointsSyntax <--> NSS_CRLDistributionPoints ---

void CL_cssmDistPointsToNss(
	const CE_CRLDistPointsSyntax 	&cdsaObj,
	NSS_CRLDistributionPoints		&nssObj,
	SecNssCoder 					&coder)
{
	memset(&nssObj, 0, sizeof(nssObj));
	unsigned numPoints = cdsaObj.numDistPoints;
	if(numPoints == 0) {
		return;
	}
	nssObj.distPoints = 
		(NSS_DistributionPoint **)clNssNullArray(numPoints, coder);
	for(unsigned dex=0; dex<numPoints; dex++) {
		nssObj.distPoints[dex] = (NSS_DistributionPoint *)
			coder.malloc(sizeof(NSS_DistributionPoint));
		NSS_DistributionPoint *npoint = nssObj.distPoints[dex];
		memset(npoint, 0, sizeof(NSS_DistributionPoint));
		CE_CRLDistributionPoint *cpoint = &cdsaObj.distPoints[dex];
		
		/* all fields are optional */
		if(cpoint->distPointName) {
			/* encode and drop into ASN_ANY slot */
			npoint->distPointName = (CSSM_DATA *)
				coder.malloc(sizeof(CSSM_DATA));
			CL_encodeDistributionPointName(*cpoint->distPointName,
				*npoint->distPointName, coder);
			
		}
		
		if(cpoint->reasonsPresent) {
			/* bit string, presumed max length 8 bits */
			coder.allocItem(npoint->reasons, 1);
			npoint->reasons.Data[0] = cpoint->reasons;
			/* adjust for bit string length */
			npoint->reasons.Length = 8;
		}
		
		if(cpoint->crlIssuer) {
			CL_cssmGeneralNamesToNss(*cpoint->crlIssuer,
				npoint->crlIssuer, coder);
		}
	}
}

void CL_nssDistPointsToCssm(
	const NSS_CRLDistributionPoints	&nssObj,
	CE_CRLDistPointsSyntax			&cdsaObj,
	SecNssCoder 					&coder,	// for temp decoding
	CssmAllocator					&alloc)
{
	memset(&cdsaObj, 0, sizeof(cdsaObj));
	unsigned numPoints = clNssArraySize((const void **)nssObj.distPoints);
	if(numPoints == 0) {
		return;
	}
	
	unsigned len = sizeof(CE_CRLDistributionPoint) * numPoints;
	cdsaObj.distPoints = (CE_CRLDistributionPoint *)alloc.malloc(len);
	memset(cdsaObj.distPoints, 0, len);
	cdsaObj.numDistPoints = numPoints;

	for(unsigned dex=0; dex<numPoints; dex++) {
		CE_CRLDistributionPoint &cpoint = cdsaObj.distPoints[dex];
		NSS_DistributionPoint &npoint = *(nssObj.distPoints[dex]);
	
		/* All three fields are optional */
		if(npoint.distPointName != NULL) {
			/* Drop in a CE_DistributionPointName */
			CE_DistributionPointName *cname = 
				(CE_DistributionPointName *)alloc.malloc(
					sizeof(CE_DistributionPointName));
			memset(cname, 0, sizeof(*cname));
			cpoint.distPointName = cname;
			
			/*
			 * This one is currently still encoded; we have to peek
			 * at its tag and decode accordingly.
			 */
			CL_decodeDistributionPointName(*npoint.distPointName,
				*cname, coder, alloc);
		}

		if(npoint.reasons.Data != NULL) {
			/* careful, it's a bit string */
			if(npoint.reasons.Length > 8) {
				clErrorLog("***CL_nssDistPointsToCssm: Malformed reasons\n");
				CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
			}
			cpoint.reasonsPresent = CSSM_TRUE;
			if(npoint.reasons.Length != 0) {
				cpoint.reasons = npoint.reasons.Data[0];
			}
		}
		
		if(npoint.crlIssuer.names != NULL) {
			/* Cook up a new CE_GeneralNames */
			cpoint.crlIssuer = 
				(CE_GeneralNames *)alloc.malloc(sizeof(CE_GeneralNames));
			CL_nssGeneralNamesToCssm(npoint.crlIssuer, *cpoint.crlIssuer,
				coder, alloc);
		}
	}
}

#pragma mark ----- IssuingDistributionPoint -----

void CL_nssIssuingDistPointToCssm(
	NSS_IssuingDistributionPoint *nssIdp,
	CE_IssuingDistributionPoint	*cssmIdp,
	SecNssCoder					&coder,
	CssmAllocator				&alloc)
{
	/* All fields optional */
	memset(cssmIdp, 0, sizeof(*cssmIdp));
	if(nssIdp->distPointName) {
		CE_DistributionPointName *cssmDp = (CE_DistributionPointName *)
			alloc.malloc(sizeof(CE_DistributionPointName));
			
		/*
		 * This one is currently still encoded; we have to peek
		 * at its tag and decode accordingly.
		 */
		CL_decodeDistributionPointName(*nssIdp->distPointName,
			*cssmDp, coder, alloc);
		cssmIdp->distPointName = cssmDp;
	}
	if(nssIdp->onlyUserCerts) {
		cssmIdp->onlyUserCertsPresent = CSSM_TRUE;
		cssmIdp->onlyUserCerts = clNssBoolToCssm(*nssIdp->onlyUserCerts);
	}
	if(nssIdp->onlyCACerts) {
		cssmIdp->onlyCACertsPresent = CSSM_TRUE;
		cssmIdp->onlyCACerts = clNssBoolToCssm(*nssIdp->onlyCACerts);
	}
	if(nssIdp->onlySomeReasons) {
		cssmIdp->onlySomeReasonsPresent = CSSM_TRUE;
		if(nssIdp->onlySomeReasons->Length > 0) {
			cssmIdp->onlySomeReasons = *nssIdp->onlySomeReasons->Data;
		}
		else {
			cssmIdp->onlySomeReasons = 0;
		}
	}
	if(nssIdp->indirectCRL) {
		cssmIdp->indirectCrlPresent = CSSM_TRUE;
		cssmIdp->indirectCrl = clNssBoolToCssm(*nssIdp->indirectCRL);
	}
}

#pragma mark ----- Top-level Cert/CRL encode and decode -----

/*
 * To ensure a secure means of signing and verifying TBSCert blobs, we
 * provide these functions to encode and decode just the top-level
 * elements of a certificate. Unfortunately there is no guarantee 
 * that when you decode and re-encode a TBSCert blob, you get the 
 * same thing you started with (although with DER rules, as opposed 
 * to BER rules, you should). Thus when signing, we sign the TBSCert
 * and encode the signed cert here without ever decoding the TBSCert (or,
 * at least, without using the decoded version to get the encoded TBS blob).
 */

void CL_certCrlDecodeComponents(
	const CssmData 	&signedItem,		// DER-encoded cert or CRL
	CssmOwnedData	&tbsBlob,			// still DER-encoded
	CssmOwnedData	&algId,				// ditto
	CssmOwnedData	&rawSig)			// raw bits (not an encoded AsnBits)
{
	/* BER-decode into temp memory */
	NSS_SignedCertOrCRL nssObj;
	SecNssCoder coder;
	PRErrorCode prtn;
	
	memset(&nssObj, 0, sizeof(nssObj));
	prtn = coder.decode(signedItem.data(), signedItem.length(),
		NSS_SignedCertOrCRLTemplate, &nssObj);
	if(prtn) {
		CssmError::throwMe(CSSMERR_CL_UNKNOWN_FORMAT);
	}
	
	/* tbsBlob and algId are raw ASN_ANY including tags, which we pass 
	 * back to caller intact */
	tbsBlob.copy(nssObj.tbsBlob.Data, nssObj.tbsBlob.Length);
	algId.copy(nssObj.signatureAlgorithm.Data, 
		nssObj.signatureAlgorithm.Length);
		
	/* signature is a bit string which we do in fact decode */
	rawSig.copy(nssObj.signature.Data,
		(nssObj.signature.Length + 7) / 8);
}


/*
 * Given pre-DER-encoded blobs, do the final encode step for a signed cert.
 */
void 
CL_certEncodeComponents(
	const CssmData		&TBSCert,		// DER-encoded
	const CssmData		&algId,			// ditto
	const CssmData		&rawSig,		// raw bits, not encoded
	CssmOwnedData 		&signedCert)	// DER-encoded
{
	NSS_SignedCertOrCRL nssObj;
	nssObj.tbsBlob.Data = TBSCert.Data;
	nssObj.tbsBlob.Length = TBSCert.Length;
	nssObj.signatureAlgorithm.Data = algId.Data;
	nssObj.signatureAlgorithm.Length = algId.Length;
	nssObj.signature.Data = rawSig.Data;
	nssObj.signature.Length = rawSig.Length * 8;	// BIT STRING
	
	PRErrorCode prtn;
	
	prtn = SecNssEncodeItemOdata(&nssObj,
		NSS_SignedCertOrCRLTemplate,signedCert);
	if(prtn) {
		CssmError::throwMe(CSSMERR_CL_MEMORY_ERROR);
	}

}
