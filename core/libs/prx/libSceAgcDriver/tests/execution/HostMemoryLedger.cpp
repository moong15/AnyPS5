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
    const char* fields[]{"; vq private ", " (guest ", ", host ", ") mapped ", " image ", "; vk dev ", "; pipelines ", " cache ", "; spirv ", "; heap ", " mapped ", ") vectors ", "; cache loaded ", " saved ", "; vk import host ", " dev ", " unique ", " guest ", "; vk alloc host "};
    for (const auto* field : fields) if (std::strstr(line, field) == nullptr) return 7;
    double walkMiB = 0;
    if (std::sscanf(std::strstr(line, "; vq private "), "; vq private %lf", &walkMiB) != 1) return 8;
#ifdef _WIN32
    if (walkMiB <= 0) return 9;
#endif
    return 0;
}
