#include "sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace rumicar {

namespace {

std::string to_hex(const unsigned char* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(digits[d[i] >> 4]);
        out.push_back(digits[d[i] & 0x0F]);
    }
    return out;
}

struct CtxDeleter {
    void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};

} // namespace

std::string sha256_file_hex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::unique_ptr<EVP_MD_CTX, CtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 init failed");
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0 && EVP_DigestUpdate(ctx.get(), buf.data(), static_cast<size_t>(got)) != 1)
            throw std::runtime_error("sha256 update failed");
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &len) != 1) throw std::runtime_error("sha256 final failed");
    return to_hex(digest, len);
}

std::string sha256_hex(const std::string& data) {
    std::unique_ptr<EVP_MD_CTX, CtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 init failed");
    if (EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1) throw std::runtime_error("sha256 update failed");
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &len) != 1) throw std::runtime_error("sha256 final failed");
    return to_hex(digest, len);
}

} // namespace rumicar
