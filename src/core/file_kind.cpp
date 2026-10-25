#include "core/file_kind.hpp"
#include "core/i18n.hpp"

#include <unordered_map>

namespace prism::core {

namespace {

struct Group {
    FileKind                              kind;
    std::initializer_list<std::string_view> exts;
};

const std::unordered_map<std::string_view, FileKind>& table()
{
    static const std::unordered_map<std::string_view, FileKind> t = [] {
        std::unordered_map<std::string_view, FileKind> m;
        const Group groups[] = {
            {FileKind::image, {"png", "jpg", "jpeg", "jfif", "gif", "bmp", "webp", "tif", "tiff", "ico", "svg", "heic", "heif", "avif", "psd", "raw", "cr2", "nef", "dng", "exr", "hdr", "tga", "dds"}},
            {FileKind::video, {"mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "ts", "3gp", "vob"}},
            {FileKind::audio, {"mp3", "wav", "flac", "ogg", "m4a", "aac", "wma", "opus", "aiff", "mid", "midi"}},
            {FileKind::archive, {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "zst", "cab", "tgz", "lz4"}},
            {FileKind::document, {"doc", "docx", "odt", "rtf", "dot", "dotx", "pages"}},
            {FileKind::spreadsheet, {"xls", "xlsx", "xlsm", "ods", "csv", "tsv", "numbers"}},
            {FileKind::presentation, {"ppt", "pptx", "odp", "key"}},
            {FileKind::pdf, {"pdf", "xps", "epub"}},
            {FileKind::code, {"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ixx", "cs", "java", "kt", "swift", "go", "rs", "py", "pyw", "js", "mjs", "ts", "jsx", "tsx", "html", "htm", "css", "scss", "php", "rb", "lua", "sh", "bat", "cmd", "ps1", "psm1", "sql", "asm", "s", "hlsl", "glsl", "vert", "frag", "cmake", "vue", "dart", "r", "pl", "zig", "nim"}},
            {FileKind::text, {"txt", "md", "markdown", "log", "nfo", "rst", "tex"}},
            {FileKind::executable, {"exe", "msi", "com", "scr", "appx", "msix", "apk"}},
            {FileKind::library, {"dll", "sys", "lib", "obj", "o", "a", "so", "dylib", "pdb", "ocx", "drv"}},
            {FileKind::config, {"json", "xml", "yaml", "yml", "toml", "ini", "cfg", "conf", "reg", "inf", "props", "targets", "sln", "vcxproj", "csproj", "editorconfig", "gitignore", "gitattributes", "manifest", "plist", "env"}},
            {FileKind::font, {"ttf", "otf", "woff", "woff2", "ttc", "fon"}},
            {FileKind::disk_image, {"iso", "img", "vhd", "vhdx", "vmdk", "wim", "dmg", "bin"}},
            {FileKind::shortcut, {"lnk", "url"}},
        };
        // (first group wins: "ts" is video here, as explorer's association would say)
        for (const Group& g : groups) {
            for (const std::string_view e : g.exts) { m.emplace(e, g.kind); }
        }
        return m;
    }();
    return t;
}

} // namespace

bool is_program_name(std::string_view name) noexcept
{
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) { return false; }
    const std::string_view ext = name.substr(dot + 1);
    if (ext.size() > 3) { return false; }
    char lower[4]{};
    for (std::size_t i = 0; i < ext.size(); ++i) { lower[i] = static_cast<char>(ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + ('a' - 'A') : ext[i]); }
    const std::string_view e{lower, ext.size()};
    return e == "exe" || e == "com" || e == "bat" || e == "cmd" || e == "scr" || e == "lnk";
}

FileKind classify_extension(std::string_view ext) noexcept
{
    if (ext.empty()) { return FileKind::other; }
    const auto& t  = table();
    const auto  it = t.find(ext);
    return it == t.end() ? FileKind::other : it->second;
}

std::string type_label(FileKind kind, std::string_view ext)
{
    switch (kind) {
    case FileKind::folder:       return std::string{i18n::tr("File folder")};
    case FileKind::drive:        return std::string{i18n::tr("Local disk")};
    case FileKind::shortcut:     return std::string{i18n::tr("Shortcut")};
    case FileKind::executable:   return ext == "msi" ? std::string{i18n::tr("Windows Installer")} : std::string{i18n::tr("Application")};
    case FileKind::library:      return ext == "dll" ? std::string{i18n::tr("Application extension")} : std::string{i18n::tr("System file")};
    case FileKind::text:         return ext == "txt" ? std::string{i18n::tr("Text document")} : (ext == "md" || ext == "markdown") ? std::string{i18n::tr("Markdown")} : std::string{i18n::tr("Text file")};
    case FileKind::pdf:          return ext == "pdf" ? std::string{i18n::tr("PDF document")} : std::string{i18n::tr("Document")};
    case FileKind::document:     return std::string{i18n::tr("Word document")};
    case FileKind::spreadsheet:  return ext == "csv" ? std::string{i18n::tr("CSV file")} : std::string{i18n::tr("Spreadsheet")};
    case FileKind::presentation: return std::string{i18n::tr("Presentation")};
    case FileKind::archive:      return std::string{i18n::tr("Compressed archive")};
    case FileKind::disk_image:   return std::string{i18n::tr("Disk image")};
    default: break;
    }
    if (ext.empty()) { return std::string{i18n::tr("File")}; }
    std::string label;
    for (const char c : ext) { label.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c); }
    label = i18n::trf("{} file", label);
    return label;
}

} // namespace prism::core
