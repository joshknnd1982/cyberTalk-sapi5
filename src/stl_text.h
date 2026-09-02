// Text conversion for the CyberTalk engine (7-bit ASCII with \Tag\ codes).
#pragma once
#include <string>
#include <vector>

namespace stl {

// Convert text into the 7-bit ASCII the engine understands: accented Latin
// letters lose their accents, typographic punctuation is replaced, other
// characters become spaces.  Backslashes cannot be spoken by the engine (they
// introduce control tags) and become spaces unless keep_backslash is set.
// If offsets is given it receives, for every output byte, the index of the
// source character it came from (used to map the engine's word positions
// back to the caller's text).
std::string wide_to_engine_text(const wchar_t* text, size_t len, bool keep_backslash = false,
                                std::vector<unsigned>* offsets = nullptr);
std::string utf8_to_engine_text(const std::string& utf8, bool keep_backslash = false);

}  // namespace stl
