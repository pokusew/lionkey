#include "ctap.h"

#include <hmac.h>

static inline bool is_option_present(const CTAP_mc_ga_options *options, const uint8_t option) {
	return (options->present & option) == option;
}

static inline bool get_option_value(const CTAP_mc_ga_options *options, const uint8_t option) {
	return (options->values & option) == option;
}

static inline bool get_option_value_or_false(const CTAP_mc_ga_options *options, const uint8_t option) {
	return is_option_present(options, option) ? get_option_value(options, option) : false;
}

static inline bool get_option_value_or_true(const CTAP_mc_ga_options *options, const uint8_t option) {
	return is_option_present(options, option) ? get_option_value(options, option) : true;
}

static inline uint8_t verify_client_data_hash(
	ctap_state_t *state,
	const CTAP_mc_ga_common *params,
	ctap_pin_protocol_t *pin_protocol
) {
	uint8_t verify_ctx[pin_protocol->verify_get_context_size(pin_protocol)];
	if (pin_protocol->verify_init_with_pin_uv_auth_token(
		pin_protocol,
		&verify_ctx,
		&state->pin_uv_auth_token_state
	) != 0) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	pin_protocol->verify_update(
		pin_protocol,
		&verify_ctx,
		/* message */ params->clientDataHash.data, params->clientDataHash.size
	);
	if (pin_protocol->verify_final(
		pin_protocol,
		&verify_ctx,
		/* signature */ params->pinUvAuthParam.data, params->pinUvAuthParam.size
	) != 0) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	return CTAP2_OK;
}

void ctap_compute_rp_id_hash(const ctap_crypto_t *const crypto, uint8_t *rp_id_hash, const CTAP_rpId *rp_id) {
	crypto->sha256_compute_digest(crypto, rp_id->id.data, rp_id->id.size, rp_id_hash);
}

static uint8_t create_attested_credential_data(
	const ctap_crypto_t *crypto,
	CTAP_authenticator_data_attestedCredentialData *attested_credential_data,
	size_t *attested_credential_data_size,
	const ctap_credential_handle_t *credential
) {

	static_assert(
		sizeof(attested_credential_data->fixed_header.aaguid) == sizeof(ctap_aaguid),
		"aaguid size mismatch"
	);

	memcpy(attested_credential_data->fixed_header.aaguid, ctap_aaguid, sizeof(ctap_aaguid));

	ctap_string_t credential_id;
	ctap_credential_get_id(credential, &credential_id);
	assert(credential_id.size <= 1023);
	attested_credential_data->fixed_header.credentialIdLength = lion_htons(credential_id.size);

	memcpy(attested_credential_data->variable_data, credential_id.data, credential_id.size);

	CborEncoder encoder;
	uint8_t ret;

	uint8_t *credentialPublicKey = &attested_credential_data->variable_data[credential_id.size];
	const size_t credentialPublicKey_buffer_size =
		sizeof(attested_credential_data->variable_data) - credential_id.size;

	cbor_encoder_init(&encoder, credentialPublicKey, credentialPublicKey_buffer_size, 0);

	uint8_t public_key[64];
	ctap_check(ctap_credential_compute_public_key(crypto, credential, public_key));
	ctap_check(ctap_encode_public_key(&encoder, public_key));

	*attested_credential_data_size =
		sizeof(attested_credential_data->fixed_header)
		+ credential_id.size
		+ cbor_encoder_get_buffer_size(&encoder, credentialPublicKey);

	assert(*attested_credential_data_size <= sizeof(CTAP_authenticator_data_attestedCredentialData));

	return CTAP2_OK;

}

static uint8_t encode_make_credential_authenticator_data_extensions(
	uint8_t *buffer,
	const size_t buffer_size,
	size_t *extensions_size,
	const uint8_t extensions_present,
	const ctap_credential_handle_t *credential
) {

	CborEncoder encoder;
	cbor_encoder_init(&encoder, buffer, buffer_size, 0);

	CborError err;
	CborEncoder map;

	size_t num_extensions_in_output_map = 0;
	if ((extensions_present & CTAP_extension_credProtect) != 0u) {
		num_extensions_in_output_map++;
	}
	if ((extensions_present & CTAP_extension_hmac_secret) != 0u) {
		num_extensions_in_output_map++;
	}

	cbor_encoding_check(cbor_encoder_create_map(&encoder, &map, num_extensions_in_output_map));
	if ((extensions_present & CTAP_extension_credProtect) != 0u) {
		cbor_encoding_check(cbor_encode_text_string(&map, "credProtect", 11));
		const uint8_t cred_protect = ctap_credential_get_cred_protect(credential);
		cbor_encoding_check(cbor_encode_uint(&map, cred_protect));
	}
	if ((extensions_present & CTAP_extension_hmac_secret) != 0u) {
		cbor_encoding_check(cbor_encode_text_string(&map, "hmac-secret", 11));
		cbor_encoding_check(cbor_encode_boolean(&map, true));
	}
	cbor_encoding_check(cbor_encoder_close_container(&encoder, &map));

	*extensions_size = cbor_encoder_get_buffer_size(&encoder, buffer);

	return CTAP2_OK;

}

static uint8_t compute_signature(
	const ctap_crypto_t *const crypto,
	const CTAP_authenticator_data *auth_data,
	const size_t auth_data_size,
	const uint8_t *client_data_hash,
	const ctap_credential_handle_t *credential,
	uint8_t asn1_der_sig[72],
	size_t *asn1_der_sig_size
) {

	uint8_t ret;

	// message_hash = SHA256(authenticatorData || clientDataHash)
	uint8_t message_hash[CTAP_SHA256_HASH_SIZE];

	const hash_alg_t *const sha256 = crypto->sha256;
	uint8_t sha256_ctx[sha256->ctx_size];
	crypto->sha256_bind_ctx(crypto, sha256_ctx);

	sha256->init(sha256_ctx);
	sha256->update(sha256_ctx, (const uint8_t *) auth_data, auth_data_size);
	sha256->update(sha256_ctx, client_data_hash, 32);
	sha256->final(sha256_ctx, message_hash);

	uint8_t signature[64];
	ctap_check(ctap_credential_compute_signature(
		crypto,
		credential,
		message_hash,
		sizeof(message_hash),
		signature
	));

	// WebAuthn 6.5.5. Signature Formats for Packed Attestation, FIDO U2F Attestation, and Assertion Signatures
	// https://w3c.github.io/webauthn/#sctn-signature-attestation-types
	// For COSEAlgorithmIdentifier -7 (ES256), and other ECDSA-based algorithms,
	// the sig value MUST be encoded as an ASN.1 DER Ecdsa-Sig-Value, as defined in [RFC3279] section 2.2.3.
	ctap_convert_to_asn1_der_ecdsa_sig_value(
		signature,
		asn1_der_sig,
		asn1_der_sig_size
	);

	return CTAP2_OK;

}

static uint8_t create_self_attestation_statement(
	const ctap_crypto_t *const crypto,
	CborEncoder *encoder,
	const CTAP_authenticator_data *auth_data,
	const size_t auth_data_size,
	const uint8_t *client_data_hash,
	const ctap_credential_handle_t *credential
) {

	uint8_t ret;

	uint8_t asn1_der_sig[72];
	size_t asn1_der_sig_size;
	ctap_check(compute_signature(
		crypto,
		auth_data,
		auth_data_size,
		client_data_hash,
		credential,
		asn1_der_sig,
		&asn1_der_sig_size
	));

	CborError err;
	CborEncoder map;

	cbor_encoding_check(cbor_encoder_create_map(encoder, &map, 2));
	// alg: COSEAlgorithmIdentifier
	cbor_encoding_check(cbor_encode_text_string(&map, "alg", 3));
	cbor_encoding_check(cbor_encode_int(&map, COSE_ALG_ES256));
	// sig: bytes
	cbor_encoding_check(cbor_encode_text_string(&map, "sig", 3));
	cbor_encoding_check(cbor_encode_byte_string(
		&map,
		asn1_der_sig,
		asn1_der_sig_size
	));
	// close response map
	cbor_encoding_check(cbor_encoder_close_container(encoder, &map));

	return CTAP2_OK;

}

/**
 * This functions implements Step 1 (handling of the legacy CTAP2.0 selection behavior)
 * and Step 2 (validating pinUvAuthProtocol and getting the pointer to the corresponding ctap_pin_protocol_t).
 *
 * These steps are common to both 6.1.2. authenticatorMakeCredential Algorithm
 * and 6.2.2. authenticatorGetAssertion Algorithm.
 *
 * @return CTAP2_OK if the caller function should continue with the request processing,
 *         otherwise the caller function should stop processing the request and return the error code
 */
static uint8_t handle_pin_uv_auth_param_and_protocol(
	ctap_state_t *state,
	const bool pinUvAuthParam_present,
	const size_t pinUvAuthParam_size,
	const bool pinUvAuthProtocol_present,
	const uint8_t pinUvAuthProtocol,
	ctap_pin_protocol_t **pin_protocol
) {
	uint8_t ret;

	// 1. If authenticator supports either pinUvAuthToken or clientPin features
	//    and the platform sends a zero length pinUvAuthParam:
	//    Note:
	//      This is done for backwards compatibility with CTAP2.0 platforms in the case
	//      where multiple authenticators are attached to the platform and the platform
	//      wants to enforce pinUvAuthToken feature semantics, but the user has to select
	//      which authenticator to get the pinUvAuthToken from.
	//      CTAP2.1 platforms SHOULD use 6.9 authenticatorSelection (0x0B).
	if (pinUvAuthParam_present && pinUvAuthParam_size == 0) {
		// 1. Request evidence of user interaction in an authenticator-specific way (e.g., flash the LED light).
		ctap_user_presence_result_t up_result = ctap_wait_for_user_presence();
		switch (up_result) {
			case CTAP_UP_RESULT_CANCEL:
				// handling of 11.2.9.1.5. CTAPHID_CANCEL (0x11)
				return CTAP2_ERR_KEEPALIVE_CANCEL;
			case CTAP_UP_RESULT_TIMEOUT:
			case CTAP_UP_RESULT_DENY:
				// 2. If the user declines permission, or the operation times out,
				//    then end the operation by returning CTAP2_ERR_OPERATION_DENIED.
				return CTAP2_ERR_OPERATION_DENIED;
			case CTAP_UP_RESULT_ALLOW:
				// 3. If evidence of user interaction is provided in this step then return either CTAP2_ERR_PIN_NOT_SET
				//    if PIN is not set or CTAP2_ERR_PIN_INVALID if PIN has been set.
				return !ctap_is_pin_set(state) ? CTAP2_ERR_PIN_NOT_SET : CTAP2_ERR_PIN_INVALID;
		}
	}

	// 2. If the pinUvAuthParam parameter is present:
	if (pinUvAuthParam_present) {
		// 2. If the pinUvAuthProtocol parameter is absent,
		//    return CTAP2_ERR_MISSING_PARAMETER error.
		if (!pinUvAuthProtocol_present) {
			return CTAP2_ERR_MISSING_PARAMETER;
		}
		// 1. If the pinUvAuthProtocol parameter's value is not supported,
		//    return CTAP1_ERR_INVALID_PARAMETER error.
		ctap_check(ctap_get_pin_protocol(state, pinUvAuthProtocol, pin_protocol));
	}

	// the caller function should continue with the request processing
	return CTAP2_OK;
}

/**
 * Ensures that the effective value of the "uv" option is false.
 *
 * Currently, LionKey does not support a built-in user verification method,
 * therefore the only supported "uv" option value is false.
 * Note that "clientPin" is NOT a "built-in user verification method",
 * it is only considered to be "some form of user verification".
 *
 * @param pinUvAuthParam_present If the pinUvAuthParam is present, the "uv" option is ignored.
 * @param options
 *
 * @return CTAP2_OK if the effective value of the "uv" option is false
 */
static uint8_t ensure_uv_option_false(const bool pinUvAuthParam_present, CTAP_mc_ga_options *options) {
	// user verification:
	//   Note:
	//     Use of this "uv" option key is deprecated in CTAP2.1.
	//     Instead, platforms SHOULD create a pinUvAuthParam by obtaining pinUvAuthToken
	//     via getPinUvAuthTokenUsingUvWithPermissions or getPinUvAuthTokenUsingPinWithPermissions,
	//     as appropriate.
	//   Note:
	//     pinUvAuthParam and the "uv" option are processed as mutually exclusive
	//     with pinUvAuthParam taking precedence.
	const bool uv = pinUvAuthParam_present ? false : get_option_value_or_false(options, CTAP_ma_ga_option_uv);
	if (uv) {
		// 3. If the "uv" option is true then:
		//    1. If the authenticator does not support a built-in user verification method
		//       (as is the case with the current version of LionKey),
		//       end the operation by returning CTAP2_ERR_INVALID_OPTION.
		//       Note: One would expect the CTAP2_ERR_UNSUPPORTED_OPTION error code,
		//             but the spec really says CTAP2_ERR_INVALID_OPTION.
		debug_log(red("unsupported uv option value true") nl);
		return CTAP2_ERR_INVALID_OPTION;
	}
	return CTAP2_OK;
}

static uint8_t ensure_valid_pin_uv_auth_param(
	ctap_state_t *state,
	CTAP_mc_ga_common *params,
	ctap_pin_protocol_t *pin_protocol,
	uint32_t permissions
) {
	uint8_t ret;
	// 1. Call verify(key=pinUvAuthToken, message=clientDataHash, signature: pinUvAuthParam).
	//    1. If the verification returns error,
	//       then end the operation by returning CTAP2_ERR_PIN_AUTH_INVALID error.
	ctap_check(verify_client_data_hash(state, params, pin_protocol));
	// 2. Verify that the pinUvAuthToken has the required permissions,
	//    if not, then end the operation by returning CTAP2_ERR_PIN_AUTH_INVALID.
	if (!ctap_pin_uv_auth_token_has_permissions(state, permissions)) {
		debug_log(
			red("pinUvAuthToken does not have the required permissions:"
				" current=%" PRIu32 " required=%" PRIu32) nl,
			state->pin_uv_auth_token_state.permissions,
			permissions
		);
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	// 3. If the pinUvAuthToken has a permissions RP ID associated:
	//    1. If the permissions RP ID does NOT match the rp.id in this request,
	//       then end the operation by returning CTAP2_ERR_PIN_AUTH_INVALID.
	if ((
		state->pin_uv_auth_token_state.rpId_set
		&& memcmp(state->pin_uv_auth_token_state.rpId_hash, params->rpId.hash, CTAP_SHA256_HASH_SIZE) != 0
	)) {
		debug_log(
			red("pinUvAuthToken RP ID mismatch: required='%.*s' hash = "),
			(int) params->rpId.id.size, params->rpId.id.data
		);
		dump_hex(params->rpId.hash, sizeof(params->rpId.hash));
		debug_log("pinUvAuthToken associated RP ID hash = ");
		dump_hex(state->pin_uv_auth_token_state.rpId_hash, sizeof(state->pin_uv_auth_token_state.rpId_hash));
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	// Let userVerifiedFlagValue be the result of calling getUserVerifiedFlagValue().
	// If userVerifiedFlagValue is false then end the operation by returning CTAP2_ERR_PIN_AUTH_INVALID.
	if (!ctap_pin_uv_auth_token_get_user_verified_flag_value(state)) {
		debug_log(red("pinUvAuthToken user_verified=false") nl);
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}
	// If the pinUvAuthToken does not have a permissions RP ID associated:
	// Associate the request's rp.id parameter value with the pinUvAuthToken as its permissions RP ID.
	if (!state->pin_uv_auth_token_state.rpId_set) {
		ctap_compute_rp_id_hash(state->crypto, state->pin_uv_auth_token_state.rpId_hash, &params->rpId);
		state->pin_uv_auth_token_state.rpId_set = true;
		debug_log(
			"pinUvAuthToken did not have RP ID associated, setting to '%.*s' hash = ",
			(int) params->rpId.id.size, params->rpId.id.data
		);
		dump_hex(state->pin_uv_auth_token_state.rpId_hash, sizeof(state->pin_uv_auth_token_state.rpId_hash));
	}
	return CTAP2_OK;
}

static uint8_t ensure_user_present(ctap_state_t *state, const bool pinUvAuthParam_present) {
	const bool user_present = pinUvAuthParam_present && ctap_pin_uv_auth_token_get_user_present_flag_value(state);
	if (!user_present) {
		// 1. Request evidence of user interaction in an authenticator-specific way (e.g., flash the LED light).
		//    If the authenticator has a display, show the items contained within the user and rp parameter
		//    structures to the user, and request permission to create a credential.
		ctap_user_presence_result_t up_result = ctap_wait_for_user_presence();
		switch (up_result) {
			case CTAP_UP_RESULT_CANCEL:
				// handling of 11.2.9.1.5. CTAPHID_CANCEL (0x11)
				return CTAP2_ERR_KEEPALIVE_CANCEL;
			case CTAP_UP_RESULT_TIMEOUT:
			case CTAP_UP_RESULT_DENY:
				// 2. If the user declines permission, or the operation times out,
				//    then end the operation by returning CTAP2_ERR_OPERATION_DENIED.
				return CTAP2_ERR_OPERATION_DENIED;
			case CTAP_UP_RESULT_ALLOW:
				// continue
				ctap_send_keepalive_if_needed(CTAP_STATUS_PROCESSING);
				break;
		}
	}
	return CTAP2_OK;
}

static uint8_t process_exclude_list(
	ctap_state_t *state,
	const CborValue *excludeList,
	const CTAP_rpId *rpId,
	const bool pinUvAuthParam_present,
	const bool uv_collected
) {
	uint8_t ret;

	ctap_parse_pub_key_cred_desc_list_ctx list_ctx;
	ctap_check(ctap_parse_pub_key_cred_desc_list_init(&list_ctx, excludeList));

	CTAP_credDesc *cred_desc;
	while (true) {
		ctap_check(ctap_parse_pub_key_cred_desc_list_next_cred(&list_ctx, &cred_desc));
		// if the end of the list, stop iteration
		if (cred_desc == NULL) {
			break;
		}
		ctap_credential_handle_t credential;
		if (!ctap_lookup_credential_by_desc(state->storage, cred_desc, &credential)) {
			debug_log("process_exclude_list: skipping unknown credential ID" nl);
			continue;
		}
		if (!ctap_credential_matches_rp(&credential, rpId)) {
			debug_log("process_exclude_list: skipping credential ID that is bound to a different RP" nl);
			continue;
		}
		const uint8_t cred_protect = ctap_credential_get_cred_protect(&credential);
		if (cred_protect == CTAP_extension_credProtect_3_userVerificationRequired && !uv_collected) {
			debug_log(
				"process_exclude_list:"
				" skipping a credential with credProtect_3_userVerificationRequired"
				" because uv not collected" nl
			);
			continue;
		}
		// TODO:
		//   What if is the pinUvAuthParam is invalid here?
		//   Note that it is validated only if !allow_no_verification && ctap_is_pin_set(state) (see Step 11).
		const bool user_present = pinUvAuthParam_present && ctap_pin_uv_auth_token_get_user_present_flag_value(state);
		if (!user_present) {
			debug_log("process_exclude_list: collecting user presence ..." nl);
			ctap_user_presence_result_t up_result = ctap_wait_for_user_presence();
			if (up_result == CTAP_UP_RESULT_CANCEL) {
				// handling of 11.2.9.1.5. CTAPHID_CANCEL (0x11)
				return CTAP2_ERR_KEEPALIVE_CANCEL;
			}
		}
		return CTAP2_ERR_CREDENTIAL_EXCLUDED;
	}

	return CTAP2_OK;
}

static uint8_t process_allow_list(
	ctap_state_t *state,
	const CborValue *allowList,
	const CTAP_rpId *rpId,
	const bool response_has_uv,
	ctap_credential_handle_t *credentials,
	size_t *const num_credentials,
	const size_t max_num_credentials
) {

	debug_log("process_allow_list" nl);

	uint8_t ret;
	size_t credentials_num = 0;

	ctap_parse_pub_key_cred_desc_list_ctx list_ctx;
	ctap_check(ctap_parse_pub_key_cred_desc_list_init(&list_ctx, allowList));

	CTAP_credDesc *cred_desc;
	// iterate over ALL credential descriptors to parse and validate them,
	// find the first applicable credential
	while (true) {

		// parse and validate each credential descriptors
		ctap_check(ctap_parse_pub_key_cred_desc_list_next_cred(&list_ctx, &cred_desc));

		// if the end of the list, stop iteration
		if (cred_desc == NULL) {
			break;
		}

		ctap_credential_handle_t credential;
		if (!ctap_lookup_credential_by_desc(state->storage, cred_desc, &credential)) {
			debug_log("process_allow_list: skipping unknown credential ID" nl);
			continue;
		}

		if (!ctap_credential_matches_rp(&credential, rpId)) {
			debug_log("process_allow_list: skipping credential ID that is bound to a different RP" nl);
			continue;
		}

		if (!ctap_should_add_credential_to_list(&credential, true, response_has_uv)) {
			debug_log("process_allow_list: skipping credential due to its credProtect" nl);
			continue;
		}

		if (credentials_num == max_num_credentials) {
			error_log(
				red("process_allow_list: credentials_max_size (%" PRIsz ") reached") nl,
				max_num_credentials
			);
			return CTAP1_ERR_INVALID_LENGTH;
		}
		credentials[credentials_num++] = credential;

	}

	*num_credentials = credentials_num;

	return CTAP2_OK;

}

uint8_t ctap_make_credential(ctap_state_t *const state, CborValue *const it, CborEncoder *const encoder) {

	uint8_t ret;
	CborError err;

	CTAP_makeCredential mc;
	CTAP_mc_ga_common *const params = &mc.common;
	ctap_check(ctap_parse_make_credential(it, &mc));

	const bool pinUvAuthParam_present = ctap_param_is_present(params, CTAP_makeCredential_pinUvAuthParam);
	ctap_pin_protocol_t *pin_protocol = NULL;

	const ctap_crypto_t *const crypto = state->crypto;

	// 6.1.2. authenticatorMakeCredential Algorithm
	// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#sctn-makeCred-authnr-alg
	// see also WebAuthn 6.3.2. The authenticatorMakeCredential Operation
	// https://w3c.github.io/webauthn/#sctn-op-make-cred

	// rpId_hash is needed throughout the whole algorithm, so we compute it right away.
	ctap_compute_rp_id_hash(crypto, params->rpId.hash, &params->rpId);

	// 1. + 2.
	ctap_check(handle_pin_uv_auth_param_and_protocol(
		state,
		pinUvAuthParam_present,
		params->pinUvAuthParam.size,
		ctap_param_is_present(params, CTAP_makeCredential_pinUvAuthProtocol),
		params->pinUvAuthProtocol,
		&pin_protocol
	));

	// 3. Validate pubKeyCredParams and choose the first supported algorithm.
	ctap_check(ctap_parse_make_credential_pub_key_cred_params(&mc));
	debug_log("chosen algorithm = %" PRId32 nl, mc.pubKeyCredParams_chosen.alg);

	// 4. Create a new authenticatorMakeCredential response structure
	//    and initialize both its "uv" bit and "up" bit as false.
	CTAP_authenticator_data auth_data;
	memset(&auth_data, 0, sizeof(auth_data));
	// thanks to the memset above, alls flags are initialized to 0 (false)

	// 5. If the options parameter is present, process all option keys and values present in the parameter.
	//    Treat any option keys that are not understood as absent.
	const bool discoverable = get_option_value_or_false(&params->options, CTAP_ma_ga_option_rk);
	debug_log("discoverable = %d" nl, discoverable);
	if (ctap_param_is_present(params, CTAP_makeCredential_options)) {
		ctap_check(ensure_uv_option_false(pinUvAuthParam_present, &params->options));
		// user presence (defaults to true):
		//   Instructs the authenticator to require user consent to complete the operation.
		//   Platforms MAY send the "up" option key to CTAP2.1 authenticators,
		//   and its value MUST be true if present.
		//   The value false will cause a CTAP2_ERR_INVALID_OPTION response regardless of authenticator version.
		if ((
			is_option_present(&params->options, CTAP_ma_ga_option_up)
			&& get_option_value(&params->options, CTAP_ma_ga_option_up) == false
		)) {
			return CTAP2_ERR_INVALID_OPTION;
		}
	}

	// 6. (not applicable to LionKey) If the alwaysUv option ID is present and true then ...

	// 7. If the makeCredUvNotRqd option ID is present and set to true in the authenticatorGetInfo response:
	//    Note:
	//      This step returns an error if the platform tries to create a discoverable credential
	//      without performing some form of user verification.
	if (ctap_is_pin_set(state) && !pinUvAuthParam_present && discoverable) {
		return CTAP2_ERR_PUAT_REQUIRED;
	}

	// 8. (not applicable to LionKey) Else: (the makeCredUvNotRqd option ID in authenticatorGetInfo's response
	//    is present with the value false or is absent): ...

	// 9. If the enterpriseAttestation parameter is present:
	if (ctap_param_is_present(params, CTAP_makeCredential_enterpriseAttestation)) {
		// 1. If the authenticator is not enterprise attestation capable,
		//    or the authenticator is enterprise attestation capable but enterprise attestation is disabled,
		//    then end the operation by returning CTAP1_ERR_INVALID_PARAMETER.
		return CTAP1_ERR_INVALID_PARAMETER;
	}

	// 10. Allow the authenticator to create a non-discoverable credential
	//     without requiring some form of user verification under the below specific criteria.
	const bool allow_no_verification = !discoverable && !pinUvAuthParam_present;
	debug_log("allow_no_verification=%d" nl, allow_no_verification);
	// 11. If the authenticator is protected by some form of user verification , then:
	if (!allow_no_verification && ctap_is_pin_set(state)) {
		// 1. If pinUvAuthParam parameter is present (implying the "uv" option is false (see Step 5)):
		if (pinUvAuthParam_present) {
			assert(pin_protocol != NULL); // <- this should be ensured by Step 2
			ctap_check(ensure_valid_pin_uv_auth_param(
				state,
				params,
				pin_protocol,
				CTAP_clientPIN_pinUvAuthToken_permission_mc
			));
			auth_data.fixed_header.flags |= CTAP_authenticator_data_flags_uv;
		}
		// 2. If the "uv" option is present and set to true (implying the pinUvAuthParam parameter is not present,
		//    and that the authenticator supports an enabled built-in user verification method, see Step 5):
		//    (not applicable to LionKey)
	}

	// 12. If the excludeList parameter is present ...
	if (ctap_param_is_present(params, CTAP_makeCredential_excludeList)) {
		ctap_check(process_exclude_list(
			state,
			&mc.excludeList,
			&params->rpId,
			pinUvAuthParam_present,
			(auth_data.fixed_header.flags & CTAP_authenticator_data_flags_uv) != 0u
		));
	}

	// 13. (not applicable to LionKey) If evidence of user interaction was provided as part of Step 11
	//     (i.e., by invoking performBuiltInUv()): ...

	// 14. If the "up" option is set to true:
	//     Note: Step 3 ensures that the "up" option is effectively always true.
	// 14.1. and 14.2. together (since we do not perform Step 13, the "up" bit in the response
	// can never be true at this point and thus the 14.2.1. check is unnecessary).
	ctap_check(ensure_user_present(state, pinUvAuthParam_present));
	// 14.3. Set the "up" bit to true in the response.
	auth_data.fixed_header.flags |= CTAP_authenticator_data_flags_up;
	// 14.4. Call clearUserPresentFlag(), clearUserVerifiedFlag(), and clearPinUvAuthTokenPermissionsExceptLbw().
	//       Note: This consumes both the "user present state", sometimes referred to as the "cached UP",
	//       and the "user verified state", sometimes referred to as "cached UV".
	//       These functions are no-ops if there is not an in-use pinUvAuthToken.
	ctap_pin_uv_auth_token_clear_user_present_flag(state);
	ctap_pin_uv_auth_token_clear_user_verified_flag(state);
	ctap_pin_uv_auth_token_clear_permissions_except_lbw(state);

	// extensions processing

	// 12.1. Credential Protection (credProtect)
	// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#sctn-credProtect-extension
	//   credProtect value is persisted with the credential.
	//   If no credProtect extension was included in the request the authenticator
	//   SHOULD use the default value of 1 for compatibility with CTAP2.0 platforms.
	//   The authenticator MUST NOT return an unsolicited credProtect extension output.
	uint8_t credProtect = CTAP_extension_credProtect_1_userVerificationOptional;

	// 12.5. HMAC Secret Extension (hmac-secret)
	//   (handled by ctap_create_credential())
	//   authenticatorMakeCredential additional behaviors
	//     The authenticator generates two random 32-byte values (called CredRandomWithUV and CredRandomWithoutUV)
	//     and associates them with the credential.
	//     Note:
	//       Authenticator SHOULD generate CredRandomWithUV/CredRandomWithoutUV and associate
	//       them with the credential, even if hmac-secret extension is NOT present
	//       in authenticatorMakeCredential request.

	// 15. If the extensions parameter is present:
	if (ctap_param_is_present(params, CTAP_makeCredential_extensions)) {
		// 1. Process any extensions that this authenticator supports,
		//    ignoring any that it does not support.
		// 2. Authenticator extension outputs generated by the authenticator
		//    extension processing are returned in the authenticator data.
		//    The set of keys in the authenticator extension outputs map MUST
		//    be equal to, or a subset of, the keys of the authenticator extension inputs map.
		// Note: Some extensions may produce different output depending on the state of
		// the "uv" bit and/or "up" bit in the response.
		if ((params->extensions_present & CTAP_extension_credProtect) != 0u) {
			// Only set, if the given credProtect value is valid and non-default.
			// Note:
			//   The spec does not explicitly say what to do with invalid credProtect values.
			//   We decided to ignore any invalid values here since the actual used value will be returned
			//   in the response and the client/RP can decide, what to do.
			if ((
				mc.credProtect == CTAP_extension_credProtect_2_userVerificationOptionalWithCredentialIDList
				|| mc.credProtect == CTAP_extension_credProtect_3_userVerificationRequired
			)) {
				credProtect = mc.credProtect;
			}
		}
	}

	// 16. Generate a new credential key pair for the algorithm chosen in Step 3.
	// 17. If the "rk" option is set to true:
	//     1. The authenticator MUST create a discoverable credential.
	//     2. If a credential for the same rp.id and account ID already exists on the authenticator:
	//        1. If the existing credential contains a largeBlobKey, an authenticator MAY erase any
	//           associated large-blob data. Platforms MUST NOT assume that authenticators will do this.
	//           Platforms can later garbage collect any orphaned large-blobs.
	//        2. Overwrite that credential.
	//     3. Store the user parameter along with the newly-created key pair.
	//     4. If authenticator does not have enough internal storage to persist the new credential,
	//        return CTAP2_ERR_KEY_STORE_FULL.
	// 18. Otherwise, if the "rk" option is false: the authenticator MUST create a non-discoverable credential.
	//     Note: This step is a change from CTAP2.0 where if the "rk" option is false the authenticator
	//           could optionally create a discoverable credential.

	ctap_credential_handle_t credential;
	ctap_check(ctap_create_credential(
		state->storage,
		state->crypto,
		discoverable,
		credProtect,
		&params->rpId,
		&mc.user,
		&credential
	));

	// 19. Generate an attestation statement for the newly-created credential using clientDataHash,
	//     taking into account the value of the enterpriseAttestation parameter, if present,
	//     as described above in Step 9.
	memcpy(auth_data.fixed_header.rpIdHash, params->rpId.hash, CTAP_SHA256_HASH_SIZE);
	uint32_t signCount;
	ctap_check(ctap_increment_global_signature_counter(state->storage, state->crypto, &signCount));
	auth_data.fixed_header.signCount = lion_htonl(signCount);

	size_t auth_data_variable_size = 0;
	const size_t auth_data_variable_max_size = sizeof(auth_data.variable_data);

	CTAP_authenticator_data_attestedCredentialData *attested_credential_data =
		(CTAP_authenticator_data_attestedCredentialData *) &auth_data.variable_data[auth_data_variable_size];
	size_t attested_credential_data_size;
	ctap_check(create_attested_credential_data(
		state->crypto,
		attested_credential_data,
		&attested_credential_data_size,
		&credential
	));
	auth_data_variable_size += attested_credential_data_size;
	auth_data.fixed_header.flags |= CTAP_authenticator_data_flags_at;

	if (ctap_param_is_present(params, CTAP_makeCredential_extensions)) {
		size_t extensions_size;
		ctap_check(encode_make_credential_authenticator_data_extensions(
			&auth_data.variable_data[auth_data_variable_size],
			auth_data_variable_max_size - auth_data_variable_size,
			&extensions_size,
			params->extensions_present,
			&credential
		));
		auth_data_variable_size += extensions_size;
		auth_data.fixed_header.flags |= CTAP_authenticator_data_flags_ed;
	}

	assert(auth_data_variable_size < auth_data_variable_max_size);

	const size_t auth_data_total_size = sizeof(auth_data.fixed_header) + auth_data_variable_size;

	CborEncoder map;

	// start response map
	cbor_encoding_check(cbor_encoder_create_map(encoder, &map, 3));
	// fmt (0x01)
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_makeCredential_res_fmt));
	cbor_encoding_check(cbor_encode_text_string(&map, "packed", 6));
	// authData (0x02)
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_makeCredential_res_authData));
	cbor_encoding_check(cbor_encode_byte_string(
		&map,
		(const uint8_t *) &auth_data,
		auth_data_total_size
	));
	// attStmt (0x03)
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_makeCredential_res_attStmt));
	ctap_check(create_self_attestation_statement(
		crypto,
		&map,
		&auth_data,
		auth_data_total_size,
		params->clientDataHash.data,
		&credential
	));
	// close response map
	cbor_encoding_check(cbor_encoder_close_container(encoder, &map));

	return CTAP2_OK;

}

static uint8_t prepare_get_assertion_hmac_secret_extension(
	ctap_state_t *const state,
	ctap_get_assertion_state_t *const ga_state,
	const CTAP_getAssertion_hmac_secret *const hmac_secret,
	const uint32_t auth_data_flags
) {

	if ((auth_data_flags & CTAP_authenticator_data_flags_up) == 0) {
		return CTAP2_ERR_UNSUPPORTED_OPTION;
	}

	uint8_t ret;

	const uint8_t pinUvAuthProtocol =
		ctap_param_is_present(hmac_secret, CTAP_getAssertion_hmac_secret_pinUvAuthProtocol)
			? hmac_secret->pinUvAuthProtocol
			: 1;
	ctap_pin_protocol_t *pin_protocol;
	ctap_check(ctap_get_pin_protocol(state, pinUvAuthProtocol, &pin_protocol));

	assert(pin_protocol->shared_secret_length <= sizeof(ga_state->hmac_secret_state.shared_secret));
	if (pin_protocol->decapsulate(
		pin_protocol,
		&hmac_secret->keyAgreement,
		ga_state->hmac_secret_state.shared_secret
	) != 0) {
		return CTAP1_ERR_INVALID_PARAMETER;
	}

	uint8_t verify_ctx[pin_protocol->verify_get_context_size(pin_protocol)];
	pin_protocol->verify_init_with_shared_secret(pin_protocol, &verify_ctx, ga_state->hmac_secret_state.shared_secret);
	pin_protocol->verify_update(pin_protocol, &verify_ctx, hmac_secret->saltEnc.data, hmac_secret->saltEnc.size);
	if (pin_protocol->verify_final(
		pin_protocol, &verify_ctx, hmac_secret->saltAuth.data, hmac_secret->saltAuth.size
	) != 0) {
		return CTAP2_ERR_PIN_AUTH_INVALID;
	}

	if (hmac_secret->saltEnc.size < pin_protocol->encryption_extra_length) {
		return CTAP1_ERR_INVALID_PARAMETER;
	}
	const size_t salt_length = hmac_secret->saltEnc.size - pin_protocol->encryption_extra_length;
	if (salt_length != 32 && salt_length != 64) {
		return CTAP1_ERR_INVALID_PARAMETER;
	}
	static_assert(
		sizeof(ga_state->hmac_secret_state.salt) == 64,
		"sizeof(ga_state->hmac_secret_state.salt) == 64"
	);
	if (pin_protocol->decrypt(
		pin_protocol,
		ga_state->hmac_secret_state.shared_secret,
		hmac_secret->saltEnc.data, hmac_secret->saltEnc.size,
		ga_state->hmac_secret_state.salt
	) != 0) {
		return CTAP1_ERR_INVALID_PARAMETER;
	}

	// the stored hmac_secret_state will be used by generate_get_assertion_extensions_output()
	// to generate the output for each returned credential because multiple credentials
	// can be returned (authenticatorGetAssertion followed by authenticatorGetNextAssertion commands)
	ga_state->hmac_secret_state.salt_length = salt_length;
	ga_state->hmac_secret_state.pin_protocol = pin_protocol;
	ga_state->extensions = CTAP_extension_hmac_secret;

	return CTAP2_OK;

}

static uint8_t generate_get_assertion_hmac_secret_extension_output(
	const ctap_crypto_t *const crypto,
	const ctap_get_assertion_hmac_secret_state_t *const hmac_secret_state,
	const uint32_t auth_data_flags,
	const ctap_credential_handle_t *const credential,
	CborEncoder *const encoder
) {

	ctap_pin_protocol_t *const pin_protocol = hmac_secret_state->pin_protocol;
	assert(pin_protocol != NULL);

	const hash_alg_t *const sha256 = crypto->sha256;
	uint8_t sha256_ctx[crypto->sha256->ctx_size];
	crypto->sha256_bind_ctx(crypto, sha256_ctx);
	uint8_t hmac_ctx[hmac_get_context_size(sha256)];

	const uint8_t *const CredRandom = ctap_credential_get_cred_random(
		credential,
		(auth_data_flags & CTAP_authenticator_data_flags_uv) != 0
	);

	assert(hmac_secret_state->salt_length == 32 || hmac_secret_state->salt_length == 64);
	static_assert(
		sizeof(hmac_secret_state->salt) == 64,
		"sizeof(hmac_secret_state->salt) == 64"
	);
	uint8_t output[hmac_secret_state->salt_length];
	// output1: HMAC-SHA-256(CredRandom, salt1)
	hmac_init(hmac_ctx, sha256, sha256_ctx, CredRandom, 32);
	hmac_update(hmac_ctx, hmac_secret_state->salt, 32);
	hmac_final(hmac_ctx, output);
	if (hmac_secret_state->salt_length == 64) {
		// output2: HMAC-SHA-256(CredRandom, salt2)
		hmac_init(hmac_ctx, sha256, sha256_ctx, CredRandom, 32);
		hmac_update(hmac_ctx, hmac_secret_state->salt + 32, 32);
		hmac_final(hmac_ctx, output + 32);
	}

	uint8_t output_enc[hmac_secret_state->salt_length + pin_protocol->encryption_extra_length];
	if (pin_protocol->encrypt(
		pin_protocol,
		hmac_secret_state->shared_secret,
		output, hmac_secret_state->salt_length,
		output_enc
	) != 0) {
		return CTAP1_ERR_OTHER;
	}

	CborError err;
	cbor_encoding_check(cbor_encode_byte_string(encoder, output_enc, sizeof(output_enc)));

	return CTAP2_OK;

}

static uint8_t generate_get_assertion_extensions_output(
	const ctap_crypto_t *const crypto,
	const ctap_get_assertion_state_t *const ga_state,
	uint8_t *const buffer,
	const size_t buffer_size,
	size_t *const extensions_size
) {

	uint8_t ret;

	assert(ga_state->next_credential_idx < ga_state->num_credentials);

	const ctap_credential_handle_t *const credential = &ga_state->credentials[ga_state->next_credential_idx];

	const uint8_t extensions = ga_state->extensions;

	CborEncoder encoder;
	cbor_encoder_init(&encoder, buffer, buffer_size, 0);

	CborError err;
	CborEncoder map;

	size_t num_extensions_in_output_map = 0;
	if ((extensions & CTAP_extension_hmac_secret) != 0u) {
		num_extensions_in_output_map++;
	}

	cbor_encoding_check(cbor_encoder_create_map(&encoder, &map, num_extensions_in_output_map));
	if ((extensions & CTAP_extension_hmac_secret) != 0u) {
		cbor_encoding_check(cbor_encode_text_string(&map, "hmac-secret", 11));
		ctap_check(generate_get_assertion_hmac_secret_extension_output(
			crypto, &ga_state->hmac_secret_state, ga_state->auth_data_flags, credential, &map
		));
	}
	cbor_encoding_check(cbor_encoder_close_container(&encoder, &map));

	*extensions_size = cbor_encoder_get_buffer_size(&encoder, buffer);

	return CTAP2_OK;

}

static uint8_t generate_get_assertion_response(
	CborEncoder *encoder,
	const ctap_crypto_t *const crypto,
	const ctap_storage_t *const storage,
	const ctap_get_assertion_state_t *const ga_state
) {

	uint8_t ret;

	assert(ga_state->next_credential_idx < ga_state->num_credentials);

	const ctap_credential_handle_t *const credential = &ga_state->credentials[ga_state->next_credential_idx];

	CTAP_authenticator_data auth_data;

	// copy the RP ID hash to the authenticator data
	static_assert(
		sizeof(auth_data.fixed_header.rpIdHash) == sizeof(ga_state->auth_data_rp_id_hash),
		"sizeof(auth_data.fixed_header.rpIdHash) == sizeof(ga_state->auth_data_rp_id_hash)"
	);
	memcpy(auth_data.fixed_header.rpIdHash, ga_state->auth_data_rp_id_hash, sizeof(auth_data.fixed_header.rpIdHash));

	// copy the auth_data_flags to the authenticator data
	auth_data.fixed_header.flags = ga_state->auth_data_flags;

	// increment the credential's signature counter and copy the new value to the authenticator data
	uint32_t signCount;
	ctap_check(ctap_increment_global_signature_counter(storage, crypto, &signCount));
	auth_data.fixed_header.signCount = lion_htonl(signCount);

	size_t auth_data_variable_size = 0u;
	const size_t auth_data_variable_max_size = sizeof(auth_data.variable_data);

	if (ga_state->extensions != 0) {
		size_t extensions_size;
		ctap_check(generate_get_assertion_extensions_output(
			crypto,
			ga_state,
			&auth_data.variable_data[auth_data_variable_size],
			auth_data_variable_max_size - auth_data_variable_size,
			&extensions_size
		));
		auth_data_variable_size += extensions_size;
		auth_data.fixed_header.flags |= CTAP_authenticator_data_flags_ed;
	}

	assert(auth_data_variable_size < auth_data_variable_max_size);

	const size_t auth_data_total_size = sizeof(auth_data.fixed_header) + auth_data_variable_size;

	uint8_t asn1_der_sig[72];
	size_t asn1_der_sig_size;
	ctap_check(compute_signature(
		crypto,
		&auth_data,
		auth_data_total_size,
		ga_state->client_data_hash,
		credential,
		asn1_der_sig,
		&asn1_der_sig_size
	));

	CborError err;
	CborEncoder map;

	// numberOfCredentials (0x05) is only included in the authenticatorGetAssertion response
	// (it is omitted in the authenticatorGetNextAssertion response)
	const size_t num_credentials = ga_state->next_credential_idx == 0 ? ga_state->num_credentials : 0;
	// start response map
	cbor_encoding_check(cbor_encoder_create_map(
		encoder,
		&map,
		num_credentials > 0 ? 5 : 4
	));
	// credential (0x01)
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_getAssertion_res_credential));
	ctap_string_t credential_id;
	ctap_credential_get_id(credential, &credential_id);
	ctap_check(ctap_encode_pub_key_cred_desc(&map, &credential_id));
	// authData (0x02)
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_getAssertion_res_authData));
	cbor_encoding_check(cbor_encode_byte_string(
		&map,
		(const uint8_t *) &auth_data,
		auth_data_total_size
	));
	// signature (0x03)
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_getAssertion_res_signature));
	cbor_encoding_check(cbor_encode_byte_string(
		&map,
		asn1_der_sig,
		asn1_der_sig_size
	));
	// user (0x04)
	//   User identifiable information (name, DisplayName, icon) inside the publicKeyCredentialUserEntity
	//   MUST NOT be returned if user verification was not done by the authenticator
	//   in the original authenticatorGetAssertion call.
	bool include_user_identifiable_info = (ga_state->auth_data_flags & CTAP_authenticator_data_flags_uv) != 0u;
	cbor_encoding_check(cbor_encode_uint(&map, CTAP_getAssertion_res_user));
	CTAP_userEntity user;
	ctap_credential_get_user(credential, &user);
	ctap_check(ctap_encode_pub_key_cred_user_entity(
		&map,
		&user,
		include_user_identifiable_info
	));
	if (num_credentials > 0) {
		// numberOfCredentials (0x05)
		cbor_encoding_check(cbor_encode_uint(&map, CTAP_getAssertion_res_numberOfCredentials));
		cbor_encoding_check(cbor_encode_uint(&map, num_credentials));
	}
	// close response map
	cbor_encoding_check(cbor_encoder_close_container(encoder, &map));

	return CTAP2_OK;

}

uint8_t ctap_get_assertion(ctap_state_t *const state, CborValue *const it, CborEncoder *const encoder) {

	uint8_t ret;

	CTAP_getAssertion ga;
	CTAP_mc_ga_common *const params = &ga.common;
	ctap_check(ctap_parse_get_assertion(it, &ga));

	const bool pinUvAuthParam_present = ctap_param_is_present(params, CTAP_getAssertion_pinUvAuthParam);
	ctap_pin_protocol_t *pin_protocol = NULL;

	const ctap_crypto_t *const crypto = state->crypto;

	// 6.2.2. authenticatorGetAssertion Algorithm
	// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#sctn-getAssert-authnr-alg
	// see also WebAuthn 6.3.3. The authenticatorGetAssertion Operation
	// https://w3c.github.io/webauthn/#sctn-op-get-assertion

	// rpId_hash is needed throughout the whole algorithm, so we compute it right away.
	ctap_compute_rp_id_hash(crypto, params->rpId.hash, &params->rpId);

	// 1. + 2.
	ctap_check(handle_pin_uv_auth_param_and_protocol(
		state,
		pinUvAuthParam_present,
		params->pinUvAuthParam.size,
		ctap_param_is_present(params, CTAP_getAssertion_pinUvAuthProtocol),
		params->pinUvAuthProtocol,
		&pin_protocol
	));

	// 3. Create a new authenticatorGetAssertion response structure
	//    and initialize both its "uv" bit and "up" bit as false.
	uint32_t auth_data_flags = 0u;

	// 4. If the options parameter is present, process all option keys and values present in the parameter.
	//    Treat any option keys that are not understood as absent.
	// 4.5. If the "up" option is not present then, let the "up" option be treated
	//      as being present with the value true.
	const bool up = get_option_value_or_true(&params->options, CTAP_ma_ga_option_up);
	debug_log("option up=%d" nl, up);
	if (ctap_param_is_present(params, CTAP_getAssertion_options)) {
		// 1. +. 2. + 3.
		ctap_check(ensure_uv_option_false(pinUvAuthParam_present, &params->options));
		// 4. If the "rk" option is present then, return CTAP2_ERR_UNSUPPORTED_OPTION.
		if (is_option_present(&params->options, CTAP_ma_ga_option_rk)) {
			return CTAP2_ERR_UNSUPPORTED_OPTION;
		}
	}

	// 5. (not applicable to LionKey) If the alwaysUv option ID is present and true and ...

	// 6. If the authenticator is protected by some form of user verification, then:
	if (ctap_is_pin_set(state)) {
		// 1. If pinUvAuthParam parameter is present (implying the "uv" option is false (see Step 4)):
		if (pinUvAuthParam_present) {
			assert(pin_protocol != NULL); // <- this should be ensured by Step 2
			ctap_check(ensure_valid_pin_uv_auth_param(
				state,
				params,
				pin_protocol,
				CTAP_clientPIN_pinUvAuthToken_permission_ga
			));
			auth_data_flags |= CTAP_authenticator_data_flags_uv;
		}
		// 2. If the "uv" option is present and set to true (implying the pinUvAuthParam parameter is not present,
		//    and that the authenticator supports an enabled built-in user verification method, see Step 4):
		//    (not applicable to LionKey)
	}

	ctap_discard_stateful_command_state(state);
	ctap_get_assertion_state_t *ga_state = &state->stateful_command_state.get_assertion;
	ga_state->extensions = 0;
	ga_state->num_credentials = 0;
	ga_state->next_credential_idx = 0;
	const size_t max_num_credentials = sizeof(ga_state->credentials) / sizeof(ctap_credential_handle_t);
	const bool response_has_uv = (auth_data_flags & CTAP_authenticator_data_flags_uv) != 0u;
	// 7. Locate all credentials that are eligible for retrieval under the specified criteria:
	//    Note: Our implementation also performs steps 7.3., 7.4., and 7.5. at the same time.
	if (ctap_param_is_present(params, CTAP_getAssertion_allowList)) {
		// 7.1. If the allowList parameter is present and is non-empty,
		//      locate all denoted credentials created by this authenticator and bound to the specified rpId.
		ctap_check(process_allow_list(
			state,
			&ga.allowList,
			&params->rpId,
			response_has_uv,
			ga_state->credentials,
			&ga_state->num_credentials,
			max_num_credentials
		));
	} else {
		// 7.2. If an allowList is not present, locate all discoverable credentials
		//      that are created by this authenticator and bound to the specified rpId.
		// TODO: (stems from Step 12.1.2.)
		//   Order the credentials in the applicable credentials list
		//   by the time when they were created in reverse order.
		//   (I.e. the first credential is the most recently created.)
		ctap_check(ctap_find_discoverable_credentials_by_rp_id(
			state->storage,
			&params->rpId,
			NULL,
			response_has_uv,
			ga_state->credentials,
			&ga_state->num_credentials,
			max_num_credentials
		));

	}
	// 7.6. If the applicable credentials list is empty, return CTAP2_ERR_NO_CREDENTIALS.
	if (ga_state->num_credentials == 0) {
		info_log("no credentials found" nl);
		return CTAP2_ERR_NO_CREDENTIALS;
	}

	// 8. (not applicable to LionKey) If evidence of user interaction was provided as part of Step 6.2
	//    (i.e., by invoking performBuiltInUv()): ...

	// 9. If the "up" option is set to true or not present
	//    (not present has already been replaced with true, the default, in Step 4):
	if (up) {
		// 9.1. and 9.2. together (since we do not perform Step 8, the "up" bit in the response
		// can never be true at this point and thus the 9.2.1. check is unnecessary).
		ctap_check(ensure_user_present(state, pinUvAuthParam_present));
		// 9.3. Set the "up" bit to true in the response.
		auth_data_flags |= CTAP_authenticator_data_flags_up;
		// 9.4. Call clearUserPresentFlag(), clearUserVerifiedFlag(), and clearPinUvAuthTokenPermissionsExceptLbw().
		//      Note: This consumes both the "user present state", sometimes referred to as the "cached UP",
		//      and the "user verified state", sometimes referred to as "cached UV".
		//      These functions are no-ops if there is not an in-use pinUvAuthToken.
		ctap_pin_uv_auth_token_clear_user_present_flag(state);
		ctap_pin_uv_auth_token_clear_user_verified_flag(state);
		ctap_pin_uv_auth_token_clear_permissions_except_lbw(state);
	}

	// extensions processing

	// 10. If the extensions parameter is present:
	if (ctap_param_is_present(params, CTAP_getAssertion_extensions)) {
		// 1. Process any extensions that this authenticator supports,
		//    ignoring any that it does not support.
		// 2. Authenticator extension outputs generated by the authenticator
		//    extension processing are returned in the authenticator data.
		//    The set of keys in the authenticator extension outputs map MUST
		//    be equal to, or a subset of, the keys of the authenticator extension inputs map.
		// Note: Some extensions may produce different output depending on the state of
		// the "uv" bit and/or "up" bit in the response.
		if ((params->extensions_present & CTAP_extension_hmac_secret) != 0u) {
			ctap_check(prepare_get_assertion_hmac_secret_extension(
				state, ga_state, &ga.hmac_secret, auth_data_flags
			));
		}
	}

	// 11. If the allowList parameter is present: ...
	//     (nothing to do here, already handled by our other logic)

	// copy the remaining necessary data into the ga_state
	// so that they can be used by the generate_get_assertion_response()
	memcpy(ga_state->client_data_hash, params->clientDataHash.data, CTAP_SHA256_HASH_SIZE);
	memcpy(ga_state->auth_data_rp_id_hash, params->rpId.hash, CTAP_SHA256_HASH_SIZE);
	ga_state->auth_data_flags = auth_data_flags;

	// 13. Sign the clientDataHash along with authData.
	ctap_check(generate_get_assertion_response(
		encoder,
		crypto,
		state->storage,
		ga_state
	));

	// 12. If allowList is not present:
	// 12.1. If numberOfCredentials is one, select that credential.
	//       (nothing to do here, already handled by our other logic).
	// 12.2. If numberOfCredentials is more than one:
	// 12.2.1. Order the credentials ... (already handled by our modified 7.2.)
	// (not applicable) 12.2.3. If authenticator has a display ...
	// 12.2.2. If the authenticator does not have a display,
	//         or the authenticator does have a display and the "uv" and "up" options are false:
	if (!ctap_param_is_present(params, CTAP_getAssertion_allowList) && ga_state->num_credentials > 1) {
		state->stateful_command_state.active_cmd = CTAP_STATEFUL_CMD_GET_ASSERTION;
		// 12.2.2.2. Create a credential counter (credentialCounter) and set it to 1.
		//    This counter signifies the next credential to be returned by the authenticator,
		//    assuming zero-based indexing.
		ga_state->next_credential_idx++;
		// 12.2.2.3. Start a timer. This is used during authenticatorGetNextAssertion command.
		//           This step is OPTIONAL if transport is done over NFC.
		ctap_update_stateful_command_timer(state);
	} else {
		// We could just leave the partial state in memory since it's not valid anyway
		// (because, at this point, stateful_command_state.active_cmd == CTAP_STATEFUL_CMD_NONE).
		// However, as a good practice, we want to avoid keeping any potentially sensitive state
		// in memory longer than necessary. To avoid unnecessary large memset()
		// in ctap_discard_stateful_command_state(), we manually clean up the partial state.
		memset(ga_state->client_data_hash, 0, CTAP_SHA256_HASH_SIZE);
		memset(ga_state->auth_data_rp_id_hash, 0, CTAP_SHA256_HASH_SIZE);
		ga_state->auth_data_flags = 0;
		ga_state->extensions = 0;
		memset(&ga_state->hmac_secret_state, 0, sizeof(ga_state->hmac_secret_state));
		static_assert(
			sizeof(ga_state->credentials[0]) == sizeof(ctap_credential_handle_t),
			"sizeof(ga_state->credentials[0]) == sizeof(ctap_credential_handle_t)"
		);
		memset(ga_state->credentials, 0, sizeof(ga_state->credentials[0]) * ga_state->num_credentials);
		ga_state->num_credentials = 0;
	}

	return CTAP2_OK;

}

uint8_t ctap_get_next_assertion(ctap_state_t *const state, CborValue *const it, CborEncoder *const encoder) {

	// This command does not take any parameters.
	lion_unused(it);

	// 6.3. authenticatorGetNextAssertion (0x08)
	// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#authenticatorGetNextAssertion

	uint8_t ret;

	// When this command is received, the authenticator performs the following procedure:

	// 1. If authenticator does not remember any authenticatorGetAssertion parameters,
	//    return CTAP2_ERR_NOT_ALLOWED.
	// 2. If the credentialCounter is equal to or greater than numberOfCredentials,
	//    return CTAP2_ERR_NOT_ALLOWED.
	//    Note:
	//      In our implementation, we discard the state as soon as the iteration reaches the end,
	//      so only need to check the ga_state->valid.
	// 3. If timer since the last call to authenticatorGetAssertion/authenticatorGetNextAssertion
	//    is greater than 30 seconds, discard the current authenticatorGetAssertion state
	//    and return CTAP2_ERR_NOT_ALLOWED. This step is OPTIONAL if transport is done over NFC.
	//    (implemented centrally for all stateful commands in ctap_request() by discarding the state
	//     when the timer since the last call is greater than 30 seconds)
	if (state->stateful_command_state.active_cmd != CTAP_STATEFUL_CMD_GET_ASSERTION) {
		return CTAP2_ERR_NOT_ALLOWED;
	}

	ctap_get_assertion_state_t *ga_state = &state->stateful_command_state.get_assertion;

	// This should be ensured by the check at the end of the function,
	// which discards the state as soon as the iteration reaches the end
	// (i.e., if ga_state->next_credential_idx == ga_state->num_credentials).
	assert(ga_state->next_credential_idx < ga_state->num_credentials);

	// 6. Sign the clientDataHash along with authData. (also handles Step 5)
	ctap_check(generate_get_assertion_response(
		encoder,
		state->crypto,
		state->storage,
		ga_state
	));

	// 7. Reset the timer. This step is OPTIONAL if transport is done over NFC.
	ctap_update_stateful_command_timer(state);
	// 8. Increment credentialCounter.
	ga_state->next_credential_idx++;
	// Discard the state as soon as the iteration finishes.
	if (ga_state->next_credential_idx == ga_state->num_credentials) {
		ctap_discard_stateful_command_state(state);
	}

	return CTAP2_OK;

}
