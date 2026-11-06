#pragma once

#include <string>
#include <vector>
#include <3ds.h>

class Session;

// Every console line (in-game AND PC Link) goes through this.
// (Named consolecmd.h so it never collides with the tree's existing
// include/console.h, which declares ConsoleProfile / g_profile.)
void runConsoleCommand(Session* session, const char* raw);

struct ParsedConsole
{
    std::string prefix;
    std::string cmd;
    std::vector<std::string> args;
    std::string raw;
};
bool parseConsoleLine(const std::string& in, ParsedConsole& out);
