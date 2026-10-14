#include "ShaderCachePack.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ThreadOwned.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#endif

namespace ShaderRecompiler::ShaderDiskCache {
namespace {
bool disabledByCommandLine() {
#ifdef _WIN32
    int count = 0;
    const std::unique_ptr<LPWSTR, void (*)(LPWSTR*)> arguments(CommandLineToArgvW(GetCommandLineW(), &count), [](LPWSTR* values) { LocalFree(values); });
    if (arguments) {
        for (int i = 1; i < count; ++i) if (std::wstring_view(arguments.get()[i]) == L"--no-prebuilt-shaders") return true;
    }
#elif defined(__APPLE__)
    const auto count = *_NSGetArgc();
    const auto arguments = *_NSGetArgv();
    for (int i = 1; i < count; ++i) if (std::string_view(arguments[i]) == "--no-prebuilt-shaders") return true;
#else
    std::ifstream stream("/proc/self/cmdline", std::ios::binary);
    std::string argument;
    std::getline(stream, argument, '\0');
    while (std::getline(stream, argument, '\0')) if (argument == "--no-prebuilt-shaders") return true;
#endif
    return false;
}
using Clock = std::chrono::steady_clock;
std::uint64_t elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
}

std::array<std::uint64_t, 2> PackKeyHash(std::span<const std::byte> key) {
    return {HashBytes(key, 0x5eed0001ull), HashBytes(key, 0x5eed0002ull)};
}

std::vector<std::filesystem::path> PrebuiltCachePaths(const std::filesystem::path& cacheRoot) {
    const char* disabled = std::getenv("ANYPS5_NO_PREBUILT_SHADERS");
    if ((disabled && *disabled && std::strcmp(disabled, "0") != 0) || disabledByCommandLine()) return {};
    std::vector<std::filesystem::path> paths;
    if (!cacheRoot.empty()) paths.push_back(cacheRoot / "prebuilt");
#ifdef _WIN32
    const auto* value = _wgetenv(L"ANYPS5_PREBUILT_SHADER_CACHE");
    const std::wstring_view text = value ? value : L"";
    constexpr wchar_t separator = L';';
#else
    const auto* value = std::getenv("ANYPS5_PREBUILT_SHADER_CACHE");
    const std::string_view text = value ? value : "";
    constexpr char separator = ':';
#endif
    for (std::size_t at = 0; at < text.size();) {
        const auto next = text.find(separator, at);
        const auto part = text.substr(at, next == text.npos ? text.size() - at : next - at);
        if (!part.empty()) {
            const std::filesystem::path path(part);
            if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
        }
        if (next == text.npos) break;
        at = next + 1;
    }
    return paths;
}

CachePack::CachePack(const std::filesystem::path& path) {
    static_assert(std::endian::native == std::endian::little);
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { error = "cannot open"; return; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(PackHeader)) || static_cast<std::uint64_t>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
        CloseHandle(handle); error = "invalid size"; return;
    }
    mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    fileHandle = handle;
    const auto* mapped = std::getenv("ANYPS5_PREBUILT_MAPPED_READ");
    bufferedRead = !(mapped && *mapped && std::strcmp(mapped, "0") != 0);
    if (!mapping) { error = "cannot map"; return; }
    const auto* data = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data) { error = "cannot map view"; return; }
    file = {data, static_cast<std::size_t>(size.QuadPart)};
#else
    const auto fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { error = "cannot open"; return; }
    struct stat size{};
    if (fstat(fd, &size) != 0 || size.st_size < static_cast<off_t>(sizeof(PackHeader)) || static_cast<std::uint64_t>(size.st_size) > std::numeric_limits<std::size_t>::max()) {
        close(fd); error = "invalid size"; return;
    }
    auto* data = mmap(nullptr, size.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { error = "cannot map"; return; }
    file = {static_cast<const std::byte*>(data), static_cast<std::size_t>(size.st_size)};
#endif
#ifdef _WIN32
    mappingOwner = std::shared_ptr<const void>(file.data(), [](const void* data) { UnmapViewOfFile(data); });
#else
    mappingOwner = std::shared_ptr<const void>(file.data(), [size = file.size()](const void* data) { munmap(const_cast<void*>(data), size); });
#endif
    Validate();
}

CachePack::~CachePack() {
#ifdef _WIN32
    if (mapping) CloseHandle(mapping);
    if (fileHandle) CloseHandle(fileHandle);
#endif
}

void CachePack::Validate() {
    PackHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.magic != PackMagic) { error = "unknown pack format"; return; }
    if (header.sourceVersion != SourceVersion()) { error = "different compiler source version"; return; }
    if (header.fileBytes != file.size() || header.entryCount > (file.size() - sizeof(header)) / sizeof(PackRecord)) { error = "truncated index"; return; }
    const auto indexBytes = static_cast<std::size_t>(header.entryCount) * sizeof(PackRecord);
    const auto index = file.subspan(sizeof(header), indexBytes);
    if (HashBytes(index) != header.indexHash) { error = "index checksum mismatch"; return; }
    const std::span<const PackRecord> checked(reinterpret_cast<const PackRecord*>(index.data()), header.entryCount);
    std::array<std::uint64_t, 2> previous{};
    for (const auto& record : checked) {
        if (record.hash < previous || record.offset < sizeof(header) + indexBytes || record.offset > file.size() || record.size > file.size() - record.offset || record.size < 48) { error = "invalid index record"; return; }
        previous = record.hash;
    }
    records = checked;
}

LoadStatus CachePack::Load(std::span<const std::byte> key, CompiledVariant& variant, PackLoadTimings& timings) const {
    return LoadEncoded(key, [&](std::span<const std::byte> bytes, std::shared_ptr<const void> owner) { return DecodeEntry(bytes, key, variant, std::move(owner)); }, timings);
}

LoadStatus CachePack::LoadEncoded(std::span<const std::byte> key, const Decoder& decodeEntry, PackLoadTimings& timings) const {
    if (!Valid()) return LoadStatus::Rejected;
    const auto start = Clock::now();
    const auto hash = PackKeyHash(key);
    const auto first = std::lower_bound(records.begin(), records.end(), hash, [](const auto& record, const auto& value) { return record.hash < value; });
    timings.lookupNanoseconds += elapsed(start);
    auto status = LoadStatus::Absent;
    for (auto it = first; it != records.end() && it->hash == hash; ++it) {
        auto data = file.subspan(it->offset, it->size);
        std::shared_ptr<const void> owner = mappingOwner;
#ifdef _WIN32
        if (bufferedRead) {
            const auto read = Clock::now();
            struct ReadBuffer {
                HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                ~ReadBuffer() { if (event) CloseHandle(event); }
            };
            thread_local ReadBuffer* slot = nullptr;
            auto& buffer = ThreadOwned(slot);
            if (!buffer.event || it->size > std::numeric_limits<DWORD>::max()) return LoadStatus::Rejected;
            auto bytes = std::make_shared<std::vector<std::byte>>(it->size);
            OVERLAPPED operation{};
            operation.Offset = static_cast<DWORD>(it->offset);
            operation.OffsetHigh = static_cast<DWORD>(it->offset >> 32u);
            operation.hEvent = buffer.event;
            ResetEvent(buffer.event);
            DWORD received = 0;
            const auto complete = ReadFile(fileHandle, bytes->data(), static_cast<DWORD>(bytes->size()), &received, &operation);
            const bool pending = !complete && GetLastError() == ERROR_IO_PENDING;
            const bool ready = complete || (pending && GetOverlappedResult(fileHandle, &operation, &received, TRUE));
            timings.readNanoseconds += elapsed(read);
            if (!ready || received != it->size) return LoadStatus::Rejected;
            data = *bytes;
            owner = std::move(bytes);
        }
#endif
        const auto decode = Clock::now();
        const auto result = decodeEntry(data, std::move(owner));
        timings.decodeNanoseconds += elapsed(decode);
        timings.bytes += data.size();
        if (result == LoadStatus::Loaded) return result;
        if (result == LoadStatus::Rejected) status = result;
    }
    return status;
}
}
