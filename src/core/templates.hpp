#pragma once

// the "New" menu's documents: a few built in, plus every file you put in the Templates folder beside settings.ini
// (a Word document, a license header, a project skeleton file ...)

#include <string>
#include <vector>

namespace prism::core {

struct Template {
    std::string label;    // "Markdown document"
    std::string stem;     // "New Markdown document"
    std::string ext;      // ".md"
    std::string content;  // built-in text
    std::string source;   // user template: the file to copy (content is empty then)
};

[[nodiscard]] const std::vector<Template>& builtin_templates();
// the files of `folder` (hidden ones skipped), sorted by name; label is the name without its extension
[[nodiscard]] std::vector<Template> user_templates(const std::string& folder);

// makes "<stem><ext>", "<stem> (2)<ext>" ... in `dir` with the template's content; returns the name, empty on failure
[[nodiscard]] std::string create_from_template(const std::string& dir, const Template& t);

} // namespace prism::core
