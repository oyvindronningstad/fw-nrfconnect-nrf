/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef BOOTLOADER_CRYPTO_H__
#define BOOTLOADER_CRYPTO_H__

#include <zephyr/types.h>
#include <fw_metadata.h>

/* Placeholder defines. Values should be updated, if no existing errors can be
 * used instead. */
#define EHASHINV 101
#define ESIGINV  102


#if CONFIG_SB_CRYPTO_OBERON_SHA256
	#include <occ_sha256.h>
	#define SHA256_CTX_SIZE sizeof(occ_sha256_ctx)
	typedef occ_sha256_ctx bl_sha256_ctx_t;
#elif CONFIG_SB_CRYPTO_CC310_SHA256
	#include <nrf_cc310_bl_hash_sha256.h>
	#define SHA256_CTX_SIZE sizeof(nrf_cc310_bl_hash_context_sha256_t)
	typedef nrf_cc310_bl_hash_context_sha256_t bl_sha256_ctx_t;
#else
	#define SHA256_CTX_SIZE 128
	// u32_t to make sure it is aligned equally as the other contexts.
	typedef u32_t bl_sha256_ctx_t[SHA256_CTX_SIZE/4];
#endif

#define ROT_VERIFY_ABI_ID 0x1001
#define ROT_VERIFY_ABI_FLAGS 2
#define ROT_VERIFY_ABI_VER 1
#define ROT_VERIFY_ABI_MAX_VER 0xFF

#define BL_SHA256_ABI_ID 0x1002
#define BL_SHA256_ABI_FLAGS 0
#define BL_SHA256_ABI_VER 1
#define BL_SHA256_ABI_MAX_VER 0xFF

#define BL_SECP256R1_ABI_ID 0x1003
#define BL_SECP256R1_ABI_FLAGS 1
#define BL_SECP256R1_ABI_VER 1
#define BL_SECP256R1_ABI_MAX_VER 0xFF

/**
 * @brief Initialize bootloader crypto module.
 *
 * @retval 0        On success.
 * @retval -EFAULT  If crypto module reported an error.
 */
int crypto_init(void);


/**
 * @brief Verify a signature using configured signature and SHA-256
 *
 * Verifies the public key against the public key hash, then verifies the hash
 * of the signed data against the signature using the public key.
 *
 * @param[in]  public_key       Public key.
 * @param[in]  public_key_hash  Expected hash of the public key. This is the
 *                              root of trust.
 * @param[in]  signature        Firmware signature.
 * @param[in]  firmware         Firmware.
 * @param[in]  firmware_len     Length of firmware.
 *
 * @retval 0          On success.
 * @retval -EHASHINV  If public_key_hash didn't match public_key.
 * @retval -ESIGINV   If signature validation failed.
 * @return Any error code from @ref bl_sha256_init, @ref bl_sha256_update,
 *         @ref bl_sha256_finalize, or @ref bl_secp256r1_validate if something
 *         else went wrong.
 *
 * @remark No parameter can be NULL.
 */
EXT_ABI_FUNCTION(int, root_of_trust_verify, const u8_t *public_key,
					    const u8_t *public_key_hash,
					    const u8_t *signature,
					    const u8_t *firmware,
					    const u32_t firmware_len);

/**
 * @brief Initialize a sha256 operation context variable.
 *
 * @param[out]  ctx  Context to be initialized.
 *
 * @retval 0         On success.
 * @retval -EINVAL   If @p ctx was NULL.
 */
EXT_ABI_FUNCTION(int, bl_sha256_init, bl_sha256_ctx_t *ctx);

/**
 * @brief Hash a portion of data.
 *
 * @warning @p ctx must be initialized before being used in this function.
 *          An uninitialized @p ctx might not be reported as an error. Also,
 *          @p ctx must not be used if it has been finalized, though this might
 *          also not be reported as an error.
 *
 * @param[in]  ctx       Context variable. Must have been initialized.
 * @param[in]  data      Data to hash.
 * @param[in]  data_len  Length of @p data.
 *
 * @retval 0         On success.
 * @retval -EINVAL   If @p ctx was NULL, uninitialized, or corrupted.
 * @retval -ENOSYS   If the context has already been finalized.
 */
EXT_ABI_FUNCTION(int, bl_sha256_update, bl_sha256_ctx_t *ctx,
					const u8_t *data,
					u32_t data_len);

/**
 * @brief Finalize a hash result.
 *
 * @param[in]  ctx       Context variable.
 * @param[out] output    Where to put the resulting digest. Must be at least
 *                       32 bytes long.
 *
 * @retval 0         On success.
 * @retval -EINVAL   If @p ctx was NULL or corrupted, or @p output was NULL.
 */
EXT_ABI_FUNCTION(int, bl_sha256_finalize, bl_sha256_ctx_t *ctx,
					  u8_t *output);

/**
 * @brief Calculate a digest and verify it directly.
 *
 * @param[in]  data      The data to hash.
 * @param[in]  data_len  The length of @p data.
 * @param[in]  expected  The expected digest over @data.
 *
 * @retval 0          If the procedure succeeded and the resulting digest is
 *                    identical to @p expected.
 * @retval -EHASHINV  If the procedure succeeded, but the digests don't match.
 * @return Any error code from @ref bl_sha256_init, @ref bl_sha256_update, or
 *         @ref bl_sha256_finalize if something else went wrong.
 */
EXT_ABI_FUNCTION(int, bl_sha256_verify, const u8_t *data,
					u32_t data_len,
					const u8_t *expected);

/**
 * @brief Validate a secp256r1 signature.
 *
 * @param[in]  hash        The hash to validate against.
 * @param[in]  hash_len    The length of the hash.
 * @param[in]  signature   The signature to validate.
 * @param[in]  public_key  The public key to validate with.
 *
 * @retval 0         The operation succeeded and the signature is valid for the
 *                   hash.
 * @retval -EINVAL   A parameter was NULL, or the @hash_len was not 32 bytes.
 * @retval -ESIGINV  The signature validation failed.
 */
EXT_ABI_FUNCTION(int, bl_secp256r1_validate, const u8_t *hash,
					    u32_t hash_len,
					    const u8_t *signature,
					    const u8_t *public_key);

struct rot_verify_abi {
	struct fw_abi_info header;
	struct {
		root_of_trust_verify_t root_of_trust_verify;
	} abi;
};

struct bl_sha256_abi {
	struct fw_abi_info header;
	struct {
		bl_sha256_init_t bl_sha256_init;
		bl_sha256_update_t bl_sha256_update;
		bl_sha256_finalize_t bl_sha256_finalize;
		bl_sha256_verify_t bl_sha256_verify;
		u32_t bl_sha256_ctx_size;
	} abi;
};

struct bl_secp256r1_abi {
	struct fw_abi_info header;
	struct {
		bl_secp256r1_validate_t bl_secp256r1_validate;
	} abi;
};

#endif

