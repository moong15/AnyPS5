#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <cstdio>
#include <cstring>
#include <memory>

int main() {
    const auto close = [](std::FILE* file) { std::fclose(file); };
    std::unique_ptr<std::FILE, decltype(close)> output(std::tmpfile(), close);
    if (!output) return 1;
    AgcDriverReportHostMemory_nid_no_patch(output.get());
    std::rewind(output.get());
    char line[4096]{};
    if (!std::fgets(line, sizeof(line), output.get())) return 2;
    if (std::strncmp(line, "[hostmem] private", 17) != 0) return 3;
    double privateMiB = 0;
    if (std::sscanf(line, "[hostmem] private %lf MiB:", &privateMiB) != 1) return 4;
#ifdef _WIN32
    if (privateMiB <= 0) return 5;
#endif
    if (std::strchr(line, '\n') == nullptr || std::fgetc(output.get()) != EOF) return 6;
    return 0;
}
