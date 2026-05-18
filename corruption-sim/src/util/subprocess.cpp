/*
 * subprocess.cpp - Run shell commands and capture output.
 */
#include "corrsim.h"
#include <cstdio>
#include <array>

SubprocessResult run_cmd(const std::string &cmd, int /*verbosity*/)
{
    SubprocessResult r;
    // Redirect stderr to stdout so we capture both
    std::string full = cmd + " 2>&1";

    FILE *pipe = popen(full.c_str(), "r");
    if (!pipe) {
        r.exit_code = -1;
        r.err = "popen failed";
        return r;
    }

    std::array<char, 4096> buf;
    while (fgets(buf.data(), buf.size(), pipe))
        r.out += buf.data();

    int status = pclose(pipe);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return r;
}
