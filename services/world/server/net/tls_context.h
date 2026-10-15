// Server TLS context. Uses TLS_CERT_FILE/TLS_KEY_FILE when set; otherwise generates a self-signed dev certificate
// into TLS_DEV_CERT_DIR on first start and reuses it afterwards (mirrors the dev-key fallback in services/auth).
#pragma once
#include <asio/ssl/context.hpp>

#include <string>

namespace kast::net {

struct TlsFiles {
    std::string certFile, keyFile; // both empty = dev certificate
    std::string devCertDir;
};

// Throws std::runtime_error when the files can't be read or the certificate can't be generated.
asio::ssl::context make_server_tls(const TlsFiles& files);

} // namespace kast::net
