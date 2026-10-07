#pragma once

// Machine-readable commands for the WPF frontend, before GPU/guest startup.
// false means this is a normal game invocation; true supplies its exit code.
bool launcher_bridge_command(int argc, char** argv, int* result);
