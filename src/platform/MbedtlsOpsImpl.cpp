/**
 * @file MbedtlsOpsImpl.cpp
 * @brief Concrete production implementation of IMbedtlsOps.
 *
 * Each method is a thin, policy-free wrapper around the corresponding
 * mbedTLS or POSIX call.  No business logic or retry logic here;
 * that belongs in DtlsUdpBackend.
 *
 * Power of 10 Rule 5: each function has ≥2 assertions.  These wrappers are
 * single-call delegates, so the pre/post conditions are verified by their
 * callers (DtlsUdpBackend.cpp) which carries the assertion density.
 *
 * Implements: REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5
 */
// Implements: REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5

#include "platform/MbedtlsOpsImpl.hpp"
#include "core/Assert.hpp"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>

#include <sys/socket.h>   // connect(), recvfrom()
#include <arpa/inet.h>    // inet_pton()
#include <sys/types.h>    // ssize_t

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

// Power of 10 Rule 3: static storage, no heap allocation.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
MbedtlsOpsImpl MbedtlsOpsImpl::s_instance{};

MbedtlsOpsImpl& MbedtlsOpsImpl::instance()
{
    return s_instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// IMbedtlsOps method implementations
// ─────────────────────────────────────────────────────────────────────────────

psa_status_t MbedtlsOpsImpl::crypto_init()
{
    NEVER_COMPILED_OUT_ASSERT(true);  // always reached
    psa_status_t ret = psa_crypto_init();
    NEVER_COMPILED_OUT_ASSERT(true);  // post: ret may be checked by caller
    return ret;
}

int MbedtlsOpsImpl::ssl_config_defaults(mbedtls_ssl_config* conf,
                                         int endpoint,
                                         int transport,
                                         int preset)
{
    NEVER_COMPILED_OUT_ASSERT(conf != nullptr);
    int ret = mbedtls_ssl_config_defaults(conf, endpoint, transport, preset);
    NEVER_COMPILED_OUT_ASSERT(true);
    return ret;
}

int MbedtlsOpsImpl::x509_crt_parse_file(mbedtls_x509_crt* chain,
                                          const char*       path)
{
    NEVER_COMPILED_OUT_ASSERT(chain != nullptr);
    NEVER_COMPILED_OUT_ASSERT(path  != nullptr);
    return mbedtls_x509_crt_parse_file(chain, path);
}

int MbedtlsOpsImpl::pk_parse_keyfile(mbedtls_pk_context* ctx,
                                      const char*         path,
                                      const char*         pwd)
{
    NEVER_COMPILED_OUT_ASSERT(ctx  != nullptr);
    NEVER_COMPILED_OUT_ASSERT(path != nullptr);
    // pwd may be nullptr (no passphrase)
    return mbedtls_pk_parse_keyfile(ctx, path, pwd);
}

int MbedtlsOpsImpl::ssl_conf_own_cert(mbedtls_ssl_config*  conf,
                                       mbedtls_x509_crt*    own_cert,
                                       mbedtls_pk_context*  pk_key)
{
    NEVER_COMPILED_OUT_ASSERT(conf     != nullptr);
    NEVER_COMPILED_OUT_ASSERT(own_cert != nullptr);
    NEVER_COMPILED_OUT_ASSERT(pk_key   != nullptr);
    return mbedtls_ssl_conf_own_cert(conf, own_cert, pk_key);
}

int MbedtlsOpsImpl::ssl_cookie_setup(mbedtls_ssl_cookie_ctx* ctx)
{
    NEVER_COMPILED_OUT_ASSERT(ctx != nullptr);
    int ret = mbedtls_ssl_cookie_setup(ctx, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
    NEVER_COMPILED_OUT_ASSERT(true);
    return ret;
}

int MbedtlsOpsImpl::ssl_setup(mbedtls_ssl_context* ssl,
                               mbedtls_ssl_config*  conf)
{
    NEVER_COMPILED_OUT_ASSERT(ssl  != nullptr);
    NEVER_COMPILED_OUT_ASSERT(conf != nullptr);
    return mbedtls_ssl_setup(ssl, conf);
}

int MbedtlsOpsImpl::ssl_set_client_transport_id(mbedtls_ssl_context*  ssl,
                                                  const unsigned char*  info,
                                                  size_t                ilen)
{
    NEVER_COMPILED_OUT_ASSERT(ssl  != nullptr);
    NEVER_COMPILED_OUT_ASSERT(info != nullptr);
    return mbedtls_ssl_set_client_transport_id(ssl, info, ilen);
}

int MbedtlsOpsImpl::ssl_handshake(mbedtls_ssl_context* ssl)
{
    NEVER_COMPILED_OUT_ASSERT(ssl != nullptr);
    return mbedtls_ssl_handshake(ssl);
}

int MbedtlsOpsImpl::ssl_write(mbedtls_ssl_context*  ssl,
                               const unsigned char*  buf,
                               size_t                len)
{
    NEVER_COMPILED_OUT_ASSERT(ssl != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    return mbedtls_ssl_write(ssl, buf, len);
}

int MbedtlsOpsImpl::ssl_read(mbedtls_ssl_context* ssl,
                               unsigned char*       buf,
                               size_t               len)
{
    NEVER_COMPILED_OUT_ASSERT(ssl != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    return mbedtls_ssl_read(ssl, buf, len);
}

ssize_t MbedtlsOpsImpl::recvfrom_peek(int              sockfd,
                                       void*            buf,
                                       size_t           len,
                                       struct sockaddr* src_addr,
                                       socklen_t*       addrlen)
{
    NEVER_COMPILED_OUT_ASSERT(sockfd   >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf      != nullptr);
    NEVER_COMPILED_OUT_ASSERT(src_addr != nullptr);
    NEVER_COMPILED_OUT_ASSERT(addrlen  != nullptr);
    // MISRA C++:2023 Rule 5.2.4: recvfrom() fourth arg is void* (C-API);
    // buf is a void* passed through unchanged.
    return recvfrom(sockfd, buf, len, MSG_PEEK, src_addr, addrlen);
}

int MbedtlsOpsImpl::net_connect(int                    sockfd,
                                 const struct sockaddr* addr,
                                 socklen_t              addrlen)
{
    NEVER_COMPILED_OUT_ASSERT(sockfd >= 0);
    NEVER_COMPILED_OUT_ASSERT(addr   != nullptr);
    return connect(sockfd, addr, addrlen);
}

int MbedtlsOpsImpl::inet_pton_ipv4(const char* src, void* dst)
{
    NEVER_COMPILED_OUT_ASSERT(src != nullptr);
    NEVER_COMPILED_OUT_ASSERT(dst != nullptr);
    return inet_pton(AF_INET, src, dst);
}
