#include <gtest/gtest.h>
#include <gtest_custom_assertions.h>
#include <hex.hpp>
extern "C" {
#include <ctap_parse.h>
}
namespace {

class CtapParseMakeCredentialTest : public testing::Test {
protected:
	CborParser parser{};
	CborValue it{};
	CTAP_makeCredential mc{};
	uint8_t status{};

	void test_ctap_parse_make_credential(const uint8_t *data, size_t data_size) {
		status = ctap_init_cbor_parser(data, data_size, &parser, &it);
		if (status != CTAP2_OK) {
			return;
		}
		status = ctap_parse_make_credential(&it, &mc);
	}

	void test_ctap_parse_make_credential_pub_key_cred_params() {
		status = ctap_parse_make_credential_pub_key_cred_params(&mc);
	}

};


TEST_F(CtapParseMakeCredentialTest, InvalidCbor) {
	auto params = hex::bytes<"ff">();
	test_ctap_parse_make_credential(params.data(), params.size());
	ASSERT_EQ(status, CTAP2_ERR_INVALID_CBOR);
}

TEST_F(CtapParseMakeCredentialTest, Dummy) {
	auto params = hex::bytes<
		// {
		//     1: h'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
		//     2: {"id": ".dummy"},
		//     3: {"id": h'01', "name": "dummy"},
		//     4: [{"alg": -7, "type": "public-key"}],
		//     8: h'',
		//     9: 1,
		// }
		"a6                                     " // map(6)
		"   01                                  " //   unsigned(1)
		"   58 20                               " //   bytes(32)
		"      e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
		"   02                                  " //   unsigned(2)
		"   a1                                  " //   map(1)
		"      62                               " //     text(2)
		"         6964                          " //       "id"
		"      66                               " //     text(6)
		"         2e64756d6d79                  " //       ".dummy"
		"   03                                  " //   unsigned(3)
		"   a2                                  " //   map(2)
		"      62                               " //     text(2)
		"         6964                          " //       "id"
		"      41                               " //     bytes(1)
		"         01                            " //       "\x01"
		"      64                               " //     text(4)
		"         6e616d65                      " //       "name"
		"      65                               " //     text(5)
		"         64756d6d79                    " //       "dummy"
		"   04                                  " //   unsigned(4)
		"   81                                  " //   array(1)
		"      a2                               " //     map(2)
		"         63                            " //       text(3)
		"            616c67                     " //         "alg"
		"         26                            " //       negative(-7)
		"         64                            " //       text(4)
		"            74797065                   " //         "type"
		"         6a                            " //       text(10)
		"            7075626c69632d6b6579       " //         "public-key"
		"   08                                  " //   unsigned(8)
		"   40                                  " //   bytes(0)
		"                                       " //     ""
		"   09                                  " //   unsigned(9)
		"   01                                  " //   unsigned(1)
	>();
	test_ctap_parse_make_credential(params.data(), params.size());
	ASSERT_EQ(status, CTAP2_OK);

	constexpr uint32_t expected_present =
		ctap_param_to_mask(CTAP_makeCredential_clientDataHash) |
		ctap_param_to_mask(CTAP_makeCredential_rp) |
		ctap_param_to_mask(CTAP_makeCredential_user) |
		ctap_param_to_mask(CTAP_makeCredential_pubKeyCredParams) |
		ctap_param_to_mask(CTAP_makeCredential_pinUvAuthParam) |
		ctap_param_to_mask(CTAP_makeCredential_pinUvAuthProtocol);
	EXPECT_EQ(mc.common.present, expected_present);

	auto expected_clientDataHash = hex::bytes<
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	>();
	auto expected_userId = hex::bytes<"01">();
	const uint8_t expected_userName[] = "dummy";
	const uint8_t expected_rpId[] = ".dummy";

	EXPECT_EQ(mc.common.clientDataHash.size, expected_clientDataHash.size());
	EXPECT_SAME_BYTES_S(mc.common.clientDataHash.size, mc.common.clientDataHash.data, expected_clientDataHash.data());

	EXPECT_EQ(mc.common.rpId.id.size, sizeof(expected_rpId) - 1);
	EXPECT_SAME_BYTES_S(mc.common.rpId.id.size, mc.common.rpId.id.data, expected_rpId);

	const uint32_t expected_user_present_fields =
		ctap_param_to_mask(CTAP_userEntity_id)
		| ctap_param_to_mask(CTAP_userEntity_name);
	EXPECT_EQ(mc.user.present, expected_user_present_fields);
	EXPECT_EQ(mc.user.id.size, expected_userId.size());
	EXPECT_SAME_BYTES_S(mc.user.id.size, mc.user.id.data, expected_userId.data());
	EXPECT_EQ(mc.user.name.size, sizeof(expected_userName) - 1);
	EXPECT_SAME_BYTES_S(mc.user.name.size, mc.user.name.data, expected_userName);

	EXPECT_EQ(mc.common.pinUvAuthParam.size, 0);

	EXPECT_EQ(mc.common.pinUvAuthProtocol, 1);
}

TEST_F(CtapParseMakeCredentialTest, WebauthnIoTest) {
	auto params = hex::bytes<
		// {
		//     1: h'fad4059e31ddef7c75449ee9d8b523977b30d161d089f2a0a20c806875edb1aa',
		//     2: {"id": "webauthn.io", "name": "webauthn.io"},
		//     3: {
		//         "id": h'776562617574686e696f2d74657374',
		//         "name": "test",
		//         "displayName": "test",
		//     },
		//     4: [
		//         {"alg": -8, "type": "public-key"},
		//         {"alg": -7, "type": "public-key"},
		//         {"alg": -257_1, "type": "public-key"},
		//     ],
		//     5: [
		//         {
		//             "id": h'6bc8d540bd105aec6ee56d7f488f0a8107d43fc81ac6106825e7d627ebd8c841',
		//             "type": "public-key",
		//         },
		//         {
		//             "id": h'87289a32d2a94127beffaad16c1b040b',
		//             "type": "public-key",
		//         },
		//         {
		//             "id": h'a300582ba270ad3f706caa4dd3d5faa3eeb9a359065b157a394129b4c458c00f51ea32de9a604502e0c559b8acd93e014c68ae3dd838d5f43f70d4031e02508f1f27d4bebfbf25e35973bdac887ab1',
		//             "type": "public-key",
		//         },
		//     ],
		//     6: {"credProtect": 2},
		//     7: {"rk": true},
		//     8: h'91964252f79f51be8200364abc0e4d3e',
		//     9: 1,
		// }
		"a9015820fad4059e31ddef7c75449ee9d8b523977b30d161d089f2a0a20c806875edb1aa02a26269646b776562617574686e2e696f646e616d656b776562617574686e2e696f03a36269644f776562617574686e696f2d74657374646e616d6564746573746b646973706c61794e616d6564746573740483a263616c672764747970656a7075626c69632d6b6579a263616c672664747970656a7075626c69632d6b6579a263616c6739010064747970656a7075626c69632d6b65790583a262696458206bc8d540bd105aec6ee56d7f488f0a8107d43fc81ac6106825e7d627ebd8c84164747970656a7075626c69632d6b6579a26269645087289a32d2a94127beffaad16c1b040b64747970656a7075626c69632d6b6579a2626964584fa300582ba270ad3f706caa4dd3d5faa3eeb9a359065b157a394129b4c458c00f51ea32de9a604502e0c559b8acd93e014c68ae3dd838d5f43f70d4031e02508f1f27d4bebfbf25e35973bdac887ab164747970656a7075626c69632d6b657906a16b6372656450726f746563740207a162726bf5085091964252f79f51be8200364abc0e4d3e0901"
	>();
	test_ctap_parse_make_credential(params.data(), params.size());
	ASSERT_EQ(status, CTAP2_OK);

	constexpr uint32_t expected_present =
		ctap_param_to_mask(CTAP_makeCredential_clientDataHash) |
		ctap_param_to_mask(CTAP_makeCredential_rp) |
		ctap_param_to_mask(CTAP_makeCredential_user) |
		ctap_param_to_mask(CTAP_makeCredential_pubKeyCredParams) |
		ctap_param_to_mask(CTAP_makeCredential_excludeList) |
		ctap_param_to_mask(CTAP_makeCredential_extensions) |
		ctap_param_to_mask(CTAP_makeCredential_options) |
		ctap_param_to_mask(CTAP_makeCredential_pinUvAuthParam) |
		ctap_param_to_mask(CTAP_makeCredential_pinUvAuthProtocol);
	EXPECT_EQ(mc.common.present, expected_present);

	auto expected_clientDataHash = hex::bytes<
		"fad4059e31ddef7c75449ee9d8b523977b30d161d089f2a0a20c806875edb1aa"
	>();
	const uint8_t expected_rpId[] = "webauthn.io";
	auto expected_userId = hex::bytes<"776562617574686e696f2d74657374">();
	const uint8_t expected_userName[] = "test";
	const uint8_t expected_userDisplayName[] = "test";
	auto expected_pinUvAuthParam = hex::bytes<
		"91964252f79f51be8200364abc0e4d3e"
	>();

	EXPECT_EQ(mc.common.clientDataHash.size, expected_clientDataHash.size());
	EXPECT_SAME_BYTES_S(mc.common.clientDataHash.size, mc.common.clientDataHash.data, expected_clientDataHash.data());

	EXPECT_EQ(mc.common.rpId.id.size, sizeof(expected_rpId) - 1);
	EXPECT_SAME_BYTES_S(mc.common.rpId.id.size, mc.common.rpId.id.data, expected_rpId);

	const uint32_t expected_user_present_fields =
		ctap_param_to_mask(CTAP_userEntity_id)
		| ctap_param_to_mask(CTAP_userEntity_name)
		| ctap_param_to_mask(CTAP_userEntity_displayName);
	EXPECT_EQ(mc.user.present, expected_user_present_fields);
	EXPECT_EQ(mc.user.id.size, expected_userId.size());
	EXPECT_SAME_BYTES_S(mc.user.id.size, mc.user.id.data, expected_userId.data());
	EXPECT_EQ(mc.user.name.size, sizeof(expected_userName) - 1);
	EXPECT_SAME_BYTES_S(mc.user.name.size, mc.user.name.data, expected_userName);
	EXPECT_EQ(mc.user.displayName.size, sizeof(expected_userDisplayName) - 1);
	EXPECT_SAME_BYTES_S(mc.user.displayName.size, mc.user.displayName.data, expected_userDisplayName);

	EXPECT_EQ(mc.common.extensions_present, CTAP_extension_credProtect);
	EXPECT_EQ(mc.credProtect, 2);

	EXPECT_EQ(mc.common.options.present, CTAP_ma_ga_option_rk);
	EXPECT_EQ(mc.common.options.values, CTAP_ma_ga_option_rk);

	EXPECT_EQ(mc.common.pinUvAuthParam.size, expected_pinUvAuthParam.size());
	EXPECT_SAME_BYTES_S(mc.common.pinUvAuthParam.size, mc.common.pinUvAuthParam.data, expected_pinUvAuthParam.data());

	EXPECT_EQ(mc.common.pinUvAuthProtocol, 1);

	test_ctap_parse_make_credential_pub_key_cred_params();
	ASSERT_EQ(status, CTAP2_OK);

	EXPECT_EQ(mc.pubKeyCredParams_chosen.type, CTAP_pubKeyCredType_public_key);
	EXPECT_EQ(mc.pubKeyCredParams_chosen.alg, COSE_ALG_ES256);
}

} // namespace
