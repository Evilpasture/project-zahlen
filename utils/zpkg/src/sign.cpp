// SHA256SUMS parsing and ed25519 signature verification.
//
// The checksum file is the trust boundary for a local install, and the remote
// index is the trust boundary for a remote one. Both are plain text so a human
// can read them, and both are signed with ed25519 over their exact bytes.
//
// zpkg never signs. A release is signed once, by the publisher, with anything
// that can produce an ed25519 signature (openssl pkeyutl, minisign, ssh-keygen);
// verification is all that has to live here.
#include "zpkg/zpkg.hpp"
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(ZPKG_HAVE_OPENSSL)
#include <openssl/evp.h>
#endif

namespace ZHLN {

namespace {

bool IsHex(std::string_view text) {
    if (text.empty())
        return false;
    for (char c: text) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

bool LooksLikeSha256(std::string_view text) {
    return text.size() == 64 && IsHex(text);
}

} // namespace

// ---------------------------------------------------------------------------
// SHA256SUMS
// ---------------------------------------------------------------------------
// Three formats are in the wild and all of them are accepted, because a release
// job may be written with sha256sum, with `sha256sum -b`, or with BSD's
// `SHA256 (file) = hex`:
//
//     <hex>  <path>          (two spaces, sha256sum)
//     <hex> *<path>          (binary marker, sha256sum -b)
//     SHA256 (<path>) = <hex> (BSD / LibreSSL)
auto ParseSums(std::string_view text) -> std::expected<std::vector<SumEntry>, Fail> {
    std::vector<SumEntry> entries;
    size_t                position = 0;
    while (position < text.size()) {
        const size_t      newline = text.find('\n', position);
        const size_t      end     = newline == std::string_view::npos ? text.size() : newline;
        const std::string line    = Trim(text.substr(position, end - position));
        position                  = end + 1;
        if (line.empty() || line[0] == '#')
            continue;

        if (line.compare(0, 8, "SHA256 (") == 0) {
            const size_t close = line.rfind(") = ");
            if (close == std::string::npos)
                continue;
            SumEntry entry;
            entry.name   = line.substr(7, close - 7);
            entry.sha256 = AsciiLower(line.substr(close + 4));
            if (LooksLikeSha256(entry.sha256))
                entries.push_back(std::move(entry));
            continue;
        }

        const size_t space = line.find_first_of(" \t");
        if (space == std::string::npos)
            continue;
        SumEntry entry;
        entry.sha256     = AsciiLower(line.substr(0, space));
        std::string rest = Trim(line.substr(space));
        if (!rest.empty() && rest[0] == '*')
            rest.erase(0, 1);
        entry.name = rest;
        if (LooksLikeSha256(entry.sha256) && !entry.name.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::optional<std::string> LookupSum(const std::vector<SumEntry>& entries, std::string_view basename) {
    for (const auto& entry: entries) {
        // sha256sum may record a path (relative or absolute); only the leaf is
        // what the store cares about, and comparing leaves is what makes a sums
        // file portable between a build host and a user's machine.
        const size_t           slash = entry.name.find_last_of("/\\");
        const std::string_view leaf  = slash == std::string::npos ? std::string_view(entry.name) : std::string_view(entry.name).substr(slash + 1);
        if (leaf == basename)
            return entry.sha256;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Signature files
// ---------------------------------------------------------------------------
// One signer per line: "<fingerprint-hex> <base64 signature>". Several publishers
// may sign the same release, and a line whose key is not trusted is ignored
// rather than fatal, so a mirror can add its own signature without breaking
// everybody else.
auto ParseSignatureFile(std::string_view text) -> std::expected<std::vector<Signature>, Fail> {
    std::vector<Signature> signatures;
    size_t                 position = 0;
    while (position < text.size()) {
        const size_t      newline = text.find('\n', position);
        const size_t      end     = newline == std::string_view::npos ? text.size() : newline;
        const std::string line    = Trim(text.substr(position, end - position));
        position                  = end + 1;
        if (line.empty() || line[0] == '#')
            continue;

        const size_t space = line.find_first_of(" \t");
        if (space == std::string::npos)
            continue;
        Signature signature;
        signature.fingerprint = AsciiLower(line.substr(0, space));
        if (!IsHex(signature.fingerprint))
            continue;
        auto decoded = Base64Decode(Trim(line.substr(space)));
        if (!decoded)
            continue; // a malformed line is skipped, as before
        signature.signature = std::move(*decoded);
        if (signature.signature.size() != 64)
            continue;
        signatures.push_back(std::move(signature));
    }
    return signatures;
}

std::string KeyFingerprint(const std::vector<uint8_t>& publicKey) {
    const std::string digest = Sha256Hex(std::string_view(reinterpret_cast<const char*>(publicKey.data()), publicKey.size()));
    return digest.substr(0, 32); // 16 bytes, plenty to name a key
}

// Accepts a raw ed25519 public key as hex or base64, optionally prefixed with
// "zpkg-key-v1", optionally followed by a trailing comment.
auto ParsePublicKey(std::string_view text) -> std::expected<std::vector<uint8_t>, Fail> {
    std::string line = Trim(text);
    if (StartsWith(line, "zpkg-key-v1"))
        line = Trim(line.substr(11));

    const size_t space = line.find_first_of(" \t\n");
    if (space != std::string::npos)
        line = line.substr(0, space);

    if (line.size() == 64 && IsHex(line)) {
        std::vector<uint8_t> key;
        key.reserve(32);
        for (size_t i = 0; i < 64; i += 2) {
            uint8_t    byte   = 0;
            const auto parsed = std::from_chars(line.data() + i, line.data() + i + 2, byte, 16);
            if (parsed.ec != std::errc {})
                return std::unexpected(Fail {"a zpkg public key is not hexadecimal"});
            key.push_back(byte);
        }
        return key;
    }
    return Base64Decode(line).and_then([](std::vector<uint8_t> key) -> std::expected<std::vector<uint8_t>, Fail> {
        if (key.size() != 32) {
            return std::unexpected(Fail {"a zpkg public key is 32 raw bytes as hex or base64 (got " + std::to_string(key.size()) + " bytes)"});
        }
        return key;
    });
}

auto Ed25519Verify(const std::vector<uint8_t>& publicKey, std::string_view message, const std::vector<uint8_t>& signature) -> std::expected<bool, Fail> {
#if defined(ZPKG_HAVE_OPENSSL)
    if (publicKey.size() != 32)
        return std::unexpected(Fail {"an ed25519 public key is 32 bytes"});
    if (signature.size() != 64)
        return std::unexpected(Fail {"an ed25519 signature is 64 bytes"});

    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, publicKey.data(), publicKey.size());
    if (key == nullptr)
        return std::unexpected(Fail {"cannot load the ed25519 public key"});
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        EVP_PKEY_free(key);
        return std::unexpected(Fail {"cannot allocate a signature context"});
    }
    bool ok = false;
    if (EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1) {
        const int result =
            EVP_DigestVerify(context, signature.data(), signature.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size());
        ok = result == 1;
        if (result < 0) {
            EVP_MD_CTX_free(context);
            EVP_PKEY_free(key);
            return std::unexpected(Fail {"the signature could not be checked"});
        }
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return ok;
#else
    (void) publicKey;
    (void) message;
    (void) signature;
    return std::unexpected(Fail {"this zpkg was built without signature support (no OpenSSL), so it cannot verify "
                                 "--require-signatures"});
#endif
}

} // namespace ZHLN
