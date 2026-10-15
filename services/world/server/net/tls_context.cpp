#include "tls_context.h"
#include "../core/log.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace kast::net {
namespace {

template <typename T, void (*F)(T*)>
struct Free {
    void operator()(T* p) const { F(p); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, Free<EVP_PKEY, EVP_PKEY_free>>;
using X509Ptr = std::unique_ptr<X509, Free<X509, X509_free>>;

void add_ext(X509* cert, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) throw std::runtime_error("X509 extension failed");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

void write_pem(const std::filesystem::path& path, bool isKey, EVP_PKEY* key, X509* cert) {
    BIO* bio = BIO_new_file(path.string().c_str(), "wb");
    if (!bio) throw std::runtime_error("cannot write " + path.string());
    const int ok = isKey ? PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr)
                         : PEM_write_bio_X509(bio, cert);
    BIO_free(bio);
    if (!ok) throw std::runtime_error("cannot write " + path.string());
}

// Self-signed EC P-256 certificate for localhost, the compose service name `world`, and 127.0.0.1. Valid 10 years.
void generate_dev_cert(const std::filesystem::path& certPath, const std::filesystem::path& keyPath) {
    PkeyPtr key(EVP_EC_gen("P-256"));
    X509Ptr cert(X509_new());
    if (!key || !cert) throw std::runtime_error("dev certificate generation failed");
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), static_cast<long>(std::time(nullptr)));
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 3650);
    X509_set_pubkey(cert.get(), key.get());
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("kast-world-dev"), -1, -1, 0);
    X509_set_issuer_name(cert.get(), name);
    add_ext(cert.get(), NID_subject_alt_name, "DNS:localhost,DNS:world,IP:127.0.0.1");
    add_ext(cert.get(), NID_basic_constraints, "critical,CA:TRUE"); // lets clients trust it directly as a CA file
    add_ext(cert.get(), NID_key_usage, "critical,digitalSignature,keyCertSign");
    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) throw std::runtime_error("dev certificate signing failed");

    std::filesystem::create_directories(certPath.parent_path());
    write_pem(keyPath, true, key.get(), nullptr);
    std::filesystem::permissions(keyPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    write_pem(certPath, false, nullptr, cert.get());
}

} // namespace

asio::ssl::context make_server_tls(const TlsFiles& files) {
    asio::ssl::context ctx(asio::ssl::context::tls_server);
    ctx.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                    asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 |
                    asio::ssl::context::single_dh_use);
    std::string cert = files.certFile, key = files.keyFile;
    if (cert.empty()) {
        const auto dir = std::filesystem::path(files.devCertDir);
        cert = (dir / "server.crt").string();
        key = (dir / "server.key").string();
        if (!std::filesystem::exists(cert) || !std::filesystem::exists(key)) {
            generate_dev_cert(cert, key);
            log::warn("generated self-signed dev TLS certificate; set TLS_CERT_FILE/TLS_KEY_FILE in production",
                      {{"cert", cert}});
        } else {
            log::info("using dev TLS certificate", {{"cert", cert}});
        }
    }
    try {
        ctx.use_certificate_chain_file(cert);
        ctx.use_private_key_file(key, asio::ssl::context::pem);
    } catch (const std::exception& e) {
        throw std::runtime_error("TLS certificate/key: " + std::string(e.what()));
    }
    return ctx;
}

} // namespace kast::net
