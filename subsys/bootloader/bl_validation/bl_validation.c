/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <bl_validation.h>
#include <zephyr/types.h>
#include <fw_info.h>
#include <kernel.h>
#include <bl_storage.h>


int set_monotonic_version(u16_t version, u16_t slot)
{
	__ASSERT(version <= 0x7FFF, "version too large.\r\n");
	__ASSERT(slot <= 1, "Slot must be either 0 or 1.\r\n");
	printk("Setting monotonic counter (version: %d, slot: %d)\r\n",
		version, slot);
	int err = set_monotonic_counter((version << 1) | !slot);

	if (num_monotonic_counter_slots() == 0) {
		printk("Monotonic version counter is disabled.\r\n");
	} else if (err != 0) {
		printk("set_monotonic_counter() error: %d\n\r", err);
	}
	return err;
}

u16_t get_monotonic_version(u16_t *slot_out)
{
	u16_t monotonic_version = get_monotonic_counter();

	if (slot_out != NULL) {
		*slot_out = !(monotonic_version & 1);
	}
	return monotonic_version >> 1;
}


#ifndef CONFIG_BL_VALIDATE_FW_EXT_API_UNUSED
#ifdef CONFIG_BL_VALIDATE_FW_EXT_API_REQUIRED
	#define BL_VALIDATE_FW_EXT_API_REQUIRED 1
#else
	#define BL_VALIDATE_FW_EXT_API_REQUIRED 0
#endif

EXT_API_REQ(BL_VALIDATE_FW, BL_VALIDATE_FW_EXT_API_REQUIRED,
		struct bl_validate_fw_ext_api, bl_validate_fw);

bool bl_validate_firmware(u32_t fw_dst_address, u32_t fw_src_address)
{
#ifdef CONFIG_BL_VALIDATE_FW_EXT_API_OPTIONAL
	if (!bl_validate_firmware_available()) {
		return false;
	}
#endif
	return bl_validate_fw->ext_api.bl_validate_firmware(fw_dst_address,
							fw_src_address);
}


#else
#include <errno.h>
#include <sys/printk.h>
#include <toolchain.h>
#include <bl_crypto.h>

#define PRINT(...) if (!external) printk(__VA_ARGS__)

struct __packed fw_validation_info {
	/* Magic value to verify that the struct has the correct type. */
	u32_t magic[MAGIC_LEN_WORDS];

	/* The address of the start (vector table) of the firmware. */
	u32_t address;

	/* The hash of the firmware.*/
	u8_t  hash[CONFIG_SB_HASH_LEN];

	/* Public key to be used for signature verification. This must be
	 * checked against a trusted hash.
	 */
	u8_t  public_key[CONFIG_SB_PUBLIC_KEY_LEN];

	/* Signature over the firmware as represented by the address and size in
	 * the firmware_info.
	 */
	u8_t  signature[CONFIG_SB_SIGNATURE_LEN];
};


/* Static asserts to ensure compatibility */
OFFSET_CHECK(struct fw_validation_info, magic, 0);
OFFSET_CHECK(struct fw_validation_info, address, 12);
OFFSET_CHECK(struct fw_validation_info, hash, 16);
OFFSET_CHECK(struct fw_validation_info, public_key, (16 + CONFIG_SB_HASH_LEN));
OFFSET_CHECK(struct fw_validation_info, signature, (16 + CONFIG_SB_HASH_LEN
	+ CONFIG_SB_SIGNATURE_LEN));

/* Can be used to make the firmware discoverable in other locations, e.g. when
 * searching backwards. This struct would typically be constructed locally, so
 * it needs no version.
 */
struct __packed fw_validation_pointer {
	u32_t magic[MAGIC_LEN_WORDS];
	const struct fw_validation_info *validation_info;
};

/* Static asserts to ensure compatibility */
OFFSET_CHECK(struct fw_validation_pointer, magic, 0);
OFFSET_CHECK(struct fw_validation_pointer, validation_info, 12);

static bool validation_info_check(const struct fw_validation_info *vinfo)
{
	const u32_t validation_info_magic[] = {VALIDATION_INFO_MAGIC};

	if (memcmp(vinfo->magic, validation_info_magic,
					CONFIG_FW_INFO_MAGIC_LEN) == 0) {
		return vinfo;
	}
	return NULL;
}


/* Find the validation_info at the end of the firmware. */
static const struct fw_validation_info *
validation_info_find(u32_t start_address, u32_t search_distance)
{
	const struct fw_validation_info *vinfo;

	for (int i = 0; i <= search_distance; i++) {
		vinfo = (const struct fw_validation_info *)(start_address + i);
		if (validation_info_check(vinfo)) {
			return vinfo;
		}
	}
	return NULL;
}


#ifdef CONFIG_SB_VALIDATE_FW_SIGNATURE
static bool validate_signature(const u32_t fw_src_address,
				const u32_t fw_size,
				const struct fw_validation_info *fw_val_info,
				bool external)
{
	bool validated = false;
	int init_retval = bl_crypto_init();

	if (init_retval) {
		PRINT("bl_crypto_init() returned %d.\n\r", init_retval);
		return false;
	}

	int retval = -EINVAL;
	u32_t num_public_keys = num_public_keys_read();
	bl_root_of_trust_verify_t rot_verify = external ?
					bl_root_of_trust_verify_external :
					bl_root_of_trust_verify;
	/* Some key data storage backends require word sized reads, hence
	 * we need to ensure word alignment for 'key_data'
	 */
	__aligned(4) u8_t key_data[CONFIG_SB_PUBLIC_KEY_HASH_LEN];

	for (u32_t key_data_idx = 0; key_data_idx < num_public_keys;
			key_data_idx++) {
		int read_retval = public_key_data_read(key_data_idx,
				key_data, CONFIG_SB_PUBLIC_KEY_HASH_LEN);
		if (read_retval < 0) {
			if (read_retval == -EINVAL) {
				/* Invalidated key, try next key. */
				PRINT("Key %d has been invalidated.\n\r",
					key_data_idx);
				retval = -EINVAL;
				continue;
			} else if (read_retval == -EHASHFF) {
				PRINT("A public key is 0xFFFF, which is "
					"unsupported\n\r");
				retval = -EHASHFF;
				break;
			} else {
				PRINT("public_key_data_read failed: %d.\n\r",
					read_retval);
				retval = -EFAULT;
				break;
			}
		}

		PRINT("Verifying signature against key %d.\n\r", key_data_idx);
		PRINT("Hash: 0x%02x...%02x\r\n", key_data[0],
			key_data[CONFIG_SB_PUBLIC_KEY_HASH_LEN-1]);
		retval = rot_verify(fw_val_info->public_key,
					key_data,
					fw_val_info->signature,
					(const u8_t *)fw_src_address,
					fw_size);

		if (retval == 0) {
			for (u32_t i = 0; i < key_data_idx; i++) {
				PRINT("Invalidating key %d.\n\r", i);
				invalidate_public_key(i);
			}
			validated = true;
		}
		if (retval != -EHASHINV) {
			break;
		}
	}

	if (retval != 0) {
		PRINT("Firmware validation failed with error %d.\n\r",
			    retval);
		return false;
	}

	if (validated) {
		PRINT("Firmware signature verified.\n\r");
	} else {
		PRINT("Failed to validate signature.\n\r");
	}

	return validated;
}


#elif defined(CONFIG_SB_VALIDATE_FW_HASH)
static bool validate_hash(const u32_t fw_src_address, const u32_t fw_size,
			const struct fw_validation_info *fw_val_info,
			bool external)
{
	int retval = bl_crypto_init();

	if (retval) {
		PRINT("bl_crypto_init() returned %d.\n\r", retval);
		return false;
	}

	retval = bl_sha256_verify((const u8_t *)fw_src_address, fw_size,
			fw_val_info->hash);

	if (retval != 0) {
		PRINT("Firmware validation failed with error %d.\n\r",
			    retval);
		return false;
	}

	PRINT("Firmware hash verified.\n\r");

	return true;
}
#endif


static bool within(u32_t addr, u32_t start, u32_t end)
{
	if (start > end) {
		// PRINT("Invalid region (end before start).\n");
		return false;
	}
	if (addr < start) {
		// PRINT("address not within region (before).\n");
		return false;
	}
	if (addr >= end) {
		// PRINT("address not within region (after).\n");
		return false;
	}
	return true;
}

static bool region_within(u32_t inner_start, u32_t inner_end,
			u32_t start, u32_t end)
{
	if (inner_start > inner_end) {
		// PRINT("Invalid inner region (end before start).\n");
		return false;
	}
	if (!within(inner_start, start, end)) {
		// PRINT("inner region start not within region.\n");
		return false;
	}
	if (!within(inner_end, start, end)) {
		// PRINT("inner region end not within region.\n");
		return false;
	}
	return true;
}


static bool validate_firmware(u32_t fw_dst_address, u32_t fw_src_address,
		const struct fw_info *fwinfo, bool external)
{
	const struct fw_validation_info *fw_val_info;

	if (!fwinfo) {
		PRINT("NULL parameter.\n\r");
		return false;
	}

	if (!fw_info_check((u32_t)fwinfo)) {
		PRINT("Invalid firmware info format.\n\r");
		return false;
	}

	if (fw_dst_address != fwinfo->address) {
		PRINT("The firmware doesn't belong at destination addr.\n\r");
		return false;
	}

	if (!external && (fw_src_address != fw_dst_address)) {
		PRINT("src and dst must be equal for local calls.\n\r");
		return false;
	}

	if (fw_info_find(fw_src_address) != fwinfo) {
		PRINT("Firmware info doesn't point to itself.\n\r");
		return false;
	}

	if (fwinfo->valid != CONFIG_FW_INFO_VALID_VAL) {
		PRINT("Firwmare has been invalidated: 0x%x.\n\r",
			fwinfo->valid);
		return false;
	}

	if (fwinfo->version < get_monotonic_version(NULL)) {
		PRINT("Firmware version (%u) is smaller than monotonic counter "
			"(%u).\n\r", fwinfo->version,
			get_monotonic_version(NULL));
		return false;
	}

	if ((fwinfo->size > (PM_S0_SIZE))
		|| (fwinfo->size > (PM_S1_SIZE))
		|| (fwinfo->total_size > fwinfo->size)) {
		PRINT("Invalid size or total_size in firmware info.\n\r");
		return false;
	}

	if (!region_within((u32_t)fwinfo, (u32_t)fwinfo + fwinfo->total_size,
			fw_src_address, (fw_src_address + fwinfo->size))) {
		PRINT("Firmware info is not within signed region.\n\r");
		return false;
	}

	if (!within(fwinfo->boot_address,
			fw_dst_address, (fw_dst_address + fwinfo->size))) {
		PRINT("Boot address handler is not within signed region.\n\r");
		return false;
	}

	if (!within(((const u32_t *)(fwinfo->boot_address))[1],
			fw_dst_address, (fw_dst_address + fwinfo->size))) {
		PRINT("Reset handler is not within signed region.\n\r");
		return false;
	}

	fw_val_info = validation_info_find(fw_src_address + fwinfo->size, 4);

	if (!fw_val_info) {
		PRINT("Could not find valid firmware validation info.\n\r");
		return false;
	}

	if (fw_val_info->address != fwinfo->address) {
		PRINT("Validation info doesn't belong to this firmware.\n\r");
		return false;
	}

#ifdef CONFIG_SB_VALIDATE_FW_SIGNATURE
	return validate_signature(fw_src_address, fwinfo->size, fw_val_info,
				external);
#elif defined(CONFIG_SB_VALIDATE_FW_HASH)
	return validate_hash(fw_src_address, fwinfo->size, fw_val_info,
				external);
#else
	#error "Validation not specified."
#endif
}


bool bl_validate_firmware(u32_t fw_dst_address, u32_t fw_src_address)
{
	return validate_firmware(fw_dst_address, fw_src_address,
				fw_info_find(fw_src_address), true);
}


bool bl_validate_firmware_local(u32_t fw_address,
				const struct fw_info *fwinfo)
{
	return validate_firmware(fw_address, fw_address, fwinfo, false);
}
#endif

bool bl_validate_firmware_available(void)
{
#ifdef CONFIG_BL_VALIDATE_FW_EXT_API_OPTIONAL
	return (bl_validate_fw != NULL);
#else
	return true;
#endif
}


#ifdef CONFIG_BL_VALIDATE_FW_EXT_API_ENABLED
EXT_API(BL_VALIDATE_FW, struct bl_validate_fw_ext_api,
						bl_validate_fw_ext_api) = {
		.bl_validate_firmware = bl_validate_firmware,
	}
};
#endif
