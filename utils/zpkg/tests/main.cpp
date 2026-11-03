// zpkg's test suite.
//
// It drives the same functions the commands call, so "it compiles" and "it works"
// are the same statement. The archive tests are adversarial on purpose: a package
// manager reads untrusted input at the exact point where the output is a
// directory of files it is about to trust.
#include "zpkg/zpkg.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

#if defined(ZPKG_HAVE_OPENSSL)
#include <openssl/evp.h>
#endif

namespace {

namespace fs = ZHLN::fs;
using ZHLN::Fail;

int failures = 0;
int checks   = 0;

// Assertions are functions, not macros: configure/macro_allowlist.json gates every
// #define in this repository, and a test harness is not a good reason to add one.
// std::source_location records the call site for us.
void Expect(bool condition, const std::source_location& where = std::source_location::current()) {
    ++checks;
    if (condition)
        return;
    ++failures;
    std::printf("FAIL %s:%d in %s\n", where.file_name(), static_cast<int>(where.line()), where.function_name());
}

// A refusal is an std::expected carrying an error, so asking "was this refused"
// is a question about a value. Same checks, same counting, no unwinding.
template <typename T>
void ExpectError(const std::expected<T, Fail>& outcome, const std::source_location& where = std::source_location::current()) {
    ++checks;
    if (!outcome)
        return;
    ++failures;
    std::printf("FAIL %s:%d in %s: expected a refusal\n", where.file_name(), static_cast<int>(where.line()), where.function_name());
}

// Unwrapping, for the cases where a test wants the value. A wrong result is a
// failed check, not a crash, so the counter records it and the test carries on
// with a default-constructed value.
template <typename T>
auto Take(const std::expected<T, Fail>& outcome, const std::source_location& where = std::source_location::current()) -> T {
    ++checks;
    if (outcome)
        return *outcome;
    ++failures;
    std::printf("FAIL %s:%d in %s: %s\n", where.file_name(), static_cast<int>(where.line()), where.function_name(), outcome.error().message.c_str());
    return T {};
}

// For types with no default constructor (Archive, StoreLock) the test has to
// decide what to do when the open fails, so this only reports.
template <typename T>
bool Has(const std::expected<T, Fail>& outcome, const std::source_location& where = std::source_location::current()) {
    ++checks;
    if (outcome)
        return true;
    ++failures;
    std::printf("FAIL %s:%d in %s: %s\n", where.file_name(), static_cast<int>(where.line()), where.function_name(), outcome.error().message.c_str());
    return false;
}

// Test fixtures (writing a tarball, generating a key) can fail too. When they do
// the run is broken, so they report through the same counter rather than
// pretending the test continued.
void Require(const std::expected<void, ZHLN::Fail>& outcome, const std::source_location& where = std::source_location::current()) {
    ++checks;
    if (outcome)
        return;
    ++failures;
    std::printf("FAIL %s:%d in %s: %s\n", where.file_name(), static_cast<int>(where.line()), where.function_name(), outcome.error().message.c_str());
}

// Teardown never reports: std::filesystem's throwing overload does not exist in
// a build with -fno-exceptions, and a leftover fixture directory is not a test
// failure anyway -- on Windows it is usually the previous test's own handle.
void Discard(const fs::path& directory) {
    std::error_code ec;
    fs::remove_all(directory, ec);
}

fs::path MakeTemp(const std::string& name) {
    static std::mt19937_64 rng {std::random_device {}()};
    const fs::path         directory = fs::temp_directory_path() / ("zpkg-test-" + name + "-" + std::to_string(rng()));
    std::error_code        ec;
    fs::remove_all(directory, ec);
    fs::create_directories(directory);
    return directory;
}

// ---------------------------------------------------------------------------
// A tar writer, so the tests do not depend on the system tar
// ---------------------------------------------------------------------------
struct TarEntry {
    std::string path;
    std::string data;
    char        type = '0';
    std::string link;
    unsigned    mode = 0644;
};

std::string Octal(uint64_t value, size_t width) {
    std::string digits;
    do {
        digits.push_back(static_cast<char>('0' + (value & 7u)));
        value >>= 3;
    } while (value != 0);
    while (digits.size() + 1 < width)
        digits.push_back('0');
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::string TarHeader(const TarEntry& entry) {
    std::string header(512, '\0');
    std::memcpy(header.data(), entry.path.data(), std::min<size_t>(entry.path.size(), 100));
    const std::string mode = Octal(entry.mode, 8);
    std::memcpy(header.data() + 100, mode.data(), mode.size());
    const std::string uid = Octal(0, 8);
    std::memcpy(header.data() + 108, uid.data(), uid.size());
    std::memcpy(header.data() + 116, uid.data(), uid.size());
    const std::string size = Octal(entry.data.size(), 12);
    std::memcpy(header.data() + 124, size.data(), size.size());
    const std::string mtime = Octal(0, 12);
    std::memcpy(header.data() + 136, mtime.data(), mtime.size());
    std::memset(header.data() + 148, ' ', 8);
    header[156] = entry.type;
    std::memcpy(header.data() + 157, entry.link.data(), std::min<size_t>(entry.link.size(), 100));
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);

    uint64_t sum = 0;
    for (unsigned char c: header)
        sum += c;
    const std::string checksum = Octal(sum, 8);
    std::memcpy(header.data() + 148, checksum.data(), 7);
    header[155] = ' ';
    return header;
}

std::string BuildTar(const std::vector<TarEntry>& entries) {
    std::string out;
    for (const auto& entry: entries) {
        out += TarHeader(entry);
        if (entry.type == '0' || entry.type == 'L' || entry.type == 'x') {
            out += entry.data;
            const size_t remainder = entry.data.size() % 512;
            if (remainder != 0)
                out.append(512 - remainder, '\0');
        }
    }
    out.append(1024, '\0');
    return out;
}

// GNU tar writes a member whose name does not fit the 100-byte header field as an
// 'L' record holding the real name, NUL-terminated, followed by the member header
// carrying a truncated name.
void AppendGnuLongMember(std::vector<TarEntry>& entries, const std::string& path, const std::string& data) {
    entries.push_back({"././@LongLink", path + std::string(1, '\0'), 'L'});
    entries.push_back({path.substr(0, 100), data});
}

auto WriteTarGz(const fs::path& path, const std::vector<TarEntry>& entries) -> std::expected<void, ZHLN::Fail> {
    const std::string bytes = BuildTar(entries);
    gzFile            file  = gzopen(ZHLN::ToUtf8(path).c_str(), "wb6");
    if (file == nullptr)
        return std::unexpected(ZHLN::Fail {"cannot open " + ZHLN::ToUtf8(path)});
    gzwrite(file, bytes.data(), static_cast<unsigned>(bytes.size()));
    gzclose(file);
    return {};
}

void WritePlainTar(const fs::path& path, const std::vector<TarEntry>& entries) {
    std::ofstream     out(path, std::ios::binary);
    const std::string bytes = BuildTar(entries);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------
// A Zahlen package, as cmake/Toolchains.cmake writes it
// ---------------------------------------------------------------------------
struct PackageSpec {
    std::string              id        = "0.1.0-abc1234-gcc-16.2.1-x86_64-release";
    std::string              version   = "0.1.0";
    std::string              profile   = "release";
    std::string              component = "Engine";
    std::string              package   = "zahlen-engine";
    std::vector<std::string> dependencies;
    std::vector<std::string> provides {"Zahlen::Engine"};
    std::vector<TarEntry>    payload;
};

std::string ToolchainJson(const PackageSpec& spec) {
    return "{\"format\":1,\"id\":\"" + spec.id + "\",\"version\":\"" + spec.version +
           "\",\"commit\":\"abc1234\",\"compiler\":\"GNU\",\"compiler_version\":\"16.2.1\","
           "\"processor\":\"x86_64\",\"profile\":\"" +
           spec.profile + "\",\"abi\":\"deadbeef\"}\n";
}

std::string ManifestJson(const PackageSpec& spec) {
    std::string requirements;
    for (size_t i = 0; i < spec.dependencies.size(); ++i) {
        if (i != 0)
            requirements += ',';
        requirements += "\"" + spec.dependencies[i] + "\"";
    }
    std::string provides;
    for (size_t i = 0; i < spec.provides.size(); ++i) {
        if (i != 0)
            provides += ',';
        provides += "\"" + spec.provides[i] + "\"";
    }
    return "{\"format\":1,\"package\":\"" + spec.package + "\",\"component\":\"" + spec.component + "\",\"toolchain\":\"" + spec.id + "\",\"profile\":\"" +
           spec.profile + "\",\"requires\":[" + requirements + "],\"provides\":[" + provides + "]}\n";
}

std::vector<TarEntry> PackageEntries(const PackageSpec& spec) {
    std::vector<TarEntry> entries;
    entries.push_back({"usr/", {}, '5', {}, 0755});
    entries.push_back({"usr/lib/", {}, '5', {}, 0755});
    entries.push_back({"usr/share/", {}, '5', {}, 0755});
    entries.push_back({"usr/share/zahlen/", {}, '5', {}, 0755});
    entries.push_back({"usr/share/zahlen/manifests/", {}, '5', {}, 0755});
    entries.push_back({"usr/share/zahlen/toolchain.json", ToolchainJson(spec)});
    entries.push_back({"usr/share/zahlen/manifests/" + spec.package + ".json", ManifestJson(spec)});
    for (const auto& entry: spec.payload)
        entries.push_back(entry);
    if (spec.payload.empty()) {
        entries.push_back({"usr/lib/lib" + spec.package + ".so", "payload\n"});
    }
    return entries;
}

fs::path WritePackage(const fs::path& directory, const PackageSpec& spec) {
    const fs::path path = directory / ("zahlen-" + ZHLN::AsciiLower(spec.component) + "-" + spec.id + ".tar.gz");
    Require(WriteTarGz(path, PackageEntries(spec)));
    return path;
}

void WriteSums(const fs::path& directory) {
    std::string           text;
    std::vector<fs::path> archives;
    for (const auto& entry: fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (ZHLN::EndsWith(name, ".tar.gz"))
            archives.push_back(entry.path());
    }
    std::sort(archives.begin(), archives.end());
    for (const auto& archive: archives) {
        text += Take(ZHLN::Sha256FileHex(archive)) + "  " + archive.filename().string() + "\n";
    }
    Require(ZHLN::WriteFile(directory / "SHA256SUMS", text));
}

ZHLN::Env TestEnv(const fs::path& directory) {
    ZHLN::Env env;
    env.root   = directory / "root";
    env.binDir = directory / "bin";
    return env;
}

// ---------------------------------------------------------------------------
// ed25519, so the signature path is exercised rather than assumed
// ---------------------------------------------------------------------------
#if defined(ZPKG_HAVE_OPENSSL)
struct KeyPair {
    std::vector<uint8_t> publicKey;
    std::vector<uint8_t> privateKey;
};

KeyPair MakeKey() {
    // A key that cannot be generated is a broken harness, not a broken test, so
    // this one aborts loudly instead of returning a half-made pair.
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (context == nullptr)
        std::abort();
    EVP_PKEY_keygen_init(context);
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(context, &key) != 1)
        std::abort();
    KeyPair pair;
    pair.publicKey.resize(32);
    pair.privateKey.resize(32);
    size_t length = 32;
    EVP_PKEY_get_raw_public_key(key, pair.publicKey.data(), &length);
    length = 32;
    EVP_PKEY_get_raw_private_key(key, pair.privateKey.data(), &length);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(context);
    return pair;
}

std::vector<uint8_t> SignMessage(const KeyPair& pair, std::string_view message) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, pair.privateKey.data(), 32);
    if (key == nullptr)
        std::abort();
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) != 1)
        std::abort();
    std::vector<uint8_t> signature(64);
    size_t               length = signature.size();
    if (EVP_DigestSign(context, signature.data(), &length, reinterpret_cast<const unsigned char*>(message.data()), message.size()) != 1) {
        std::abort();
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    signature.resize(length);
    return signature;
}

// Signs <dir>/SHA256SUMS the way a release job would.
void SignSums(const fs::path& directory, const KeyPair& pair) {
    const std::string sums      = Take(ZHLN::ReadFile(directory / "SHA256SUMS"));
    const auto        signature = SignMessage(pair, sums);
    Require(ZHLN::WriteFile(
        directory / "SHA256SUMS.ed25519", ZHLN::KeyFingerprint(pair.publicKey) + " " + ZHLN::Base64Encode(signature.data(), signature.size()) + "\n"
    ));
}
#endif

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
void TestSha256() {
    Expect(ZHLN::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Expect(ZHLN::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Expect(ZHLN::Sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    Expect(ZHLN::Sha256Hex(std::string(1000000, 'a')).substr(0, 16) == "cdc76e5c9914fb92");
}

void TestBase64() {
    const std::vector<uint8_t> raw {0x00, 0x01, 0xfe, 0xff, 0x41};
    const std::string          encoded = ZHLN::Base64Encode(raw.data(), raw.size());
    Expect(Take(ZHLN::Base64Decode(encoded)) == raw);
    Expect(Take(ZHLN::Base64Decode("YQ==")) == std::vector<uint8_t> {'a'});
    ExpectError(ZHLN::Base64Decode("not base64!"));
}

void TestSums() {
    const std::string text    = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  zahlen-engine.tar.gz\n"
                                "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *zahlen-data.tar.gz\n"
                                "SHA256 (/tmp/out/zahlen-signal.tar.gz) = "
                                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
                                "# a comment\n"
                                "garbage line\n";
    const auto        entries = Take(ZHLN::ParseSums(text));
    Expect(entries.size() == 3);

    const auto engine = ZHLN::LookupSum(entries, "zahlen-engine.tar.gz");
    Expect(engine && *engine == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Lower-cased on read, and matched by leaf even when a path is recorded.
    Expect(ZHLN::LookupSum(entries, "zahlen-data.tar.gz") == engine);
    Expect(ZHLN::LookupSum(entries, "zahlen-signal.tar.gz") == engine);
    Expect(!ZHLN::LookupSum(entries, "zahlen-camera.tar.gz"));
}

void TestKeysAndSignatures() {
    const auto key = Take(ZHLN::ParsePublicKey(
        "zpkg-key-v1 " + ZHLN::Base64Encode(
                             reinterpret_cast<const uint8_t*>("0123456789abcde"
                                                              "f0123456789abcde"
                                                              "f"),
                             32
                         )
    ));
    Expect(key.size() == 32);
    Expect(ZHLN::KeyFingerprint(key).size() == 32);
    const std::string hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    Expect(Take(ZHLN::ParsePublicKey(hex)).size() == 32);
    ExpectError(ZHLN::ParsePublicKey("too-short"));

    const auto signatures = Take(ZHLN::ParseSignatureFile("0123456789abcdef0123456789abcdef "
                                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\n"
                                                          "not-a-signature\n"));
    Expect(signatures.size() == 1);
    Expect(signatures[0].signature.size() == 64);
}

void TestJson() {
    PackageSpec spec;
    spec.dependencies   = {"zahlen-commandline", "zahlen-signal"};
    const auto manifest = Take(ZHLN::ParseManifestJson(ManifestJson(spec), "test"));
    Expect(manifest.package == "zahlen-engine");
    Expect(manifest.component == "Engine");
    Expect(manifest.dependencies.size() == 2);
    Expect(manifest.provides.size() == 1);

    const auto toolchain = Take(ZHLN::ParseToolchainJson(ToolchainJson(spec), "test"));
    Expect(toolchain.id == spec.id);
    Expect(toolchain.profile == "release");
    Expect(toolchain.abi == "deadbeef");

    // Strict on purpose: a document that does not say what it is, is not a
    // document zpkg may act on.
    ExpectError(ZHLN::ParseManifestJson("{\"format\":1,\"package\":\"x\"}", "test"));
    ExpectError(ZHLN::ParseManifestJson("{\"format\":99,\"package\":\"x\",\"component\":\"X\"}", "test"));
    ExpectError(ZHLN::ParseManifestJson("[]", "test"));
    ExpectError(ZHLN::ParseManifestJson("{", "test"));
    ExpectError(ZHLN::ParseToolchainJson("{\"format\":1}", "test"));
    // An unknown extra key is forward compatible, not an error.
    Expect(
        Take(ZHLN::ParseManifestJson(
                 "{\"format\":1,\"package\":\"p\",\"component\":\"C\","
                 "\"requires\":[],\"future\":true}",
                 "test"
             ))
            .package == "p"
    );
}

void TestIndexRoundtrip() {
    ZHLN::IndexToolchain toolchain;
    toolchain.id      = "0.1.0-abc-gcc-16.2.1-x86_64-release";
    toolchain.version = "0.1.0";
    toolchain.profile = "asan-ubsan";
    toolchain.packages.push_back(
        {"zahlen-application",
         "Application",
         "zahlen-application.tar.gz",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         123,
         {"zahlen-engine", "zahlen-hostrunner"},
         {"Zahlen::Application"}}
    );
    const std::string json   = ZHLN::BuildIndexJson({toolchain});
    const auto        parsed = Take(ZHLN::ParseIndexJson(json, "test"));
    Expect(parsed.toolchains.size() == 1);
    Expect(parsed.toolchains[0].id == toolchain.id);
    Expect(parsed.toolchains[0].profile == "asan-ubsan");
    Expect(parsed.toolchains[0].packages.size() == 1);
    Expect(parsed.toolchains[0].packages[0].package == "zahlen-application");
    Expect(parsed.toolchains[0].packages[0].size == 123);
    // The dependency graph survives the round trip. Without it a remote install
    // can only discover requirements by downloading the archive and reading it.
    Expect(parsed.toolchains[0].packages[0].dependencies.size() == 2);
    Expect(parsed.toolchains[0].packages[0].dependencies[1] == "zahlen-hostrunner");
    Expect(parsed.toolchains[0].packages[0].provides.size() == 1);
    Expect(ZHLN::ToolchainHasDependencies(parsed.toolchains[0]));

    // A format-1 index -- what every release published before this change -- is
    // still readable, and is known not to carry the graph.
    const std::string old = "{\"format\":1,\"toolchains\":[{\"id\":\"x\",\"version\":\"0.1.0\",\"profile\":\"release\","
                            "\"packages\":[{\"package\":\"zahlen-engine\",\"component\":\"Engine\","
                            "\"file\":\"e.tar.gz\",\"sha256\":\"00\",\"size\":1}]}]}";
    const auto        legacy = Take(ZHLN::ParseIndexJson(old, "test"));
    Expect(legacy.toolchains.size() == 1);
    Expect(legacy.toolchains[0].packages[0].dependencies.empty());
    Expect(!ZHLN::ToolchainHasDependencies(legacy.toolchains[0]));
}

void TestArchiveScanAndExtract() {
    const fs::path directory = MakeTemp("archive");
    PackageSpec    spec;
    spec.payload           = {{"usr/lib/libzahlen_engine.so", "engine\n"}, {"usr/bin/zcook", "tool\n", '0', {}, 0755}};
    const fs::path archive = WritePackage(directory, spec);

    auto opened = ZHLN::Archive::Open(archive);
    if (!Has(opened)) {
        Discard(directory);
        return;
    }
    ZHLN::Archive& reader = *opened;
    Require(reader.Scan());
    Expect(reader.Toolchain().id == spec.id);
    Expect(reader.Manifest().package == spec.package);
    Expect(reader.Members().size() >= 8);

    const fs::path destination = directory / "store" / spec.id;
    Require(reader.ExtractInto(destination));
    Expect(ZHLN::PathExists(destination / "usr" / "lib" / "libzahlen_engine.so"));
    Expect(Take(ZHLN::ReadFile(destination / "usr" / "lib" / "libzahlen_engine.so")) == "engine\n");
    Expect(ZHLN::PathExists(destination / "usr" / "bin" / "zcook"));
    // The executable bit survives; nothing else does -- where such bits exist.
    const auto status = fs::status(destination / "usr" / "bin" / "zcook");
#if defined(_WIN32)
    // std::filesystem on Windows reports rw-rw-rw- for every file regardless of
    // what chmod was asked for, and the one bit it can express is the read-only
    // attribute. What matters there is that an installed file stays replaceable.
    Expect((status.permissions() & fs::perms::owner_write) == fs::perms::owner_write);
#else
    Expect((status.permissions() & fs::perms::owner_exec) == fs::perms::owner_exec);
    Expect((status.permissions() & (fs::perms::group_write | fs::perms::others_write)) == fs::perms::none);
#endif

    // A plain (uncompressed) tar is accepted too.
    const fs::path plain = directory / "zahlen-engine.tar";
    WritePlainTar(plain, PackageEntries(spec));
    auto plainOpened = ZHLN::Archive::Open(plain);
    if (Has(plainOpened)) {
        Require(plainOpened->Scan());
        Expect(plainOpened->Manifest().package == spec.package);
    }

    Discard(directory);
}

void TestArchiveRefusals() {
    const fs::path directory = MakeTemp("hostile");
    PackageSpec    spec;
    const auto     base = PackageEntries(spec);

    const auto expectRefusal = [&](std::vector<TarEntry> entries, const std::string& label) {
        const fs::path archive = directory / ("bad-" + label + ".tar.gz");
        Require(WriteTarGz(archive, entries));
        ++checks;
        auto refusal = ZHLN::Archive::Open(archive);
        if (refusal)
            refusal = std::unexpected(refusal->Scan().error());
        if (refusal) {
            std::printf("FAIL archive guard: %s was accepted\n", label.c_str());
            ++failures;
        }
        // Nothing may be written before validation finishes.
        const fs::path destination = directory / ("extract-" + label);
        Expect(!ZHLN::PathExists(destination));
    };

    {
        auto entries = base;
        entries.push_back({"etc/passwd", "root\n"});
        expectRefusal(entries, "outside-usr");
    }
    {
        auto entries = base;
        entries.push_back({"usr/../../escape", "x\n"});
        expectRefusal(entries, "traversal");
    }
    {
        auto entries = base;
        entries.push_back({"/usr/lib/absolute.so", "x\n"});
        expectRefusal(entries, "absolute");
    }
    {
        auto entries = base;
        entries.push_back({"usr\\lib\\backslash.so", "x\n"});
        expectRefusal(entries, "backslash");
    }
    {
        auto entries = base;
        entries.push_back({"usr/lib/stream.so:evil", "x\n"});
        expectRefusal(entries, "colon");
    }
    {
        auto entries = base;
        entries.push_back({"usr/lib/libzahlen_engine.so", "first\n"});
        entries.push_back({"usr/lib/libzahlen_engine.so", "second\n"});
        expectRefusal(entries, "duplicate");
    }
    {
        auto entries = base;
        entries.push_back({"usr/lib/escape", {}, '2', "../../../etc"});
        expectRefusal(entries, "escaping-symlink");
    }
    {
        // A symlink that is a prefix of a later member would let that member be
        // written through the link.
        auto entries = base;
        entries.push_back({"usr/lib", {}, '2', "/tmp"});
        entries.push_back({"usr/lib/through.so", "x\n"});
        expectRefusal(entries, "symlink-parent");
    }
    {
        auto entries = base;
        entries.push_back({"usr/lib/fifo", {}, '6', {}});
        expectRefusal(entries, "device");
    }
    {
        auto entries = base;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [](const TarEntry& entry) { return entry.path == "usr/share/zahlen/toolchain.json"; }), entries.end()
        );
        expectRefusal(entries, "no-toolchain");
    }
    {
        auto entries = base;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [](const TarEntry& entry) { return ZHLN::StartsWith(entry.path, "usr/share/zahlen/manifests/"); }),
            entries.end()
        );
        expectRefusal(entries, "no-manifest");
    }
    {
        // Two manifests means the archive is not one package.
        auto entries = base;
        entries.push_back({"usr/share/zahlen/manifests/zahlen-other.json", ManifestJson(spec)});
        expectRefusal(entries, "two-manifests");
    }
    {
        // A corrupt header checksum must be caught rather than misparsed.
        const fs::path archive = directory / "bad-checksum.tar";
        std::string    bytes   = BuildTar(base);
        bytes[148]             = '9';
        Require(ZHLN::WriteFile(archive, bytes));
        ++checks;
        auto refusal = ZHLN::Archive::Open(archive);
        if (refusal)
            refusal = std::unexpected(refusal->Scan().error());
        if (refusal) {
            std::printf("FAIL archive guard: corrupt checksum was accepted\n");
            ++failures;
        }
    }

    Discard(directory);
}

// cmake's C++-module target files are long enough to leave the 100-byte name
// field, so every component that ships them arrives through a GNU 'L' record.
// The record's size counts the trailing NUL, and a reader that keeps it refuses
// the archive cmake itself just produced -- which is what CI hit.
void TestArchiveLongNames() {
    const fs::path directory = MakeTemp("longnames");
    PackageSpec    spec;

    const std::string longPath = "usr/lib/cmake/ZahlenCharacterController/cxx-modules/cxx-modules-ZahlenCharacterControllerTargets-Release.cmake";
    Expect(longPath.size() > 100);

    {
        auto entries = PackageEntries(spec);
        AppendGnuLongMember(entries, longPath, "# targets\n");
        const fs::path archive = directory / ("zahlen-long-" + spec.id + ".tar.gz");
        Require(WriteTarGz(archive, entries));

        auto opened = ZHLN::Archive::Open(archive);
        if (!Has(opened)) {
            Discard(directory);
            return;
        }
        ZHLN::Archive& reader = *opened;
        Require(reader.Scan());

        bool sawLongMember = false;
        for (const auto& member: reader.Members()) {
            if (member.path == longPath)
                sawLongMember = true;
        }
        Expect(sawLongMember);

        const fs::path destination = directory / "store" / spec.id;
        Require(reader.ExtractInto(destination));
        Expect(Take(ZHLN::ReadFile(destination / fs::path(longPath))) == "# targets\n");
    }

    {
        // Trimming the terminator is not a licence to accept control bytes the
        // writer really put in the name -- and the refusal has to name the byte
        // instead of being cut short by it.
        auto entries = PackageEntries(spec);
        AppendGnuLongMember(entries, "usr/lib/bad\x01name.so", "x\n");
        const fs::path archive = directory / ("zahlen-control-" + spec.id + ".tar.gz");
        Require(WriteTarGz(archive, entries));

        auto opened = ZHLN::Archive::Open(archive);
        if (Has(opened)) {
            const auto refusal = opened->Scan();
            Expect(!refusal);
            if (!refusal) {
                Expect(refusal.error().message.find("\\x01") != std::string::npos);
                Expect(refusal.error().message.find('\x01') == std::string::npos);
            }
        }
    }

    Discard(directory);
}

// A store that moves has to keep working. `active` is a relative link and every
// path zpkg writes is relative, so pointing the root at the new location is all
// it takes. An absolute marker would follow the old path instead, and the
// symptom would be "no active toolchain" -- which is what CI hit when
// scripts/test-toolchain-install.sh moved the store out from under zpkg.
void TestStoreRelocation() {
    const fs::path directory = MakeTemp("relocate");
    ZHLN::Env      env       = TestEnv(directory);
    ZHLN::Store    store(env);

    const fs::path output = directory / "out";
    fs::create_directories(output);
    PackageSpec engine;
    engine.payload = {{"usr/lib/libzahlen_engine.so", "engine\n"}, {"usr/bin/zcook", "cooker\n"}};
    WritePackage(output, engine);
    WriteSums(output);

    ZHLN::InstallOptions options;
    options.requireChecksums = true;
    auto report              = Take(store.Finish(Take(store.InstallDirectory(output, options))));
    Expect(report.installed.size() == 1);
    Expect(Take(store.Active()) == engine.id);

    const fs::path moved = directory / "moved";
    std::error_code ec;
    fs::rename(ZHLN::Native(env.root), ZHLN::Native(moved), ec);
    Expect(!ec);

    ZHLN::Env movedEnv;
    movedEnv.root   = moved;
    movedEnv.binDir = directory / "bin";
    ZHLN::Store movedStore(movedEnv);

    Expect(Take(movedStore.Active()) == engine.id);
    Expect(Take(movedStore.Prefix()) == movedStore.ToolchainDir(engine.id) / "usr");
    Expect(ZHLN::PathExists(Take(movedStore.Prefix()) / "bin" / "zcook"));

#if !defined(_WIN32)
    // Windows may not create the mirror at all, which is why ACTIVE is the
    // authority there; where it exists it must be relative.
    Expect(ZHLN::PathExists(moved / "active"));
    std::error_code linkError;
    const fs::path  target = fs::read_symlink(ZHLN::Native(moved / "active"), linkError);
    Expect(!linkError);
    Expect(!target.is_absolute());
    Expect(target == fs::path("store") / engine.id);
#endif

    // Nothing zpkg wrote may name the old location.
    const fs::path metadata = movedStore.ToolchainDir(engine.id) / "zpkg" / "installed";
    for (const auto& name: {engine.package + ".meta", engine.package + ".json", engine.package + ".files"}) {
        const auto text = Take(ZHLN::ReadFile(metadata / name));
        Expect(text.find(ZHLN::ToUtf8(env.root)) == std::string::npos);
    }

    // And a root that is not there says so, instead of blaming the marker.
    ZHLN::Env absentEnv;
    absentEnv.root   = directory / "absent";
    absentEnv.binDir = directory / "bin";
    const auto missing = ZHLN::Store(absentEnv).Prefix();
    Expect(!missing);
    if (!missing)
        Expect(missing.error().message.find("no store at") != std::string::npos);

    Discard(directory);
}

// Two managers must not work on one store at once, and the failure has to be a
// refusal rather than a corrupted install. flock() and LockFileEx() are both
// per-handle, so the second lock in this same process must fail on either
// platform -- which is exactly what the Windows implementation relies on.
void TestStoreLock() {
    const fs::path root  = MakeTemp("lock");
    auto           first = ZHLN::StoreLock::Acquire(root);
    Expect(Has(first));
    Expect(ZHLN::PathExists(root / ".lock"));
    ExpectError(ZHLN::StoreLock::Acquire(root));
}

void TestStoreLifecycle() {
    const fs::path directory = MakeTemp("store");
    ZHLN::Env      env       = TestEnv(directory);
    ZHLN::Store    store(env);

    const fs::path output = directory / "out";
    fs::create_directories(output);
    PackageSpec engine;
    engine.payload = {{"usr/lib/libzahlen_engine.so", "engine\n"}};
    PackageSpec signal;
    signal.component    = "Signal";
    signal.package      = "zahlen-signal";
    signal.dependencies = {"zahlen-engine"};
    PackageSpec data;
    data.component = "Data";
    data.package   = "zahlen-data";
    data.provides.clear();
    WritePackage(output, engine);
    WritePackage(output, signal);
    WritePackage(output, data);
    WriteSums(output);

    Expect(Take(store.Toolchains()).empty());
    Expect(!Take(store.Active()));

    ZHLN::InstallOptions options;
    options.requireChecksums = true;
    auto report              = Take(store.InstallDirectory(output, options));
    Expect(report.installed.size() == 3);
    Expect(report.toolchain == engine.id);
    report = Take(store.Finish(std::move(report)));
    Expect(report.missing.empty());

    Expect(Take(store.Toolchains()).size() == 1);
    const auto active = Take(store.Active());
    Expect(active && *active == engine.id);
    Expect(Take(store.Prefix()) == store.ToolchainDir(engine.id) / "usr");
    Expect(ZHLN::PathExists(Take(store.Prefix()) / "lib" / "libzahlen_engine.so"));
    Expect(Take(store.Packages(engine.id)).size() == 3);
    Expect(Take(store.ReadToolchain(engine.id)).profile == "release");

    // --only keeps everything else invisible.
    const fs::path subset = directory / "subset";
    PackageSpec    camera;
    camera.component = "Camera";
    camera.package   = "zahlen-camera";
    camera.id        = "0.1.0-abc1234-gcc-16.2.1-x86_64-asan-ubsan";
    camera.profile   = "asan-ubsan";
    fs::create_directories(subset);
    WritePackage(subset, camera);
    WritePackage(subset, engine);
    WriteSums(subset);
    ZHLN::InstallOptions onlyOptions;
    onlyOptions.only = {"Engine"};
    auto onlyReport  = Take(store.InstallDirectory(subset, onlyOptions));
    Expect(onlyReport.installed.size() == 1);
    Expect(onlyReport.skipped.size() == 1);

    // Removing one package leaves the rest of the toolchain intact.
    Require(store.RemovePackage("zahlen-signal", engine.id));
    Expect(Take(store.Packages(engine.id)).size() == 2);
    Expect(!ZHLN::PathExists(Take(store.Prefix()) / "lib" / "libzahlen-signal.so"));
    Expect(ZHLN::PathExists(Take(store.Prefix()) / "lib" / "libzahlen_engine.so"));

    // An active toolchain is not removed without --force.
    ExpectError(store.RemoveToolchain(engine.id, false));
    Require(store.RemoveToolchain(engine.id, true));
    Expect(!Take(store.Active()));

    Discard(directory);
}

void TestChecksumAndSignaturePolicy() {
    const fs::path directory = MakeTemp("trust");
    ZHLN::Env      env       = TestEnv(directory);
    ZHLN::Store    store(env);

    const fs::path output = directory / "out";
    fs::create_directories(output);
    PackageSpec spec;
    WritePackage(output, spec);
    WriteSums(output);

    ZHLN::InstallOptions required;
    required.requireChecksums = true;

    // A tampered archive is fatal.
    {
        const fs::path tampered = output / ("zahlen-engine-" + spec.id + ".tar.gz");
        std::string    bytes    = Take(ZHLN::ReadFile(tampered));
        bytes.push_back('\0');
        Require(ZHLN::WriteFile(tampered, bytes));
        ExpectError(store.InstallArchive(tampered, required));
        WritePackage(output, spec); // restore
        WriteSums(output);
    }

    // A missing checksum entry is fatal only when it is required to exist.
    {
        const fs::path other = directory / "lonely";
        fs::create_directories(other);
        const fs::path       archive = WritePackage(other, spec);
        ZHLN::InstallOptions strict;
        strict.requireChecksums = true;
        ExpectError(store.InstallArchive(archive, strict));
        ZHLN::InstallOptions loose;
        Expect(Take(store.InstallArchive(archive, loose)).installed.size() == 1);
    }

#if defined(ZPKG_HAVE_OPENSSL)
    // Signing: trusted key verifies, untrusted key does not, tampering breaks it.
    const KeyPair  key     = MakeKey();
    const fs::path keyFile = directory / "publisher.pub";
    Require(ZHLN::WriteFile(keyFile, ZHLN::Base64Encode(key.publicKey.data(), key.publicKey.size()) + "\n"));
    Require(store.KeyAdd(keyFile, "tester"));
    Expect(Take(store.Keys()).size() == 1);

    SignSums(output, key);
    ZHLN::InstallOptions signedOptions;
    signedOptions.requireChecksums  = true;
    signedOptions.requireSignatures = true;
    signedOptions.force             = true;
    Expect(Take(store.InstallArchive(output / ("zahlen-engine-" + spec.id + ".tar.gz"), signedOptions)).installed.size() == 1);

    // A signature over different bytes must not verify.
    Require(ZHLN::WriteFile(output / "SHA256SUMS", Take(ZHLN::ReadFile(output / "SHA256SUMS")) + "# tampered\n"));
    ExpectError(store.InstallArchive(output / ("zahlen-engine-" + spec.id + ".tar.gz"), signedOptions));
    WriteSums(output);

    // An untrusted signer is not enough.
    const KeyPair stranger = MakeKey();
    SignSums(output, stranger);
    ExpectError(store.InstallArchive(output / ("zahlen-engine-" + spec.id + ".tar.gz"), signedOptions));
    SignSums(output, key);

    // Forgetting the key revokes the ability to install signed packages.
    Require(store.KeyRemove(ZHLN::KeyFingerprint(key.publicKey)));
    ExpectError(store.InstallArchive(output / ("zahlen-engine-" + spec.id + ".tar.gz"), signedOptions));
#endif

    Discard(directory);
}

void TestRemote() {
    const fs::path directory = MakeTemp("remote");
    ZHLN::Env      env       = TestEnv(directory);
    ZHLN::Store    store(env);

    const fs::path published = directory / "published";
    fs::create_directories(published);
    PackageSpec engine;
    PackageSpec signal;
    signal.component             = "Signal";
    signal.package               = "zahlen-signal";
    signal.dependencies          = {"zahlen-engine"};
    const fs::path engineArchive = WritePackage(published, engine);
    WritePackage(published, signal);
    WriteSums(published);

    // Publishing is `zpkg index`: one JSON describing every archive.
    const auto index = Take(ZHLN::BuildIndex(published));
    Expect(index.size() == 1);
    Expect(index[0].packages.size() == 2);
    Require(ZHLN::WriteFile(published / "index.json", ZHLN::BuildIndexJson(index)));

    const std::string url = ZHLN::ToFileUrl(published);
    Require(store.RemoteAdd("local", url));
    Expect(Take(store.Remotes()).count("local") == 1);

    ZHLN::InstallOptions options;
    auto                 report = Take(ZHLN::InstallFromRemote(store, "local", options));
    Expect(report.installed.size() == 2);
    Expect(report.toolchain == engine.id);
    report                = Take(store.Finish(std::move(report))); // activates the store like any other
    const fs::path prefix = Take(store.Prefix());
    Expect(ZHLN::PathExists(prefix / "lib" / "libzahlen-engine.so") || ZHLN::PathExists(prefix / "lib" / "libzahlen-signal.so"));
    // The download is content-addressed, so a second install needs no fetch.
    Expect(ZHLN::PathExists(store.CacheDir() / Take(ZHLN::Sha256FileHex(published / engineArchive.filename()))));

    // A remote whose index lies about a hash must be refused, not trusted.
    {
        ZHLN::Env   freshEnv = TestEnv(directory / "fresh");
        ZHLN::Store fresh(freshEnv);
        Require(fresh.RemoteAdd("local", url));
        const std::string lying = ZHLN::BuildIndexJson(
            {{engine.id, engine.version, engine.profile, {{engine.package, engine.component, engineArchive.filename().string(), std::string(64, 'a'), 1}}}}
        );
        Require(ZHLN::WriteFile(directory / "published-truth.json", lying));
        // Replace the published index with the lying one.
        const std::string saved = Take(ZHLN::ReadFile(published / "index.json"));
        Require(ZHLN::WriteFile(published / "index.json", lying));
        ExpectError(ZHLN::InstallFromRemote(fresh, "local", options));
        Require(ZHLN::WriteFile(published / "index.json", saved));
    }

    // --only selection reaches the remote, and brings the closure with it:
    // naming one package installs what it is built against, in dependency order.
    // Naming nothing is what installs the whole toolchain.
    {
        ZHLN::Env   freshEnv = TestEnv(directory / "fresh2");
        ZHLN::Store fresh(freshEnv);
        Require(fresh.RemoteAdd("local", url));
        ZHLN::InstallOptions only;
        only.only       = {"Signal"};
        auto reportOnly = Take(ZHLN::InstallFromRemote(fresh, "local", only));
        Expect(reportOnly.installed.size() == 2);
        Expect(reportOnly.installed[0] == "zahlen-engine"); // dependency first
        Expect(reportOnly.installed[1] == "zahlen-signal");
        reportOnly = Take(fresh.Finish(std::move(reportOnly)));
        Expect(reportOnly.missing.empty());
    }

    Require(store.RemoteRemove("local"));
    Expect(Take(store.Remotes()).empty());

    Discard(directory);
}

// The behaviour a package manager is judged on: what `install <remote>:<name>`
// puts on disk. Before this test existed, the name after the colon was parsed
// and thrown away, so every package-named install meant the whole toolchain.
void TestRemoteClosure() {
    const fs::path directory = MakeTemp("closure");
    const fs::path published = directory / "published";
    fs::create_directories(published);

    PackageSpec engine;
    PackageSpec signal;
    signal.component         = "Signal";
    signal.package           = "zahlen-signal";
    signal.dependencies      = {"zahlen-engine"};
    signal.provides          = {"Zahlen::Signal"};
    PackageSpec application;
    application.component    = "Application";
    application.package      = "zahlen-application";
    application.dependencies = {"zahlen-engine", "zahlen-signal"};
    application.provides     = {"Zahlen::Application"};
    WritePackage(published, engine);
    WritePackage(published, signal);
    WritePackage(published, application);

    const std::string url = ZHLN::ToFileUrl(published);
    const auto        index = Take(ZHLN::BuildIndex(published));
    Expect(index.size() == 1);
    Expect(ZHLN::ToolchainHasDependencies(index[0]));

    // Selection tokens: package name, component name, and provided target all
    // reach the same package.
    for (const auto& package: index[0].packages) {
        if (package.package != "zahlen-application")
            continue;
        Expect(ZHLN::PackageMatches(package, "zahlen-application"));
        Expect(ZHLN::PackageMatches(package, "Application"));
        Expect(ZHLN::PackageMatches(package, "application"));
        Expect(ZHLN::PackageMatches(package, "Zahlen::Application"));
        Expect(!ZHLN::PackageMatches(package, "zahlen-engine"));
        Expect(!ZHLN::PackageMatches(package, ""));
    }

    Require(ZHLN::WriteFile(published / "index.json", ZHLN::BuildIndexJson(index)));

    // A closure is resolved from the index alone.
    {
        const auto resolution = ZHLN::ResolveClosure(index[0], index[0].packages); // all three, as roots
        Expect(resolution.packages.size() == 3);
        Expect(resolution.missing.empty());
        Expect(resolution.packages.back().package == "zahlen-application"); // dependents last
    }

    const auto install = [&](const std::string& root, const std::vector<std::string>& only, bool ignoreMissing) {
        ZHLN::Env   env = TestEnv(directory / root);
        ZHLN::Store store(env);
        Require(store.RemoteAdd("local", url));
        ZHLN::InstallOptions options;
        options.only          = only;
        options.ignoreMissing = ignoreMissing;
        return ZHLN::InstallFromRemote(store, "local", options);
    };

    // One name, three packages, dependencies first.
    {
        ZHLN::Env   env = TestEnv(directory / "one");
        ZHLN::Store store(env);
        Require(store.RemoteAdd("local", url));
        ZHLN::InstallOptions options;
        options.only = {"zahlen-application"};
        auto report  = Take(ZHLN::InstallFromRemote(store, "local", options));
        Expect(report.installed.size() == 3);
        Expect(report.installed[0] == "zahlen-engine");
        Expect(report.installed[1] == "zahlen-signal");
        Expect(report.installed[2] == "zahlen-application");
        report = Take(store.Finish(std::move(report)));
        Expect(report.missing.empty());
    }

    // A name the toolchain does not publish is refused, and the refusal lists
    // what it does publish instead of installing everything.
    {
        const auto refused = install("unknown", {"zahlen-gltf"}, false);
        ExpectError(refused);
        if (!refused)
            Expect(refused.error().message.find("publishes: zahlen-application") != std::string::npos);
    }

    // A requirement nothing in the toolchain provides is a refusal before any
    // archive is fetched, and --ignore-missing is the only way past it.
    {
        PackageSpec ghost;
        ghost.component    = "Ghost";
        ghost.package      = "zahlen-ghost";
        ghost.dependencies = {"zahlen-nowhere"};
        WritePackage(published, ghost);
        const auto withGhost = Take(ZHLN::BuildIndex(published));
        Require(ZHLN::WriteFile(published / "index.json", ZHLN::BuildIndexJson(withGhost)));

        const auto refused = install("ghost", {"zahlen-ghost"}, false);
        ExpectError(refused);
        if (!refused)
            Expect(refused.error().message.find("zahlen-nowhere") != std::string::npos);

        ZHLN::Env   env = TestEnv(directory / "ghost-ignored");
        ZHLN::Store store(env);
        Require(store.RemoteAdd("local", url));
        ZHLN::InstallOptions options;
        options.only          = {"zahlen-ghost"};
        options.ignoreMissing = true;
        auto report           = Take(ZHLN::InstallFromRemote(store, "local", options));
        Expect(report.installed.size() == 1);
        // The store still refuses to call the result buildable.
        ExpectError(store.Finish(report, false));
        const auto tolerated = Take(store.Finish(report, true));
        Expect(tolerated.missing.size() == 1);
        Expect(tolerated.missing[0] == "zahlen-nowhere");

        Require(ZHLN::WriteFile(published / "index.json", ZHLN::BuildIndexJson(index)));
        fs::remove(published / ("zahlen-ghost-0.1.0-abc1234-gcc-16.2.1-x86_64-release.tar.gz"));
    }

    // A format-1 index names no graph. The closure is then discovered from the
    // archives' own manifests, which is what makes every already-published
    // release resolve correctly without republishing it.
    {
        ZHLN::Index legacy;
        legacy.format = 1;
        ZHLN::IndexToolchain one;
        one.id      = index[0].id;
        one.version = index[0].version;
        one.profile = index[0].profile;
        for (const auto& package: index[0].packages) {
            one.packages.push_back({package.package, package.component, package.file, package.sha256, package.size});
        }
        legacy.toolchains.push_back(one);
        Expect(!ZHLN::ToolchainHasDependencies(legacy.toolchains[0]));
        Require(ZHLN::WriteFile(published / "index.json", ZHLN::BuildIndexJson(legacy.toolchains)));

        ZHLN::Env   env = TestEnv(directory / "legacy");
        ZHLN::Store store(env);
        Require(store.RemoteAdd("local", url));
        ZHLN::InstallOptions options;
        options.only = {"zahlen-application"};
        auto report  = Take(ZHLN::InstallFromRemote(store, "local", options));
        Expect(report.installed.size() == 3);
        Expect(report.installed[0] == "zahlen-engine");
        report = Take(store.Finish(std::move(report)));
        Expect(report.missing.empty());

        Require(ZHLN::WriteFile(published / "index.json", ZHLN::BuildIndexJson(index)));
    }

    Discard(directory);
}

void TestEnvRules() {
    // The path rules are pure functions of the environment, so they can be
    // checked without being root: a system prefix is refused regardless.
    ZHLN::Env env;
    env.root = fs::path("/usr/local/zahlen");
    Expect(!ZHLN::IsWithin(fs::path("/usr"), env.root) == false);
    Expect(ZHLN::IsWithin(fs::path("/usr"), env.root));
    Expect(!ZHLN::IsWithin(fs::path("/usr/local"), fs::path("/usr/localx")));
    Expect(ZHLN::IsWithin(fs::path("/a/b"), fs::path("/a/b/c")));
    Expect(!ZHLN::IsWithin(fs::path("/a/b/c"), fs::path("/a/b")));
}

// Nothing can throw, so the wrapper only has to report whether the counter moved.
void Run(const char* name, void (*test)()) {
    const int before = failures;
    test();
    if (failures == before)
        std::printf("ok       %s\n", name);
}

} // namespace

int main() {
    ZHLN::log::SetQuiet(true);

    Run("sha256", TestSha256);
    Run("base64", TestBase64);
    Run("sums", TestSums);
    Run("keys-and-signatures", TestKeysAndSignatures);
    Run("json", TestJson);
    Run("index-roundtrip", TestIndexRoundtrip);
    Run("archive-scan-and-extract", TestArchiveScanAndExtract);
    Run("archive-refusals", TestArchiveRefusals);
    Run("archive-long-names", TestArchiveLongNames);
    Run("store-lock", TestStoreLock);
    Run("store-relocation", TestStoreRelocation);
    Run("store-lifecycle", TestStoreLifecycle);
    Run("checksum-and-signature-policy", TestChecksumAndSignaturePolicy);
    Run("remote", TestRemote);
    Run("remote-closure", TestRemoteClosure);
    Run("path-rules", TestEnvRules);

    std::printf("%s: %d checks, %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
