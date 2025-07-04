#include "hw_crypto.h"
#include "hw_crypto_curves.h"
#include "main.h"
#include "compiler.h"
#include "utils.h"

#include <assert.h>
#include <string.h>
#include <stdbool.h>

static ctap_crypto_status_t stm32h533_crypto_init_rng_peripheral(stm32h533_crypto_context_t *ctx);

static ctap_crypto_status_t stm32h533_crypto_init_pka_peripheral(stm32h533_crypto_context_t *ctx);

static ctap_crypto_status_t stm32h533_crypto_init_cryp_peripheral(stm32h533_crypto_context_t *ctx);

static ctap_crypto_status_t stm32h533_crypto_init_hash_peripheral(stm32h533_crypto_context_t *ctx);

ctap_crypto_status_t stm32h533_crypto_init(
	const ctap_crypto_t *const crypto,
	uint32_t seed
) {
	// we use True Random Number Generator (the RNG peripheral) to generate all random data directly
	// no seed can be set or reset
	lion_unused(seed);

	stm32h533_crypto_context_t *const ctx = crypto->context;

	memset(ctx, 0, sizeof(stm32h533_crypto_context_t));

	ctap_crypto_status_t status;

	status = stm32h533_crypto_init_rng_peripheral(ctx);
	if (status != CTAP_CRYPTO_OK) {
		return status;
	}

	status = stm32h533_crypto_init_pka_peripheral(ctx);
	if (status != CTAP_CRYPTO_OK) {
		return status;
	}

	status = stm32h533_crypto_init_cryp_peripheral(ctx);
	if (status != CTAP_CRYPTO_OK) {
		return status;
	}

	return stm32h533_crypto_init_hash_peripheral(ctx);
}

// ####################  RNG  ####################

void HAL_RNG_MspInit(RNG_HandleTypeDef *hrng) {
	RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
	if (hrng->Instance == RNG) {
		PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RNG;
		PeriphClkInitStruct.RngClockSelection = RCC_RNGCLKSOURCE_PLL1Q;
		if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) {
			Error_Handler();
		}
		__HAL_RCC_RNG_CLK_ENABLE();
	}
}

void HAL_RNG_MspDeInit(RNG_HandleTypeDef *hrng) {
	if (hrng->Instance == RNG) {
		__HAL_RCC_RNG_CLK_DISABLE();
	}
}

static ctap_crypto_status_t stm32h533_crypto_init_rng_peripheral(stm32h533_crypto_context_t *ctx) {
	RNG_HandleTypeDef *const hal_rng = &ctx->hal_rng;
	hal_rng->Instance = RNG;
	hal_rng->Init.ClockErrorDetection = RNG_CED_ENABLE;
	if (HAL_RNG_Init(hal_rng) != HAL_OK) {
		return CTAP_CRYPTO_ERROR;
	}
	return CTAP_CRYPTO_OK;
}

ctap_crypto_status_t stm32h533_crypto_rng_init(
	const ctap_crypto_t *const crypto,
	uint32_t seed
) {
	lion_unused(seed);
	lion_unused(crypto);
	// we use True Random Number Generator (the RNG peripheral) to generate all random data directly
	// no seed can be set or reset
	return CTAP_CRYPTO_OK;
}

ctap_crypto_status_t stm32h533_crypto_rng_generate_data(
	const ctap_crypto_t *const crypto,
	uint8_t *const buffer, const size_t length
) {
	stm32h533_crypto_context_t *const ctx = crypto->context;
	RNG_HandleTypeDef *const hal_rng = &ctx->hal_rng;

	HAL_StatusTypeDef status;
	uint32_t word;
	uint8_t *const word_bytes = (uint8_t *) &word;

	size_t i = 0;

	// full 32-bit words
	for (size_t next_length = 4; next_length <= length; i += 4, next_length += 4, ++word) {
		// We use lion_htole32() here to get consistent results independent of the target endianness.
		// When the target is little-endian (most targets are), the lion_htole32() macro does nothing.
		status = HAL_RNG_GenerateRandomNumber(hal_rng, &word);
		if (status != HAL_OK) {
			goto error;
		}
		buffer[i + 0] = word_bytes[0];
		buffer[i + 1] = word_bytes[1];
		buffer[i + 2] = word_bytes[2];
		buffer[i + 3] = word_bytes[3];
	}

	// remaining bytes (less than one 32-bit word, i.e., less than 4 bytes)
	if (i < length) {
		assert((length - i) < 4);
		status = HAL_RNG_GenerateRandomNumber(hal_rng, &word);
		if (status != HAL_OK) {
			goto error;
		}
		for (size_t j = 0; i < length; ++i, ++j) {
			buffer[i] = word_bytes[j];
		}
	}

	return CTAP_CRYPTO_OK;

	error:
	error_log(
		red("HAL_RNG_GenerateRandomNumber error: status = %d, ErrorCode = %" PRIx32) nl,
		status, hal_rng->ErrorCode
	);

	return CTAP_CRYPTO_ERROR;
}

// ####################  ECC (ECDSA and ECDH)  ####################

static const size_t stm32h533_crypto_ecc_max_num_tries = 2;

void HAL_PKA_MspInit(PKA_HandleTypeDef *hpka) {
	if (hpka->Instance == PKA) {
		__HAL_RCC_PKA_CLK_ENABLE();
	}
}

void HAL_PKA_MspDeInit(PKA_HandleTypeDef *hpka) {
	if (hpka->Instance == PKA) {
		__HAL_RCC_PKA_CLK_DISABLE();
	}
}

ctap_crypto_status_t stm32h533_crypto_init_pka_peripheral(stm32h533_crypto_context_t *ctx) {
	PKA_HandleTypeDef *const hal_pka = &ctx->hal_pka;
	hal_pka->Instance = PKA;
	if (HAL_PKA_Init(hal_pka) != HAL_OK) {
		return CTAP_CRYPTO_ERROR;
	}
	return CTAP_CRYPTO_OK;
}

static bool are_all_bytes_zero(const uint8_t *const bytes, const size_t num_bytes) {
	// if we had a guarantee that the bytes array is word (32-bit) aligned,
	// we could iterate over uint32_t
	uint8_t value = 0;
	for (size_t i = 0; i < num_bytes; ++i) {
		value |= bytes[i];
	}
	return (value == 0);
}

ctap_crypto_status_t stm32h533_crypto_ecc_secp256r1_compute_public_key(
	const ctap_crypto_t *const crypto,
	const uint8_t *const private_key,
	uint8_t *const public_key
) {
	const uint32_t t1 = HAL_GetTick();

	// make sure the private key is in the range [1, n-1]

	stm32h533_crypto_context_t *const ctx = crypto->context;
	PKA_HandleTypeDef *const hal_pka = &ctx->hal_pka;

	PKA_ECCMulInTypeDef in;

	// static params for the curve secp256r1 (P-256 = secp256r1 = prime256v1)
	in.scalarMulSize = stm32h533_secp256r1.prime_order_size;
	in.modulusSize = stm32h533_secp256r1.modulus_size;
	in.coefSign = stm32h533_secp256r1.a_sign;
	in.coefA = stm32h533_secp256r1.abs_a;
	in.coefB = stm32h533_secp256r1.b;
	in.modulus = stm32h533_secp256r1.p;
	in.pointX = stm32h533_secp256r1.xG; // point P coordinate xP
	in.pointY = stm32h533_secp256r1.yG; // point P coordinate yP
	in.primeOrder = stm32h533_secp256r1.n;


	// dynamic params
	in.scalarMul = private_key; // scalar multiplier k
	// RM0481 36.5.15 ECC Fp scalar multiplication
	//   For k = 0 this function returns a point at infinity (0, 0)
	//   if curve parameter b is nonzero, (0, 1) otherwise.
	//   For k different from 0 it might happen that a point at infinity is returned.
	//   When the application detects this behavior a new computation must be carried out.

	HAL_StatusTypeDef status = HAL_PKA_ECCMul(hal_pka, &in, HAL_MAX_DELAY);

	if (status != HAL_OK) {
		const uint32_t t2 = HAL_GetTick();
		const uint32_t output_error_code = hal_pka->Instance->RAM[PKA_ECC_SCALAR_MUL_OUT_ERROR];
		error_log(
			red("HAL_PKA_ECCMul error: duration = %" PRIu32 " ms, status = %d, ErrorCode = %" PRIx32 ", PKA output = %" PRIx32) nl,
			t2 - t1, status, hal_pka->ErrorCode, output_error_code
		);
		return CTAP_CRYPTO_ERROR;
	}

	PKA_ECCMulOutTypeDef out;
	out.ptX = public_key;
	out.ptY = public_key + 32;
	HAL_PKA_ECCMul_GetResult(hal_pka, &out);

	if (are_all_bytes_zero(public_key, 64)) {
		const uint32_t t2 = HAL_GetTick();
		error_log(
			red("ecc_secp256r1_compute_public_key: error public_key is (0,0), computed in %" PRIu32 " ms") nl,
			t2 - t1
		);
		return CTAP_CRYPTO_ERROR;
	}

	const uint32_t t2 = HAL_GetTick();
	debug_log("ecc_secp256r1_compute_public_key took %" PRIu32 "ms" nl, t2 - t1);

	return CTAP_CRYPTO_OK;
}

static ctap_crypto_status_t stm32h533_crypto_generate_random_k(
	const ctap_crypto_t *const crypto,
	const stm32h533_crypto_ecc_curve_t *const curve,
	uint8_t *const k,
	size_t max_num_tries
) {
	while (max_num_tries > 0) {
		max_num_tries--;
		if (stm32h533_crypto_rng_generate_data(crypto, k, curve->prime_order_size) == CTAP_CRYPTO_OK) {
			return CTAP_CRYPTO_OK;
		}
		// Consider checking that 0 < k < n
		// Note:
		//   1. stm32h533_crypto_rng_generate_data() will never generate 0
		//      (in fact, RNG never generates zero 32-bit word).
		//   2. PKA ECDSA sign operation fails with an error if k == 0.
		//   3. PKA ECDSA sign seems to work with k >= n. uECC_sign() strictly checks that 0 < k < n.
		//      However, a signature generated with k >= n using PKA ECDSA sign can be successfully
		//      verified using uECC_verify().
	}
	return CTAP_CRYPTO_ERROR;
}

ctap_crypto_status_t stm32h533_crypto_ecc_secp256r1_sign(
	const ctap_crypto_t *const crypto,
	const uint8_t *const private_key,
	const uint8_t *const message_hash,
	const size_t message_hash_size,
	uint8_t *const signature,
	const uint8_t *const optional_fixed_k
) {
	if (message_hash_size != stm32h533_secp256r1.prime_order_size) {
		return CTAP_CRYPTO_ERROR;
	}

	const uint32_t t1 = HAL_GetTick();

	const uint8_t *k = optional_fixed_k;
	uint8_t random_k[stm32h533_secp256r1.prime_order_size];

	if (k == NULL) {
		if (stm32h533_crypto_generate_random_k(
			crypto,
			&stm32h533_secp256r1,
			random_k,
			stm32h533_crypto_ecc_max_num_tries
		) != CTAP_CRYPTO_OK) {
			error_log(red("stm32h533_crypto_ecc_secp256r1_sign: failed to generate valid random k") nl);
			return CTAP_CRYPTO_ERROR;
		}
		k = random_k;
	}

	stm32h533_crypto_context_t *const ctx = crypto->context;
	PKA_HandleTypeDef *const hal_pka = &ctx->hal_pka;

	PKA_ECDSASignInTypeDef in;

	// static params for the curve secp256r1 (P-256 = secp256r1 = prime256v1)
	in.primeOrderSize = stm32h533_secp256r1.prime_order_size;
	in.modulusSize = stm32h533_secp256r1.modulus_size;
	in.coefSign = stm32h533_secp256r1.a_sign;
	in.coef = stm32h533_secp256r1.abs_a;
	in.coefB = stm32h533_secp256r1.b;
	in.modulus = stm32h533_secp256r1.p;
	in.basePointX = stm32h533_secp256r1.xG;
	in.basePointY = stm32h533_secp256r1.yG;
	in.primeOrder = stm32h533_secp256r1.n;

	// dynamic params
	in.integer = k; // random integer k (0 < k < n) (prime_order_size bytes)
	in.hash = message_hash; // hash of the message (prime_order_size bytes)
	in.privateKey = private_key; // private key d (prime_order_size bytes)

	HAL_StatusTypeDef status;

	for (size_t attempt = 0; attempt < stm32h533_crypto_ecc_max_num_tries; ++attempt) {

		status = HAL_PKA_ECDSASign(hal_pka, &in, HAL_MAX_DELAY);

		if (status == HAL_OK) {

			PKA_ECDSASignOutTypeDef out;
			out.RSign = signature;
			out.SSign = signature + 32;
			HAL_PKA_ECDSASign_GetResult(hal_pka, &out, NULL);

			const uint32_t t2 = HAL_GetTick();
			debug_log("ecc_secp256r1_sign took %" PRIu32 "ms" nl, t2 - t1);

			return CTAP_CRYPTO_OK;

		}

		// RM0481 36.5.16 ECDSA sign
		//   The application has to check if the output error is equal to 0xD60D,
		//   if it is different a new k must be generated and the ECDSA sign operation must be repeated.
		//   HAL_PKA_ECDSASign() returns HAL_OK iff the output error is equal to 0xD60D (successful computation)
		//   In other cases, it returns HAL_ERROR.
		//   The Table 368. ECDSA sign - Outputs (RM0481 36.5.16 ECDSA sign) lists all possible output error codes?
		//     0xD60D: successful computation, no error
		//     0xCBC9: failed computation
		//     0xA3B7: signature part r is equal to 0
		//     0xF946: signature part s is equal to 0
		const uint32_t t2 = HAL_GetTick();
		const uint32_t output_error_code = hal_pka->Instance->RAM[PKA_ECDSA_SIGN_OUT_ERROR];
		error_log(
			red("HAL_PKA_ECDSASign error: attempt = %" PRIsz ", total duration = %" PRIu32 " ms, status = %d, ErrorCode = %" PRIx32 ", PKA ECDSA sign output = %" PRIx32) nl,
			attempt, t2 - t1, status, hal_pka->ErrorCode, output_error_code
		);

	}

	error_log(red("HAL_PKA_ECDSASign() max attempts (%" PRIsz ") reached") nl, stm32h533_crypto_ecc_max_num_tries);

	return CTAP_CRYPTO_ERROR;
}

// secp256r1
//   n = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
//   -> Montgomery param = 0x1c1f0858d0b168a4619076ab51d16bdbdf119f1b30a9cdc75706acb03af42abb
//      We precomputed it using PKA (see the commented below in stm32h533_crypto_ecc_secp256r1_shared_secret()).
//      We were also able to compute the same value using the Python code from
//        https://community.st.com/t5/stm32-mcus-products/stm32-pka-rsa-montgomery-parameter/m-p/652031/highlight/true#M238683
// Montgomery param has the same number of bytes as the n itself (32 bytes, 256 bits)
// Montgomery param = 0x1c1f0858d0b168a4619076ab51d16bdbdf119f1b30a9cdc75706acb03af42abb
// Here is stored in 32-bit words, exactly as it is produced by HAL_PKA_MontgomeryParam()
// and accepted by HAL_PKA_PointCheck_IsOnCurve().
// static const uint32_t secp256r1_montgomery_param[8] = {
// 	0x3af42abd, 0x5706acb0, 0x30a9cdc7, 0xdf119f1b,
// 	0x51d16bdb, 0x619076ab, 0xd0b168a4, 0x1c1f0858,
// };

ctap_crypto_status_t stm32h533_crypto_ecc_secp256r1_shared_secret(
	const ctap_crypto_t *const crypto,
	const uint8_t *const public_key,
	const uint8_t *const private_key,
	uint8_t *const secret
) {
	const uint32_t t1 = HAL_GetTick();

	// make sure the private key is in the range [1, n-1]

	stm32h533_crypto_context_t *const ctx = crypto->context;
	PKA_HandleTypeDef *const hal_pka = &ctx->hal_pka;

	HAL_StatusTypeDef status;

	// Compute the Montgomery parameter (R^2 mod n) used by PKA to convert operands
	// into the Montgomery residue system representation.
	// It is required for the Point on elliptic curve Fp check (HAL_PKA_PointCheck()).
	// This can be precomputed beforehand (we did that, see the result in secp256r1_montgomery_param).
	// {
	// 	PKA_MontgomeryParamInTypeDef in;
	//
	// 	// static params for the curve secp256r1 (P-256 = secp256r1 = prime256v1)
	// 	in.size = stm32h533_secp256r1.modulus_size;
	// 	in.pOp1 = stm32h533_secp256r1.n;
	//
	// 	status = HAL_PKA_MontgomeryParam(hal_pka, &in, HAL_MAX_DELAY);
	// 	if (status != HAL_OK) {
	// 		const uint32_t t2 = HAL_GetTick();
	// 		const uint32_t output_error_code = hal_pka->Instance->RAM[PKA_MONTGOMERY_PARAM_OUT_PARAMETER];
	// 		error_log(
	// 			red("HAL_PKA_MontgomeryParam error: duration = %" PRIu32 " ms, status = %d, ErrorCode = %" PRIx32 ", PKA output = %" PRIx32) nl,
	// 			t2 - t1, status, hal_pka->ErrorCode, output_error_code
	// 		);
	// 		return CTAP_CRYPTO_ERROR;
	// 	}
	//
	// 	assert(stm32h533_secp256r1.modulus_size == sizeof(secp256r1_montgomery_param));
	// 	assert(stm32h533_secp256r1.modulus_size % 4 == 0);
	// 	uint32_t montgomery_param[stm32h533_secp256r1.modulus_size / 4];
	//
	// 	// we checked that HAL_PKA_MontgomeryParam() succeeded, so we can retrieve the result
	// 	HAL_PKA_MontgomeryParam_GetResult(hal_pka, montgomery_param);
	//
	// 	debug_log("montgomery_param ");
	// 	dump_hex((const uint8_t *) montgomery_param, stm32h533_secp256r1.modulus_size);
	//
	// 	assert(memcmp(montgomery_param, secp256r1_montgomery_param, sizeof(secp256r1_montgomery_param)) == 0);
	//
	// 	return CTAP_CRYPTO_ERROR;
	//
	// }

	// A valid public key is a point (x, y) that lies on the curve (it satisfies the curve equation).
	// Before using a public key to compute the shared secret (performing an ECC Fp scalar multiplication
	// using HAL_PKA_ECCMul()), we first check that the public key is a valid curve point
	// (when performing ECDH, any value can be supplied as the public key by the other party or an attacker).
	// Note that when an invalid point is passed to PKA, it seems that ECC Fp scalar multiplication operation
	// correctly fails with an error. However, the reference manual (RM0481, Section 36.5.1) states that
	// "The validity of all input parameters to the PKA must be checked before starting any operation,
	// as PKA assumes that all of them are valid and consistent with each other.".
	// Therefore, we rather validate the public key (the point) explicitly performing
	// the ECC Fp scalar multiplication operation.
	// TODO:
	//    We temporarily disabled the point on curve check as it does not currently seem to work.
	//    The PKA "Point on elliptic curve Fp check" currently seems to always return
	//    the "0xA3B7: point not on curve" error, even though the given point is valid
	//    and the PKA "ECC Fp scalar multiplication" succeeds and returns correct result.
	//    Note that when an invalid point is passed to PKA "ECC Fp scalar multiplication" operation,
	//    it correctly fails with an error (hopefully it does that always). So the missing check should be okay.
	//    But we should still try to fix the PKA "Point on elliptic curve Fp check" and re-enable it.
	// {
	//
	// 	PKA_PointCheckInTypeDef in;
	//
	// 	// static params for the curve secp256r1 (P-256 = secp256r1 = prime256v1)
	// 	in.modulusSize = stm32h533_secp256r1.modulus_size;
	// 	in.coefSign = stm32h533_secp256r1.a_sign;
	// 	in.coefA = stm32h533_secp256r1.abs_a;
	// 	in.coefB = stm32h533_secp256r1.b;
	// 	in.modulus = stm32h533_secp256r1.p;
	// 	in.pMontgomeryParam = secp256r1_montgomery_param;
	//
	// 	// dynamic params
	// 	in.pointX = public_key; // point P coordinate xP
	// 	in.pointY = public_key + 32; // point P coordinate yP
	//
	// 	status = HAL_PKA_PointCheck(hal_pka, &in, HAL_MAX_DELAY);
	//
	// 	if (status != HAL_OK) {
	// 		const uint32_t t2 = HAL_GetTick();
	// 		const uint32_t output_error_code = hal_pka->Instance->RAM[PKA_POINT_CHECK_OUT_ERROR];
	// 		error_log(
	// 			red("HAL_PKA_PointCheck error: duration = %" PRIu32 " ms, status = %d, ErrorCode = %" PRIx32 ", PKA output = %" PRIx32) nl,
	// 			t2 - t1, status, hal_pka->ErrorCode, output_error_code
	// 		);
	// 		return CTAP_CRYPTO_ERROR;
	// 	}
	//
	// 	// we checked that HAL_PKA_PointCheck() succeeded, so we can retrieve the result
	// 	if (HAL_PKA_PointCheck_IsOnCurve(hal_pka) == 0) {
	// 		const uint32_t t2 = HAL_GetTick();
	// 		const uint32_t output_error_code = hal_pka->Instance->RAM[PKA_POINT_CHECK_OUT_ERROR];
	// 		error_log(
	// 			red("ecc_secp256r1_shared_secret error: the public key is not a valid point the secp256r1, duration = %" PRIu32 " ms, status = %d, ErrorCode = %" PRIx32 ", PKA output = %" PRIx32) nl,
	// 			t2 - t1, status, hal_pka->ErrorCode, output_error_code
	// 		);
	// 		// return CTAP_CRYPTO_ERROR;
	// 	}
	//
	// }

	PKA_ECCMulInTypeDef in;

	// static params for the curve secp256r1 (P-256 = secp256r1 = prime256v1)
	in.scalarMulSize = stm32h533_secp256r1.prime_order_size;
	in.modulusSize = stm32h533_secp256r1.modulus_size;
	in.coefSign = stm32h533_secp256r1.a_sign;
	in.coefA = stm32h533_secp256r1.abs_a;
	in.coefB = stm32h533_secp256r1.b;
	in.modulus = stm32h533_secp256r1.p;
	in.primeOrder = stm32h533_secp256r1.n;

	// dynamic params
	in.pointX = public_key; // point P coordinate xP
	in.pointY = public_key + 32; // point P coordinate yP
	in.scalarMul = private_key; // scalar multiplier k
	// RM0481 36.5.15 ECC Fp scalar multiplication
	//   For k = 0 this function returns a point at infinity (0, 0)
	//   if curve parameter b is nonzero, (0, 1) otherwise.
	//   For k different from 0 it might happen that a point at infinity is returned.
	//   When the application detects this behavior a new computation must be carried out.

	status = HAL_PKA_ECCMul(hal_pka, &in, HAL_MAX_DELAY);

	if (status != HAL_OK) {
		const uint32_t t2 = HAL_GetTick();
		const uint32_t output_error_code = hal_pka->Instance->RAM[PKA_ECC_SCALAR_MUL_OUT_ERROR];
		error_log(
			red("HAL_PKA_ECCMul error: duration = %" PRIu32 " ms, status = %d, ErrorCode = %" PRIx32 ", PKA output = %" PRIx32) nl,
			t2 - t1, status, hal_pka->ErrorCode, output_error_code
		);
		return CTAP_CRYPTO_ERROR;
	}

	uint8_t shared_point_y[32];
	PKA_ECCMulOutTypeDef out;
	out.ptX = secret; // as the shared secret we return just the shared point's x-coordinate
	out.ptY = shared_point_y;
	HAL_PKA_ECCMul_GetResult(hal_pka, &out);

	// the computed shared point must not be (0,0)
	if (are_all_bytes_zero(secret, 32) && are_all_bytes_zero(shared_point_y, 32)) {
		const uint32_t t2 = HAL_GetTick();
		error_log(
			red("ecc_secp256r1_shared_secret error: the computed shared point is (0,0), computed in %" PRIu32 " ms") nl,
			t2 - t1
		);
		return CTAP_CRYPTO_ERROR;
	}

	const uint32_t t2 = HAL_GetTick();
	debug_log("ecc_secp256r1_shared_secret took %" PRIu32 "ms" nl, t2 - t1);

	return CTAP_CRYPTO_OK;
}

// ####################  AES  ####################

void HAL_CRYP_MspInit(CRYP_HandleTypeDef *hcryp) {
	if (hcryp->Instance == AES) {
		__HAL_RCC_AES_CLK_ENABLE();
	}
}

void HAL_CRYP_MspDeInit(CRYP_HandleTypeDef *hcryp) {
	if (hcryp->Instance == AES) {
		__HAL_RCC_AES_CLK_DISABLE();
	}
}

static ctap_crypto_status_t stm32h533_crypto_init_cryp_peripheral(stm32h533_crypto_context_t *ctx) {

	CRYP_HandleTypeDef *const hal_cryp = &ctx->hal_cryp;

	hal_cryp->Instance = AES;
	// Note that the swapping configuration only applies to the data (writing AES_DINR and reading AES_DOUT),
	// but not to the IV and the key (AES_IVR0-3, AES_KEY0-7).
	hal_cryp->Init.DataType = CRYP_BYTE_SWAP;
	hal_cryp->Init.KeySize = CRYP_KEYSIZE_256B;
	//
	// hal_cryp->Init.pKey = (uint32_t *) NULL;
	// hal_cryp->Init.pInitVect = (uint32_t *) NULL;
	hal_cryp->Init.Algorithm = CRYP_AES_CBC;
	hal_cryp->Init.DataWidthUnit = CRYP_DATAWIDTHUNIT_BYTE;
	hal_cryp->Init.HeaderWidthUnit = CRYP_HEADERWIDTHUNIT_WORD;
	// When KeyIVConfigSkip == CRYP_KEYIVCONFIG_ONCE, the IV and the key are initialized
	// during first encryption/description call and hal_cryp->KeyIVConfig is set internally to 1U.
	// We want to use custom functions for initializing the IV and key (to perform byte swapping
	// without the need for extra copy). Therefore, we manually set the hal_cryp->KeyIVConfig to 1U.
	// Note that this must be done AFTER the HAL_CRYP_Init() call
	// (because HAL_CRYP_Init() sets hal_cryp->KeyIVConfig to 0U).
	hal_cryp->Init.KeyIVConfigSkip = CRYP_KEYIVCONFIG_ONCE; // We hal_cryp->KeyIVConfig = 1U
	hal_cryp->Init.KeyMode = CRYP_KEYMODE_NORMAL;
	if (HAL_CRYP_Init(hal_cryp) != HAL_OK) {
		return CTAP_CRYPTO_ERROR;
	}
	// This ensures that the IV and key initialization are NEVER performed by the HAL.
	// We use custom functions that perform byte swapping efficiently.
	hal_cryp->KeyIVConfig = 1U;

	return CTAP_CRYPTO_OK;

}

static void CRYP_AES_256_SetKey_Swap(CRYP_HandleTypeDef *hal_cryp, const uint32_t *key) {
	hal_cryp->Instance->KEYR7 = lion_bswap32(*(uint32_t *) (key));
	hal_cryp->Instance->KEYR6 = lion_bswap32(*(uint32_t *) (key + 1U));
	hal_cryp->Instance->KEYR5 = lion_bswap32(*(uint32_t *) (key + 2U));
	hal_cryp->Instance->KEYR4 = lion_bswap32(*(uint32_t *) (key + 3U));
	hal_cryp->Instance->KEYR3 = lion_bswap32(*(uint32_t *) (key + 4U));
	hal_cryp->Instance->KEYR2 = lion_bswap32(*(uint32_t *) (key + 5U));
	hal_cryp->Instance->KEYR1 = lion_bswap32(*(uint32_t *) (key + 6U));
	hal_cryp->Instance->KEYR0 = lion_bswap32(*(uint32_t *) (key + 7U));
}

static void CRYP_AES_256_SetIV_Swap(CRYP_HandleTypeDef *hal_cryp, const uint32_t *iv) {
	hal_cryp->Instance->IVR3 = lion_bswap32(*(uint32_t *) (iv));
	hal_cryp->Instance->IVR2 = lion_bswap32(*(uint32_t *) (iv + 1U));
	hal_cryp->Instance->IVR1 = lion_bswap32(*(uint32_t *) (iv + 2U));
	hal_cryp->Instance->IVR0 = lion_bswap32(*(uint32_t *) (iv + 3U));
}

#define CRYP_OPERATING_MODE_KEY_DERIVATION  AES_CR_MODE_0
#define CRYP_OPERATING_MODE_DECRYPT         AES_CR_MODE_1

// copied (slightly modified to match our code style) from stm32h5xx_hal_cryp.c
// (copied because it is defined as static, but we need for our custom CRYP_AES_256_CBC_SetDecryptKey())
static HAL_StatusTypeDef CRYP_WaitOnCCFlag(CRYP_HandleTypeDef *hal_cryp, uint32_t timeout) {
	uint32_t tick_start = HAL_GetTick();
	while (HAL_IS_BIT_CLR(hal_cryp->Instance->ISR, AES_ISR_CCF)) {
		if (timeout != HAL_MAX_DELAY) {
			if (((HAL_GetTick() - tick_start) > timeout) || (timeout == 0U)) {
				__HAL_CRYP_DISABLE(hal_cryp);
				hal_cryp->ErrorCode |= HAL_CRYP_ERROR_TIMEOUT;
				hal_cryp->State = HAL_CRYP_STATE_READY;
				__HAL_UNLOCK(hal_cryp);
				return HAL_ERROR;
			}
		}
	}
	return HAL_OK;
}

static HAL_StatusTypeDef CRYP_AES_256_CBC_SetDecryptKey(
	CRYP_HandleTypeDef *hal_cryp,
	const uint32_t *key,
	uint32_t timeout
) {
	// key preparation for decryption in CBC mode
	// see RM0481 33.4.9 AES basic chaining modes (ECB, CBC), ECB and CBC encryption process
	MODIFY_REG(hal_cryp->Instance->CR, AES_CR_KMOD, CRYP_KEYMODE_NORMAL);
	MODIFY_REG(hal_cryp->Instance->CR, AES_CR_MODE, CRYP_OPERATING_MODE_KEY_DERIVATION);
	CRYP_AES_256_SetKey_Swap(hal_cryp, key);
	__HAL_CRYP_ENABLE(hal_cryp);
	if (CRYP_WaitOnCCFlag(hal_cryp, timeout) != HAL_OK) {
		return HAL_ERROR;
	}
	__HAL_CRYP_CLEAR_FLAG(hal_cryp, CRYP_CLEAR_CCF);
	MODIFY_REG(hal_cryp->Instance->CR, AES_CR_MODE, CRYP_OPERATING_MODE_DECRYPT);
	return HAL_OK;
}

ctap_crypto_status_t stm32h533_crypto_aes_256_cbc_encrypt(
	const ctap_crypto_t *const crypto,
	const uint8_t *iv,
	const uint8_t *key,
	uint8_t *data,
	const size_t data_length
) {
	if (data_length == 0) {
		return CTAP_CRYPTO_OK;
	}
	if (data_length % CTAP_CRYPTO_AES_BLOCK_SIZE) {
		return CTAP_CRYPTO_ERROR;
	}
	stm32h533_crypto_context_t *const ctx = crypto->context;
	CRYP_HandleTypeDef *const hal_cryp = &ctx->hal_cryp;
	CRYP_AES_256_SetKey_Swap(hal_cryp, (const uint32_t *) key);
	CRYP_AES_256_SetIV_Swap(hal_cryp, (const uint32_t *) iv);
	HAL_StatusTypeDef status = HAL_CRYP_Encrypt(
		hal_cryp,
		(uint32_t *) data,
		data_length,
		(uint32_t *) data,
		HAL_MAX_DELAY
	);
	if (status != HAL_OK) {
		error_log(
			red("HAL_CRYP_Encrypt error: status = %d, ErrorCode = %" PRIx32) nl,
			status, hal_cryp->ErrorCode
		);
		return CTAP_CRYPTO_ERROR;
	}
	return CTAP_CRYPTO_OK;
}

ctap_crypto_status_t stm32h533_crypto_aes_256_cbc_decrypt(
	const ctap_crypto_t *const crypto,
	const uint8_t *iv,
	const uint8_t *key,
	uint8_t *data,
	const size_t data_length
) {
	if (data_length == 0) {
		return CTAP_CRYPTO_OK;
	}
	if (data_length % CTAP_CRYPTO_AES_BLOCK_SIZE) {
		return CTAP_CRYPTO_ERROR;
	}
	stm32h533_crypto_context_t *const ctx = crypto->context;
	CRYP_HandleTypeDef *const hal_cryp = &ctx->hal_cryp;
	CRYP_AES_256_SetIV_Swap(hal_cryp, (const uint32_t *) iv);
	HAL_StatusTypeDef status = CRYP_AES_256_CBC_SetDecryptKey(
		hal_cryp, (const uint32_t *) key, HAL_MAX_DELAY
	);
	if (status != HAL_OK) {
		error_log(
			red("CRYP_AES_256_CBC_SetDecryptKey error: status = %d, ErrorCode = %" PRIx32) nl,
			status, hal_cryp->ErrorCode
		);
		return CTAP_CRYPTO_ERROR;
	}
	status = HAL_CRYP_Decrypt(
		hal_cryp,
		(uint32_t *) data,
		data_length,
		(uint32_t *) data,
		HAL_MAX_DELAY
	);
	if (status != HAL_OK) {
		error_log(
			red("HAL_CRYP_Decrypt error: status = %d, ErrorCode = %" PRIx32) nl,
			status, hal_cryp->ErrorCode
		);
		return CTAP_CRYPTO_ERROR;
	}
	return CTAP_CRYPTO_OK;
}

// ####################  HASH  ####################

void HAL_HASH_MspInit(HASH_HandleTypeDef *hhash) {
	lion_unused(hhash);
	__HAL_RCC_HASH_CLK_ENABLE();
}

void HAL_HASH_MspDeInit(HASH_HandleTypeDef *hhash) {
	lion_unused(hhash);
	__HAL_RCC_HASH_CLK_DISABLE();
}

static ctap_crypto_status_t stm32h533_crypto_init_hash_peripheral(stm32h533_crypto_context_t *ctx) {
	HASH_HandleTypeDef *const hal_hash = &ctx->hal_hash;
	hal_hash->Instance = HASH;
	hal_hash->Init.DataType = HASH_BYTE_SWAP;
	hal_hash->Init.Algorithm = HASH_ALGOSELECTION_SHA256;
	if (HAL_HASH_Init(hal_hash) != HAL_OK) {
		return CTAP_CRYPTO_ERROR;
	}
	return CTAP_CRYPTO_OK;
}

typedef struct stm32h533_crypto_sha256_ctx {
	HASH_HandleTypeDef *hal_hash;
	// HAL_HASH_Accumulate() requires all parts to be of a length that is a multiple of 4
	size_t num_extra_bytes;
	uint8_t extra_bytes[4];
} stm32h533_crypto_sha256_ctx_t;

ctap_crypto_status_t stm32h533_crypto_sha256_bind_ctx(
	const ctap_crypto_t *crypto,
	void *sha256_ctx
) {
	stm32h533_crypto_sha256_ctx_t *ctx = sha256_ctx;
	ctx->hal_hash = &((stm32h533_crypto_context_t *) crypto->context)->hal_hash;
	ctx->num_extra_bytes = 0;
	return CTAP_CRYPTO_OK;
}

void stm32h533_crypto_sha256_init(
	void *ctx
) {
	lion_unused(ctx);
}

void stm32h533_crypto_sha256_update(
	void *sha256_ctx,
	const uint8_t *data, size_t data_length
) {
	if (data_length == 0) {
		return;
	}

	stm32h533_crypto_sha256_ctx_t *ctx = sha256_ctx;
	HASH_HandleTypeDef *const hal_hash = ctx->hal_hash;

	if (ctx->num_extra_bytes > 0) {
		const size_t needed_to_4 = 4 - ctx->num_extra_bytes;
		if (needed_to_4 > 0) {
			const size_t use_from_data = min(needed_to_4, data_length);
			memcpy(&ctx->extra_bytes[ctx->num_extra_bytes], data, use_from_data);
			ctx->num_extra_bytes += use_from_data;
			data_length -= use_from_data;
			data += use_from_data;
		}
		if (ctx->num_extra_bytes != 4) {
			assert(data_length == 0);
			return;
		}
		HAL_StatusTypeDef status = HAL_HASH_Accumulate(
			hal_hash,
			ctx->extra_bytes, 4,
			HAL_MAX_DELAY
		);
		if (status != HAL_OK) {
			error_log(
				red("HAL_HASH_Accumulate error: status = %d, ErrorCode = %" PRIx32) nl,
				status, hal_hash->ErrorCode
			);
			Error_Handler();
		}
		ctx->num_extra_bytes = 0;
	}

	size_t num_extra_bytes = data_length % 4;
	if (num_extra_bytes > 0) {
		assert(num_extra_bytes <= 3);
		assert(ctx->num_extra_bytes == 0);
		memcpy(ctx->extra_bytes, &data[data_length - num_extra_bytes], num_extra_bytes);
		ctx->num_extra_bytes = num_extra_bytes;
	}

	HAL_StatusTypeDef status = HAL_HASH_Accumulate(
		hal_hash,
		data, data_length - num_extra_bytes,
		HAL_MAX_DELAY
	);
	if (status != HAL_OK) {
		error_log(
			red("HAL_HASH_Accumulate error: status = %d, ErrorCode = %" PRIx32) nl,
			status, hal_hash->ErrorCode
		);
		Error_Handler();
	}
}

void stm32h533_crypto_sha256_final(
	void *sha256_ctx,
	uint8_t *hash
) {
	stm32h533_crypto_sha256_ctx_t *ctx = sha256_ctx;
	HASH_HandleTypeDef *const hal_hash = ctx->hal_hash;
	HAL_StatusTypeDef status = HAL_HASH_AccumulateLast(
		hal_hash,
		ctx->extra_bytes, ctx->num_extra_bytes,
		hash,
		HAL_MAX_DELAY
	);
	if (status != HAL_OK) {
		error_log(
			red("HAL_HASH_AccumulateLast error: status = %d, ErrorCode = %" PRIx32) nl,
			status, hal_hash->ErrorCode
		);
		Error_Handler();
	}
	ctx->num_extra_bytes = 0;
}

ctap_crypto_status_t stm32h533_crypto_sha256_compute_digest(
	const ctap_crypto_t *crypto,
	const uint8_t *data, size_t data_length,
	uint8_t *hash
) {
	stm32h533_crypto_context_t *const ctx = crypto->context;
	HASH_HandleTypeDef *const hal_hash = &ctx->hal_hash;
	HAL_StatusTypeDef status = HAL_HASH_Start(
		hal_hash,
		data, data_length,
		hash,
		HAL_MAX_DELAY
	);
	if (status != HAL_OK) {
		error_log(
			red("HAL_HASH_Start error: status = %d, ErrorCode = %" PRIx32) nl,
			status, hal_hash->ErrorCode
		);
		return CTAP_CRYPTO_ERROR;
	}
	return CTAP_CRYPTO_OK;
}

const hash_alg_t hash_alg_hw_sha256 = {
	.ctx_size = sizeof(stm32h533_crypto_sha256_ctx_t),
	.output_size = 32,
	.block_size = 64,
	.init = stm32h533_crypto_sha256_init,
	.update = stm32h533_crypto_sha256_update,
	.final = stm32h533_crypto_sha256_final,
};
