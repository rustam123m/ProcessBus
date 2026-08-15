// Compares mbedtls and OpenSSL for the R-session HMAC and GCM modes.

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#ifdef HAVE_OPENSSL
#  include <openssl/evp.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sched.h>

namespace
{

constexpr size_t SHA256_BLOCK = 64;
constexpr size_t DIGEST_LEN   = 32;
constexpr size_t TAG_LEN      = 16;   // HMAC-SHA256-128 and GCM tag
constexpr size_t IV_LEN       = 12;
constexpr size_t AAD_LEN      = 26;   // session header covered as additional data

// Same lab key as src/common/r_session_crypto.hpp.
const uint8_t LAB_KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

// Signed region sizes for 16..416 signals, from the measured wire sizes.
const size_t DEFAULT_SIZES[] = { 130, 200, 360, 700, 1000, 1400 };

double now_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast< double >(ts.tv_sec) * 1e9 + static_cast< double >(ts.tv_nsec);
}

void pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::fprintf(stderr, "warning: could not pin to CPU %d\n", cpu);
    }
}

// The key is shorter than the block, so it is zero-extended.
void build_pads(uint8_t ipad[SHA256_BLOCK], uint8_t opad[SHA256_BLOCK])
{
    uint8_t key[SHA256_BLOCK] = {};
    std::memcpy(key, LAB_KEY, sizeof(LAB_KEY));

    for (size_t i = 0; i < SHA256_BLOCK; ++i) {
        ipad[i] = static_cast< uint8_t >(key[i] ^ 0x36);
        opad[i] = static_cast< uint8_t >(key[i] ^ 0x5C);
    }
}

// Current implementation: md layer with hmac_reset() per packet.
class MbedHmac
{
public:
    MbedHmac()
    {
        mbedtls_md_init(&m_ctx);
        mbedtls_md_setup(&m_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&m_ctx, LAB_KEY, sizeof(LAB_KEY));
    }
    ~MbedHmac() { mbedtls_md_free(&m_ctx); }

    void run(const uint8_t* data, size_t size, uint8_t* tag)
    {
        uint8_t digest[DIGEST_LEN];
        mbedtls_md_hmac_reset(&m_ctx);
        mbedtls_md_hmac_update(&m_ctx, data, size);
        mbedtls_md_hmac_finish(&m_ctx, digest);
        std::memcpy(tag, digest, TAG_LEN);
    }

private:
    mbedtls_md_context_t m_ctx = {};
};

// Same library with the ipad/opad midstates cloned per packet.
class MbedHmacClone
{
public:
    MbedHmacClone()
    {
        uint8_t ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
        build_pads(ipad, opad);

        mbedtls_sha256_init(&m_inner);
        mbedtls_sha256_init(&m_outer);
        mbedtls_sha256_init(&m_cur);

        mbedtls_sha256_starts(&m_inner, 0);
        mbedtls_sha256_update(&m_inner, ipad, SHA256_BLOCK);
        mbedtls_sha256_starts(&m_outer, 0);
        mbedtls_sha256_update(&m_outer, opad, SHA256_BLOCK);
    }
    ~MbedHmacClone()
    {
        mbedtls_sha256_free(&m_inner);
        mbedtls_sha256_free(&m_outer);
        mbedtls_sha256_free(&m_cur);
    }

    void run(const uint8_t* data, size_t size, uint8_t* tag)
    {
        uint8_t digest[DIGEST_LEN];

        mbedtls_sha256_clone(&m_cur, &m_inner);
        mbedtls_sha256_update(&m_cur, data, size);
        mbedtls_sha256_finish(&m_cur, digest);

        mbedtls_sha256_clone(&m_cur, &m_outer);
        mbedtls_sha256_update(&m_cur, digest, DIGEST_LEN);
        mbedtls_sha256_finish(&m_cur, digest);

        std::memcpy(tag, digest, TAG_LEN);
    }

private:
    mbedtls_sha256_context m_inner = {};
    mbedtls_sha256_context m_outer = {};
    mbedtls_sha256_context m_cur   = {};
};

class MbedGcm
{
public:
    MbedGcm()
    {
        mbedtls_gcm_init(&m_ctx);
        mbedtls_gcm_setkey(&m_ctx, MBEDTLS_CIPHER_ID_AES, LAB_KEY, 128);
    }
    ~MbedGcm() { mbedtls_gcm_free(&m_ctx); }

    void run(const uint8_t* iv, const uint8_t* aad, uint8_t* data, size_t size, uint8_t* tag)
    {
        mbedtls_gcm_crypt_and_tag(&m_ctx, MBEDTLS_GCM_ENCRYPT, size,
                                  iv, IV_LEN, aad, AAD_LEN,
                                  data, data, TAG_LEN, tag);
    }

private:
    mbedtls_gcm_context m_ctx = {};
};

#ifdef HAVE_OPENSSL

// Midstates cloned through EVP_MD_CTX_copy_ex.
class SslHmac
{
public:
    SslHmac()
    {
        uint8_t ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
        build_pads(ipad, opad);

        m_inner = EVP_MD_CTX_new();
        m_outer = EVP_MD_CTX_new();
        m_cur   = EVP_MD_CTX_new();

        EVP_DigestInit_ex(m_inner, EVP_sha256(), nullptr);
        EVP_DigestUpdate(m_inner, ipad, SHA256_BLOCK);
        EVP_DigestInit_ex(m_outer, EVP_sha256(), nullptr);
        EVP_DigestUpdate(m_outer, opad, SHA256_BLOCK);
    }
    ~SslHmac()
    {
        EVP_MD_CTX_free(m_inner);
        EVP_MD_CTX_free(m_outer);
        EVP_MD_CTX_free(m_cur);
    }

    void run(const uint8_t* data, size_t size, uint8_t* tag)
    {
        uint8_t  digest[DIGEST_LEN];
        unsigned len = 0;

        EVP_MD_CTX_copy_ex(m_cur, m_inner);
        EVP_DigestUpdate(m_cur, data, size);
        EVP_DigestFinal_ex(m_cur, digest, &len);

        EVP_MD_CTX_copy_ex(m_cur, m_outer);
        EVP_DigestUpdate(m_cur, digest, DIGEST_LEN);
        EVP_DigestFinal_ex(m_cur, digest, &len);

        std::memcpy(tag, digest, TAG_LEN);
    }

private:
    EVP_MD_CTX* m_inner = nullptr;
    EVP_MD_CTX* m_outer = nullptr;
    EVP_MD_CTX* m_cur   = nullptr;
};

class SslGcm
{
public:
    SslGcm()
    {
        m_ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(m_ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr);
        EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, nullptr);
        EVP_EncryptInit_ex(m_ctx, nullptr, nullptr, LAB_KEY, nullptr);
    }
    ~SslGcm() { EVP_CIPHER_CTX_free(m_ctx); }

    void run(const uint8_t* iv, const uint8_t* aad, uint8_t* data, size_t size, uint8_t* tag)
    {
        int len = 0;

        // Only the IV is rebound per packet.
        EVP_EncryptInit_ex(m_ctx, nullptr, nullptr, nullptr, iv);
        EVP_EncryptUpdate(m_ctx, nullptr, &len, aad, AAD_LEN);
        EVP_EncryptUpdate(m_ctx, data, &len, data, static_cast< int >(size));
        EVP_EncryptFinal_ex(m_ctx, data + len, &len);
        EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag);
    }

private:
    EVP_CIPHER_CTX* m_ctx = nullptr;
};

#endif

struct Result
{
    std::string name;
    size_t      size;
    double      ns_per_op;
};

// Returns the fastest batch: interrupts and migrations only add time.
template < typename Fn >
double time_op(Fn&& fn, size_t iters, size_t repeats)
{
    // Warm up the code, the data and any lazy dispatch.
    for (size_t i = 0; i < iters / 8 + 1; ++i) {
        fn();
    }

    double best = 1e30;
    for (size_t r = 0; r < repeats; ++r) {
        const double t0 = now_ns();
        for (size_t i = 0; i < iters; ++i) {
            fn();
        }
        const double per_op = (now_ns() - t0) / static_cast< double >(iters);
        best = std::min(best, per_op);
    }
    return best;
}

void usage(const char* argv0)
{
    std::printf(
        "Usage: %s [--cpu N] [--iters N] [--repeats N] [--mhz N] [--sizes a,b,c] [--csv]\n"
        "\n"
        "  --cpu N       pin to this core (default 2)\n"
        "  --iters N     iterations per timed batch (default 20000)\n"
        "  --repeats N   batches; the fastest is reported (default 7)\n"
        "  --mhz N       core clock, to print cycles/byte (default 0 = omit)\n"
        "  --sizes a,b,c payload sizes in bytes (default 130,200,360,700,1000,1400)\n"
        "  --csv         emit CSV instead of a table\n",
        argv0);
}

}

int main(int argc, char** argv)
{
    int    cpu     = 2;
    size_t iters   = 20000;
    size_t repeats = 7;
    double mhz     = 0.0;
    bool   csv     = false;

    std::vector< size_t > sizes(std::begin(DEFAULT_SIZES), std::end(DEFAULT_SIZES));

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool        has_next = (i + 1 < argc);

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (arg == "--csv") {
            csv = true;
        } else if (arg == "--cpu" && has_next) {
            cpu = std::atoi(argv[++i]);
        } else if (arg == "--iters" && has_next) {
            iters = static_cast< size_t >(std::atoll(argv[++i]));
        } else if (arg == "--repeats" && has_next) {
            repeats = static_cast< size_t >(std::atoll(argv[++i]));
        } else if (arg == "--mhz" && has_next) {
            mhz = std::atof(argv[++i]);
        } else if (arg == "--sizes" && has_next) {
            sizes.clear();
            char* spec = argv[++i];
            for (char* tok = std::strtok(spec, ","); tok != nullptr; tok = std::strtok(nullptr, ",")) {
                sizes.push_back(static_cast< size_t >(std::atoll(tok)));
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    pin_to_cpu(cpu);

#ifndef HAVE_OPENSSL
    std::fprintf(stderr,
                 "note: built without OpenSSL - only the mbedtls variants will run.\n"
                 "      Install libssl-dev and reconfigure to get the comparison.\n");
#endif

    const size_t max_size = *std::max_element(sizes.begin(), sizes.end());

    // GCM encrypts in place: one buffer per variant.
    std::vector< uint8_t > plain(max_size + TAG_LEN);
    std::vector< uint8_t > work_a(max_size + TAG_LEN);
    std::vector< uint8_t > work_b(max_size + TAG_LEN);
    uint8_t                aad[AAD_LEN];
    uint8_t                iv[IV_LEN];

    for (size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast< uint8_t >(i * 7 + 1);
    }
    for (size_t i = 0; i < AAD_LEN; ++i) {
        aad[i] = static_cast< uint8_t >(i * 3 + 5);
    }
    for (size_t i = 0; i < IV_LEN; ++i) {
        iv[i] = static_cast< uint8_t >(i + 1);
    }

    MbedHmac      mbed_hmac;
    MbedHmacClone mbed_hmac_clone;
    MbedGcm       mbed_gcm;
#ifdef HAVE_OPENSSL
    SslHmac ssl_hmac;
    SslGcm  ssl_gcm;
#endif

    {
        const size_t n = 360;
        uint8_t      t_ref[TAG_LEN], t_other[TAG_LEN];
        bool         ok = true;

        mbed_hmac.run(plain.data(), n, t_ref);

        mbed_hmac_clone.run(plain.data(), n, t_other);
        if (std::memcmp(t_ref, t_other, TAG_LEN) != 0) {
            std::fprintf(stderr, "FAIL: mbedtls clone HMAC disagrees with mbedtls md HMAC\n");
            ok = false;
        }
#ifdef HAVE_OPENSSL
        ssl_hmac.run(plain.data(), n, t_other);
        if (std::memcmp(t_ref, t_other, TAG_LEN) != 0) {
            std::fprintf(stderr, "FAIL: OpenSSL HMAC disagrees with mbedtls HMAC\n");
            ok = false;
        }
#endif
        std::memcpy(work_a.data(), plain.data(), n);
        mbed_gcm.run(iv, aad, work_a.data(), n, t_ref);
#ifdef HAVE_OPENSSL
        std::memcpy(work_b.data(), plain.data(), n);
        ssl_gcm.run(iv, aad, work_b.data(), n, t_other);
        if (std::memcmp(work_a.data(), work_b.data(), n) != 0) {
            std::fprintf(stderr, "FAIL: GCM ciphertexts differ\n");
            ok = false;
        }
        if (std::memcmp(t_ref, t_other, TAG_LEN) != 0) {
            std::fprintf(stderr, "FAIL: GCM tags differ\n");
            ok = false;
        }
#endif
        if (!ok) {
            return 1;
        }
        std::fprintf(stderr, "correctness: all variants agree\n");
    }

    std::vector< Result > results;

    for (size_t n : sizes) {
        uint8_t tag[TAG_LEN];

        results.push_back({ "mbedtls-hmac", n,
                            time_op([&] { mbed_hmac.run(plain.data(), n, tag); }, iters, repeats) });

        results.push_back({ "mbedtls-hmac-clone", n,
                            time_op([&] { mbed_hmac_clone.run(plain.data(), n, tag); }, iters, repeats) });

        results.push_back({ "mbedtls-gcm", n,
                            time_op([&] { mbed_gcm.run(iv, aad, work_a.data(), n, tag); }, iters, repeats) });

#ifdef HAVE_OPENSSL
        results.push_back({ "openssl-hmac", n,
                            time_op([&] { ssl_hmac.run(plain.data(), n, tag); }, iters, repeats) });

        results.push_back({ "openssl-gcm", n,
                            time_op([&] { ssl_gcm.run(iv, aad, work_b.data(), n, tag); }, iters, repeats) });
#endif
    }

    if (csv) {
        std::printf("variant,bytes,ns_per_op,ns_per_byte,cycles_per_byte\n");
        for (const Result& r : results) {
            const double npb = r.ns_per_op / static_cast< double >(r.size);
            std::printf("%s,%zu,%.1f,%.4f,%.2f\n", r.name.c_str(), r.size, r.ns_per_op, npb,
                        mhz > 0.0 ? npb * mhz / 1000.0 : 0.0);
        }
        return 0;
    }

    std::printf("\n%-20s %7s %11s %11s", "variant", "bytes", "ns/op", "ns/byte");
    if (mhz > 0.0) {
        std::printf(" %11s", "cycles/byte");
    }
    std::printf("\n");

    size_t last = SIZE_MAX;
    for (const Result& r : results) {
        if (r.size != last) {
            std::printf("%s\n", std::string(mhz > 0.0 ? 64 : 52, '-').c_str());
            last = r.size;
        }
        const double npb = r.ns_per_op / static_cast< double >(r.size);
        std::printf("%-20s %7zu %11.1f %11.4f", r.name.c_str(), r.size, r.ns_per_op, npb);
        if (mhz > 0.0) {
            std::printf(" %11.2f", npb * mhz / 1000.0);
        }
        std::printf("\n");
    }
    std::printf("\n");

    return 0;
}
