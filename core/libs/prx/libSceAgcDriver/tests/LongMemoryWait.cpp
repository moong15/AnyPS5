#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        file = std::tmpfile();
        Require(file != nullptr, "cannot open stderr capture");
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        Require(saved >= 0 && _dup2(_fileno(file), _fileno(stderr)) == 0, "cannot redirect stderr");
#else
        saved = dup(fileno(stderr));
        Require(saved >= 0 && dup2(fileno(file), fileno(stderr)) >= 0, "cannot redirect stderr");
#endif
    }
    ~StderrCapture() {
        Restore();
        if (file != nullptr) std::fclose(file);
    }
    std::string Finish() {
        Restore();
        std::rewind(file);
        std::string text;
        char buffer[4096];
        for (std::size_t count = 0; (count = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) text.append(buffer, count);
        std::fwrite(text.data(), 1, text.size(), stderr);
        return text;
    }

private:
    void Restore() {
        if (saved < 0) return;
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        saved = -1;
    }
    std::FILE* file = nullptr;
    int saved = -1;
};

std::array<std::uint32_t, 5> WriteData(volatile std::uint32_t* target) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    return {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u};
}

void CheckWait(bool wide, bool enabled) {
    struct alignas(64) State {
        volatile std::uint64_t label = 0;
        volatile std::uint32_t entered = 0;
        volatile std::uint32_t after = 0;
    };
    static State state;
    const auto address = reinterpret_cast<std::uintptr_t>(&state.label);
    const auto entered = WriteData(&state.entered);
    const auto after = WriteData(&state.after);
    std::vector<std::uint32_t> words(entered.begin(), entered.end());
    if (wide) words.insert(words.end(), {0xc0079300u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u, 1u, 0xffffffffu, 0xffffffffu, 0x19u});
    else words.insert(words.end(), {0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1u, 0xffffffffu, 0x19u});
    words.insert(words.end(), after.begin(), after.end());
    StderrCapture capture;
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0x20);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state.entered == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool enteredWait = state.entered == 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const bool blocked = state.after == 0;
    state.label = wide ? 0x100000001ull : 1ull;
    AgcDriverWaitIdle_nid_postfix();
    AgcDriverShutdown_nid_postfix();
    const auto text = capture.Finish();
    Require(enteredWait && blocked && state.after == 1, "delayed memory wait did not block and resume");
    if (!enabled) {
        Require(text.find("[long-wait]") == std::string::npos, "disabled long-wait logging printed a line");
        return;
    }
    char identity[192];
    std::snprintf(identity, sizeof(identity), "cursor 5 opcode 0x%x function eq address 0x%llx bytes %u reference 0x%llx mask 0x%llx first 0x0", wide ? 0x93u : 0x3cu, static_cast<unsigned long long>(address), wide ? 8u : 4u, wide ? 0x100000001ull : 1ull, wide ? 0xffffffffffffffffull : 0xffffffffull);
    std::size_t lines = 0, ongoing = 0, completed = 0;
    for (std::size_t pos = 0; (pos = text.find("[long-wait] queue 0x20 serial ", pos)) != std::string::npos; ++pos) {
        const auto end = text.find('\n', pos);
        const auto line = text.substr(pos, end - pos);
        Require(line.find(identity) != std::string::npos, "long-wait line lost packet identity or wide operands");
        Require(line.find("serial 0 ") == std::string::npos, "long-wait line lost submission serial");
        ++lines;
        if (line.find("still waiting") != std::string::npos) ++ongoing;
        if (line.find("resolved by memory ") != std::string::npos) ++completed;
    }
    Require(lines == 2 && ongoing == 1 && completed == 1, "expected one ongoing and one completed long-wait line");
}

}

int main(int argc, char** argv) {
    try {
        CheckWait(argc > 1 && std::strcmp(argv[1], "--wide") == 0, !(argc > 1 && std::strcmp(argv[1], "--disabled") == 0));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
