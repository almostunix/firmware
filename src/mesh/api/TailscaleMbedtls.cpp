// ECDSA verification for tailscale-rs's TLS via ESP-IDF mbedTLS; RustCrypto took ~36 s per handshake.
// Contract with ts_tls_util: 0 valid, 1 invalid, 2 cannot check (the Rust verifier then decides).

#include "configuration.h" // defines ARCH_ESP32

#if defined(MESHTASTIC_TAILSCALE) && defined(ARCH_ESP32)

#include <cstddef>
#include <cstdint>

#include <mbedtls/asn1.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

namespace
{
enum Result : int { VALID = 0, INVALID = 1, CANNOT_CHECK = 2 };

/// Read one DER INTEGER as an unsigned value, strictly: rejects negative and
/// non-minimal encodings, as the Rust verifier does. Returns 0 on success.
int readUnsigned(unsigned char **p, const unsigned char *end, mbedtls_mpi *out)
{
    size_t len;
    if (mbedtls_asn1_get_tag(p, end, &len, MBEDTLS_ASN1_INTEGER) != 0 || len == 0)
        return INVALID;
    const unsigned char *v = *p;
    if (v[0] & 0x80)
        return INVALID; // negative
    if (len > 1 && v[0] == 0 && !(v[1] & 0x80))
        return INVALID; // superfluous leading zero
    if (mbedtls_mpi_read_binary(out, v, len) != 0)
        return CANNOT_CHECK; // allocation failure
    *p += len;
    return 0;
}
} // namespace

extern "C" int ts_mbedtls_ecdsa_verify(uint32_t curve_bits, uint32_t hash_bits, const uint8_t *public_key,
                                       size_t public_key_len, const uint8_t *message, size_t message_len,
                                       const uint8_t *signature, size_t signature_len)
{
    mbedtls_ecp_group_id group_id;
    if (curve_bits == 256)
        group_id = MBEDTLS_ECP_DP_SECP256R1;
    else if (curve_bits == 384)
        group_id = MBEDTLS_ECP_DP_SECP384R1;
    else
        return CANNOT_CHECK;

    unsigned char hash[48];
    size_t hash_len;
    if (hash_bits == 256) {
        if (mbedtls_sha256(message, message_len, hash, 0) != 0)
            return CANNOT_CHECK;
        hash_len = 32;
    } else if (hash_bits == 384) {
        if (mbedtls_sha512(message, message_len, hash, /* is384 */ 1) != 0)
            return CANNOT_CHECK;
        hash_len = 48;
    } else {
        return CANNOT_CHECK;
    }

    mbedtls_ecp_group group;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int result = CANNOT_CHECK;
    unsigned char *p = const_cast<unsigned char *>(signature);
    const unsigned char *end = signature + signature_len;
    size_t seq_len;

    if (mbedtls_ecp_group_load(&group, group_id) != 0) {
        // allocation failure: CANNOT_CHECK
    } else if (mbedtls_ecp_point_read_binary(&group, &q, public_key, public_key_len) != 0) {
        // e.g. a compressed point, which this mbedTLS build may not parse: let Rust decide
    } else if (int rc = mbedtls_ecp_check_pubkey(&group, &q); rc != 0) {
        if (rc == MBEDTLS_ERR_ECP_INVALID_KEY)
            result = INVALID; // otherwise an allocation failure: CANNOT_CHECK
    } else if (mbedtls_asn1_get_tag(&p, end, &seq_len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0 ||
               p + seq_len != end) {
        result = INVALID; // not a DER SEQUENCE spanning the whole signature
    } else if (int rc = readUnsigned(&p, end, &r); rc != 0) {
        result = rc;
    } else if (int rc = readUnsigned(&p, end, &s); rc != 0) {
        result = rc;
    } else if (p != end) {
        result = INVALID; // trailing bytes inside the SEQUENCE
    } else {
        int rc = mbedtls_ecdsa_verify(&group, hash, hash_len, &q, &r, &s);
        if (rc == 0)
            result = VALID;
        else if (rc == MBEDTLS_ERR_ECP_VERIFY_FAILED)
            result = INVALID;
        // anything else (MBEDTLS_ERR_MPI_ALLOC_FAILED, ...): CANNOT_CHECK
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&group);
    return result;
}

#endif
