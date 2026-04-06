// Copyright 2026 Don Jessup
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file MbedtlsOpsImpl.hpp
 * @brief Concrete production implementation of IMbedtlsOps: thin wrappers
 *        around the real mbedTLS and POSIX network functions.
 *
 * MbedtlsOpsImpl is a stateless singleton; all methods delegate directly to
 * the corresponding library call with no additional policy.
 *
 * Power of 10 Rule 3: static storage, no heap allocation.
 * Power of 10 Rule 9: virtual dispatch (vtable) is the permitted exception
 *   for TransportInterface polymorphism; same exception applies here.
 *
 * Implements: REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-6.4.6
 */
// Implements: REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-6.4.6

#ifndef PLATFORM_MBEDTLS_OPS_IMPL_HPP
#define PLATFORM_MBEDTLS_OPS_IMPL_HPP

#include "platform/IMbedtlsOps.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// MbedtlsOpsImpl — real production implementation
// ─────────────────────────────────────────────────────────────────────────────

class MbedtlsOpsImpl : public IMbedtlsOps {
public:
    ~MbedtlsOpsImpl() override {}

    /// Return the process-wide singleton instance.
    /// Power of 10 Rule 3: backed by static storage; no heap allocation.
    static MbedtlsOpsImpl& instance();

    // IMbedtlsOps overrides — each is a direct delegation to the library call
    psa_status_t crypto_init() override;
    int ssl_config_defaults(mbedtls_ssl_config* conf,
                            int endpoint, int transport, int preset) override;
    int x509_crt_parse_file(mbedtls_x509_crt* chain, const char* path) override;
    int pk_parse_keyfile(mbedtls_pk_context* ctx,
                         const char* path, const char* pwd) override;
    int ssl_conf_own_cert(mbedtls_ssl_config* conf,
                          mbedtls_x509_crt* own_cert,
                          mbedtls_pk_context* pk_key) override;
    int ssl_cookie_setup(mbedtls_ssl_cookie_ctx* ctx) override;
    int ssl_setup(mbedtls_ssl_context* ssl, mbedtls_ssl_config* conf) override;
    int ssl_set_hostname(mbedtls_ssl_context* ssl,
                         const char*          hostname) override;
    int ssl_set_client_transport_id(mbedtls_ssl_context* ssl,
                                    const unsigned char* info,
                                    size_t ilen) override;
    int ssl_handshake(mbedtls_ssl_context* ssl) override;
    int ssl_write(mbedtls_ssl_context* ssl,
                  const unsigned char* buf, size_t len) override;
    int ssl_read(mbedtls_ssl_context* ssl,
                 unsigned char* buf, size_t len) override;
    ssize_t recvfrom_peek(int sockfd, void* buf, size_t len,
                          struct sockaddr* src_addr,
                          socklen_t* addrlen) override;
    int net_connect(int sockfd,
                    const struct sockaddr* addr, socklen_t addrlen) override;
    int inet_pton_ipv4(const char* src, void* dst) override;

private:
    MbedtlsOpsImpl() = default;
    // Power of 10 Rule 3: static instance, no heap
    static MbedtlsOpsImpl s_instance;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
};

#endif // PLATFORM_MBEDTLS_OPS_IMPL_HPP
