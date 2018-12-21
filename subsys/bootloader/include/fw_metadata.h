/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef FW_METADATA_H__
#define FW_METADATA_H__

/*
 * The package will consist of (firmware | (padding) | validation_info),
 * where the firmware contains the firmware_info at a predefined location. The
 * padding is present if the validation_info needs alignment. The
 * validation_info is not directly referenced from the firmware_info since the
 * validation_info doesn't actually have to be placed after the firmware.
 *
 * Putting the firmware info inside the firmware instead of in front of it
 * removes the need to consider the padding before the vector table of the
 * firmware. It will also likely make it easier to add all the info at compile
 * time.
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <toolchain.h>
#include <assert.h>

#define MAGIC_LEN_WORDS (CONFIG_SB_MAGIC_LEN / sizeof(u32_t))

#define TYPE_AND_DECL(retval, name, ...) \
	typedef retval (* name ## _t) (__VA_ARGS__); \
	retval name (__VA_ARGS__)


/**@brief Function that returns an ABI.
 *
 * @param[in]    id      Which ABI to get.
 * @param[in]    index   If there are multiple ABIs available with the same ID,
 *                       retrieve the different ones with this.
 * @param[out]   buf     Where the ABI will be copied.
 * @param[inout] len     in: Length of buffer, out: length of ABI.
 *
 * @retval 0       Success
 * @retval -ENOMEM buffer is too small, len will contain the required size. The
 *                 buffer will be filled as far as will fit.
 */
typedef int (*fw_abi_getter)(u32_t id, u32_t index, u8_t * buf, u32_t * len);

// typedef int (*fw_abi_getter2)(u32_t id, u32_t index, u8_t ** buf, u32_t * len);

// typedef int (*fw_abi_getter2)(u32_t id, u32_t index, u8_t ** buf, u32_t * len);


struct __packed fw_abi_getter_info {
	u32_t magic[MAGIC_LEN_WORDS];

	/* Function to be used to retrieve ABIs. */
	fw_abi_getter abi_getter;

	/* Pointer directly to a list of lists of ABIs. */
	struct fw_abi_info const * const * abis;

	/* Length of outer list pointed to by abis. */
	u32_t abis_len;

	/* For future use. */
	u32_t reserved1;
	u32_t reserved2;
};


/* This struct is meant to serve as a header before a list of function pointers
 * (or something else) that constitute the actual ABI. How to use the ABI, such
 * as the signatures of all the functions in the list must be unambiguous for an
 * ID/version combination. */
struct __packed fw_abi_info {
	u32_t magic[MAGIC_LEN_WORDS];

	/* Flags specifying properties of the ABI. */
	u32_t abi_flags;

	/* The id of the ABI. */
	u32_t abi_id;

	/* The version of this ABI. */
	u32_t abi_version;

	/* The length of everything after this header. */
	u32_t abi_len;
};


struct __packed fw_firmware_info {
	u32_t magic[MAGIC_LEN_WORDS];

	/* Size without validation_info pointer and padding. */
	u32_t firmware_size;

	/* Monotonically increasing version counter.*/
	u32_t firmware_version;

	/* The address of the start (vector table) of the firmware. */
	u32_t firmware_address;

	/* Where to place the getter for the ABI provided to this firmware. */
	struct fw_abi_getter_info * abi_in;

	/* This firmware's ABI getter. */
	const struct fw_abi_getter_info * abi_out;
};


struct __packed fw_validation_info {
	/* Magic value */
	u32_t magic[MAGIC_LEN_WORDS];

	/* The address of the start (vector table) of the firmware. */
	u32_t firmware_address;

	/* The hash of the firmware.*/
	u8_t  firmware_hash[CONFIG_SB_HASH_LEN];

	/* Public key to be used for signature verification.
	 * This must be checked against a trusted hash.
	 */
	u8_t  public_key[CONFIG_SB_PUBLIC_KEY_LEN];

	/* Signature over the firmware as represented by the
	 * firmware_address and firmware_size in the firmware_info.
	 */
	u8_t  signature[CONFIG_SB_SIGNATURE_LEN];
};

/*
 * Can be used to make the firmware discoverable in other locations, e.g. when
 * searching backwards. This struct would typically be constructed locally, so
 * it needs no version.
 */
struct fw_validation_pointer {
	u32_t magic[MAGIC_LEN_WORDS];
	const struct fw_validation_info *validation_info;
};


static inline bool memeq_32(const void *expected, const void *actual,
		u32_t len)
{
	u32_t *expected_32 = (u32_t *) expected;
	u32_t *actual_32   = (u32_t *) actual;

	__ASSERT(!(len & 3), "Length is not multiple of 4");

	for (u32_t i = 0; i < (len / sizeof(u32_t)); i++) {
		if (expected_32[i] != actual_32[i]) {
			return false;
		}
	}
	return true;
}

/*
 * Get a pointer to the firmware_info structure inside the firmware.
 */
static inline const struct fw_firmware_info *
firmware_info_get(u32_t firmware_address) {
	u32_t finfo_addr = firmware_address + CONFIG_SB_FIRMWARE_INFO_OFFSET;
	const struct fw_firmware_info *finfo;
	const u32_t firmware_info_magic[] = {FIRMWARE_INFO_MAGIC};

	finfo = (const struct fw_firmware_info *)(finfo_addr);
	if (memeq_32(finfo->magic, firmware_info_magic, CONFIG_SB_MAGIC_LEN)) {
		return finfo;
	}
	return NULL;
}

/*
 * Find the validation_info at the end of the firmware.
 */
static inline const struct fw_validation_info *
validation_info_find(const struct fw_firmware_info *finfo, u32_t
		search_distance) {
	u32_t vinfo_addr = finfo->firmware_address + finfo->firmware_size;
	const struct fw_validation_info *vinfo;
	const u32_t validation_info_magic[] = {VALIDATION_INFO_MAGIC};

	for (int i = 0; i <= search_distance; i++) {
		vinfo = (const struct fw_validation_info *)(vinfo_addr + i);
		if (memeq_32(vinfo->magic, validation_info_magic,
					CONFIG_SB_MAGIC_LEN)) {
			return vinfo;
		}
	}
	return NULL;
}

#endif
