/*
 * Copyright (c) 1999-2013 Apple, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */


#include <string.h>
#include <kern/cpu_number.h>
#include <kern/cpu_data.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <sys/random.h>

#include <corecrypto/ccdrbg.h>

#include <prng/YarrowCoreLib/include/yarrow.h>

#include <libkern/OSByteOrder.h>
#include <libkern/OSAtomic.h>

#include <mach/mach_time.h>

#include <prng/random.h>

#include "fips_sha1.h"


/*
	WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING!
	
	THIS FILE IS NEEDED TO PASS FIPS ACCEPTANCE FOR THE RANDOM NUMBER GENERATOR.
	IF YOU ALTER IT IN ANY WAY, WE WILL NEED TO GO THOUGH FIPS ACCEPTANCE AGAIN,
	AN OPERATION THAT IS VERY EXPENSIVE AND TIME CONSUMING.  IN OTHER WORDS,
	DON'T MESS WITH THIS FILE.

	WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING!
*/
/*
	WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING!

	ANY CODE PROTECTED UNDER "#ifdef __arm__" IS SERIOUSLY SUPPOSED TO BE THERE!
	IF YOU REMOVE ARM CODE, RANDOM WILL NOT MEAN ANYTHING FOR iPHONES ALL OVER.
	PLEASE DON'T TOUCH __arm__ CODE IN THIS FILE!

	WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING! WARNING!
*/


#define RESEED_TICKS 50 /* how long a reseed operation can take */


typedef u_int8_t BlockWord;
enum {kBSize = 20};
typedef BlockWord Block[kBSize];
enum {kBlockSize = sizeof(Block)};

struct YarrowContext {
	PrngRef		PrngRef;
	Block		xkey;
	Block		random_data;
	int		bytes_used;
	unsigned char	SelfTestInitialized;
	u_int32_t	LastBlockChecksum;
	uint64_t	bytes_since_reseed;
};
typedef struct YarrowContext *YarrowContextp;

/* define prototypes to keep the compiler happy... */

void add_blocks(Block a, Block b, BlockWord carry);
void fips_initialize(YarrowContextp yp);
void random_block(YarrowContextp yp, Block b, int addOptional);
u_int32_t CalculateCRC(u_int8_t* buffer, size_t length);

/*
 * Get 120 bits from yarrow
 */

/*
 * add block b to block a
 */
void
add_blocks(Block a, Block b, BlockWord carry)
{
	int i = kBlockSize - 1;
	while (i >= 0)
	{
		u_int32_t c = (u_int32_t)carry +
					  (u_int32_t)a[i] +
					  (u_int32_t)b[i];
		a[i] = c & 0xff;
		carry = c >> 8;
		i -= 1;
	}
}



static char zeros[(512 - kBSize * 8) / 8];

static const u_int32_t g_crc_table[] =
{
	0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
	0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
	0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
	0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
	0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
	0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
	0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
	0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
	0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
	0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
	0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
	0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
	0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
	0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
	0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
	0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
	0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
	0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
	0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
	0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
	0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
	0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
	0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
	0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
	0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
	0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
	0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
	0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
	0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
	0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
	0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
	0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

/*
 * Setup for fips compliance
 */

/*
 * calculate a crc-32 checksum
 */
u_int32_t CalculateCRC(u_int8_t* buffer, size_t length)
{
	u_int32_t crc = 0;
	
	size_t i;
	for (i = 0; i < length; ++i)
	{
		u_int32_t temp = (crc ^ ((u_int32_t) buffer[i])) & 0xFF;
		crc = (crc >> 8) ^ g_crc_table[temp];
	}
	
	return crc;
}

/*
 * get a random block of data per fips 186-2
 */
void
random_block(YarrowContextp pp, Block b, int addOptional)
{
	SHA1_CTX sha1_ctx;

	int repeatCount = 0;
	do
	{
		// do one iteration
		
		if (addOptional)
		{
			// create an xSeed to add.
			Block xSeed;
			prngOutput (pp->PrngRef, (BYTE*) &xSeed, sizeof (xSeed));
			
			// add the seed to the previous value of xkey
			add_blocks (pp->xkey, xSeed, 0);
		}
		
		// initialize the value of H
		FIPS_SHA1Init(&sha1_ctx);
		
		// to stay compatible with the FIPS specification, we need to flip the bytes in
		// xkey to little endian byte order.  In our case, this makes exactly no difference
		// (random is random), but we need to do it anyway to keep FIPS happy
		
		// compute "G"
		FIPS_SHA1Update(&sha1_ctx, pp->xkey, kBlockSize);
		
		// add zeros to fill the internal SHA-1 buffer
		FIPS_SHA1Update (&sha1_ctx, (const u_int8_t *)zeros, sizeof (zeros));
		
		// we have to do a byte order correction here because the sha1 math is being done internally
		// as u_int32_t, not a stream of bytes.  Since we maintain our data as a byte stream, we need
		// to convert
		
		u_int32_t* finger = (u_int32_t*) b;
		
		unsigned j;
		for (j = 0; j < kBlockSize / sizeof (u_int32_t); ++j)
		{
			*finger++ = OSSwapHostToBigInt32(sha1_ctx.h.b32[j]);
		}		
		
		// calculate the CRC-32 of the block
		u_int32_t new_crc = CalculateCRC(sha1_ctx.h.b8, sizeof (Block));
		
		// make sure we don't repeat
		int cmp = new_crc == pp->LastBlockChecksum;
		pp->LastBlockChecksum = new_crc;
		if (!pp->SelfTestInitialized)
		{
			pp->SelfTestInitialized = 1;
			return;
		}
		else if (!cmp)
		{
			return;
		}
		
		repeatCount += 1;
		
		// fix up the next value of xkey
		add_blocks (pp->xkey, b, 1);
	} while (repeatCount < 2);
	
	/*
	 * If we got here, three sucessive checksums of the random number
	 * generator have been the same.  Since the odds of this happening are
	 * 1 in 18,446,744,073,709,551,616, (1 in 18 quintillion) one of the following has
	 * most likely happened:
	 *
	 * 1: There is a significant bug in this code.
	 * 2: There has been a massive system failure.
	 * 3: The universe has ceased to exist.
	 *
	 * There is no good way to recover from any of these cases. We
	 * therefore panic.
	 */
	 
	 panic("FIPS random self-test failed.");
}

const Block kKnownAnswer = {0x92, 0xb4, 0x04, 0xe5, 0x56, 0x58, 0x8c, 0xed, 0x6c, 0x1a, 0xcd, 0x4e, 0xbf, 0x05, 0x3f, 0x68, 0x09, 0xf7, 0x3a, 0x93};

void
fips_initialize(YarrowContextp yp)
{
	/* So that we can do the self test, set the seed to zero */
	memset(&yp->xkey, 0, sizeof(yp->xkey));
	
	/* other initializations */
	memset (zeros, 0, sizeof (zeros));
	yp->bytes_used = 0;
	random_block(yp, yp->random_data, FALSE);
	
	// check here to see if we got the initial data we were expecting
	if (memcmp(kKnownAnswer, yp->random_data, kBlockSize) != 0)
	{
		panic("FIPS random self test failed");
	}
	
	// now do the random block again to make sure that userland doesn't get predicatable data
	random_block(yp, yp->random_data, TRUE);
}


static int
yarrow_init(
	const struct ccdrbg_info *info,
	struct ccdrbg_state *drbg,
	unsigned long entropyLength, const void* entropy,
	unsigned long nonceLength, const void* nonce,
	unsigned long psLength, const void* ps)
{
#pragma unused(info)
#pragma unused(nonceLength)
#pragma unused(nonce)
#pragma unused(psLength)
#pragma unused(ps)
	YarrowContextp		yp = (YarrowContextp) drbg;
	prng_error_status	perr;
	char			buffer[16];

	yp->SelfTestInitialized = 0;

	/* create a Yarrow object */
	perr = prngInitialize(&yp->PrngRef);
	if (perr != 0) {
		panic("Couldn't initialize Yarrow, /dev/random will not work.");
	}

	perr = prngInput(yp->PrngRef, (BYTE*) entropy, (UINT) entropyLength,
			SYSTEM_SOURCE, (UINT) entropyLength * 8);
	if (perr != 0) {
		/* an error, complain */
		panic("Couldn't seed Yarrow.\n");
	}

	/* turn the data around */
	perr = prngOutput(yp->PrngRef, (BYTE*) buffer, (UINT) sizeof(buffer));

	/* and scramble it some more */
	perr = prngForceReseed(yp->PrngRef, RESEED_TICKS);

	fips_initialize(yp);

	yp->bytes_since_reseed = 0;

	return perr;
}

static int
yarrow_generate(
	struct ccdrbg_state *prng,
	unsigned long outlen, void *out,
	unsigned long inlen, const void *in)
{
#pragma unused(inlen)
#pragma unused(in)
	YarrowContextp	yp = (YarrowContextp) prng;
	int		bytes_read = 0;
	int		bytes_remaining = (int) outlen;

	yp->bytes_since_reseed += outlen;
	if (yp->bytes_since_reseed > RESEED_BYTES)
		return CCDRBG_STATUS_NEED_RESEED;
	
	while (bytes_remaining > 0) {
		int bytes_to_read = MIN(bytes_remaining,
					kBlockSize - yp->bytes_used);
		if (bytes_to_read == 0) {
			random_block(yp, yp->random_data, TRUE);
			yp->bytes_used = 0;
			bytes_to_read = MIN(bytes_remaining, kBlockSize);
		}
		
		memmove((u_int8_t*) out + bytes_read,
			((u_int8_t*)yp->random_data) + yp->bytes_used,
			bytes_to_read);
		yp->bytes_used += bytes_to_read;
		bytes_read += bytes_to_read;
		bytes_remaining -= bytes_to_read;
	}

	return CCDRBG_STATUS_OK;
}

static int
yarrow_reseed(
	struct ccdrbg_state *prng,
	unsigned long entropylen, const void *entropy,
	unsigned long inlen, const void *in)
{
#pragma unused(inlen)
#pragma unused(in)
	YarrowContextp	yp = (YarrowContextp) prng;

	(void) prngInput(yp->PrngRef, (BYTE*) entropy, (UINT) entropylen,
			 SYSTEM_SOURCE, (UINT) entropylen * 8);
	(void) prngForceReseed(yp->PrngRef, RESEED_TICKS);

	yp->bytes_since_reseed = 0;

	return CCDRBG_STATUS_OK;
}

static void
yarrow_destroy(
	struct ccdrbg_state *prng)
{
#pragma unused(prng)
}


void
ccdrbg_factory_yarrow(
	struct ccdrbg_info 	*info,
	const void 		*custom)
{
	info->size = sizeof(struct YarrowContext);
	info->init = yarrow_init;
	info->generate = yarrow_generate;
	info->reseed = yarrow_reseed;
	info->done = yarrow_destroy;
	info->custom = custom;
}