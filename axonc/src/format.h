// format.h - `axon fmt`: re-indents and normalizes spacing, keeping line breaks and comments.
#pragma once
#include <string>

// Formats the text of a registered source file. Returns false (and leaves `out` empty) if the
// file has syntax errors or formatting would change its tokens.
bool formatSource(int fileId, std::string& out, std::string& error);
