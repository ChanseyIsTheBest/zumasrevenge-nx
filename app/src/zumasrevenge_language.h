// The game's language from the system's, for zumasrevenge_language = "" (the user's request, 2026-10-21: "make the default
// option follow the users system language"): the Switch's system language, or Windows' display language on the PC.

#pragma once

#include <string>

namespace zumasrevenge::language {

// The system language as one of the game's six (en, de, fr, es, it, pt): regional variants give their language
// (Brazilian and European Portuguese both pt), and any other language (Japanese, Chinese, Korean, Dutch, Russian) or a
// system language that cannot be read gives en. `system_name` receives the system's own name for it, for the log.
std::string FromSystem(std::string* system_name = nullptr);

}  // namespace zumasrevenge::language
