#include "net_test_util.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <stdexcept>

namespace kast::nettest {
namespace {

std::string pem(BIO* bio)
{
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<size_t>(size));
    BIO_free(bio);
    return out;
}

void addExtension(X509* cert, int nid, const std::string& value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
    if (!ext)
        throw std::runtime_error("bad certificate extension " + value);
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

} // namespace

Cert makeCert(const std::string& subjectAltName, int days)
{
    EVP_PKEY* key = EVP_RSA_gen(2048);
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_time_adj_ex(X509_getm_notAfter(cert), days, 0, nullptr); // days, not seconds: a long is 32 bits here
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Kast test"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    addExtension(cert, NID_basic_constraints, "critical,CA:TRUE");
    addExtension(cert, NID_subject_alt_name, subjectAltName);
    X509_sign(cert, key, EVP_sha256());

    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, cert);
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr);
    X509_free(cert);
    EVP_PKEY_free(key);
    return {pem(certBio), pem(keyBio)};
}

Server::Server(Body body, uint16_t port, const Cert* cert) : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), port})
{
    port_ = acceptor_.local_endpoint().port();
    if (cert) {
        tls_ = std::make_unique<ssl::context>(ssl::context::tls_server);
        tls_->use_certificate_chain(asio::buffer(cert->certPem));
        tls_->use_private_key(asio::buffer(cert->keyPem), ssl::context::pem);
    }
    thread_ = std::thread([this, body = std::move(body)] {
        try {
            body(*this);
        } catch (const std::exception& e) {
            const std::lock_guard lock(mutex_);
            error_ = e.what();
        }
    });
}

Server::~Server()
{
    stopping_ = true;
    for (int i = 0; i < 4; ++i) { // a few, in case the body accepts more than once before it notices
        boost::system::error_code ec;
        tcp::socket poke(io_);
        poke.connect({asio::ip::make_address("127.0.0.1"), port_}, ec);
    }
    thread_.join();
}

std::string Server::error() const
{
    const std::lock_guard lock(mutex_);
    return error_;
}

tcp::socket Server::accept()
{
    tcp::socket socket = acceptor_.accept();
    if (stopping_)
        throw std::runtime_error("stopping");
    ++accepted_;
    return socket;
}

TlsSocket Server::acceptTls()
{
    if (!tls_)
        throw std::logic_error("acceptTls needs a cert");
    TlsSocket stream(accept(), *tls_);
    stream.handshake(ssl::stream_base::server);
    return stream;
}

} // namespace kast::nettest
