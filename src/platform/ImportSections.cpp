#include "Platform/ImportSections.hpp"

#include <regex>

namespace Luau::LanguageServer::AutoImports
{

static std::optional<std::regex> compilePattern(const std::string& pattern)
{
    if (pattern.empty())
        return std::nullopt;
    try
    {
        return std::regex(pattern, std::regex::ECMAScript);
    }
    catch (const std::regex_error&)
    {
        return std::nullopt;
    }
}

ImportSections detectImportSections(const TextDocument& textDocument, const ClientCompletionImportsSectionsConfiguration& config)
{
    ImportSections sections{};

    auto servicesPattern = compilePattern(config.services);
    auto modulesPattern = compilePattern(config.modules);
    if (!servicesPattern && !modulesPattern)
        return sections;

    auto lineCount = textDocument.lineCount();
    std::optional<size_t> servicesHeading = std::nullopt;
    std::optional<size_t> modulesHeading = std::nullopt;
    for (size_t line = 0; line < lineCount; line++)
    {
        auto text = textDocument.getLine(line);
        if (servicesPattern && !servicesHeading && std::regex_search(text, *servicesPattern))
            servicesHeading = line;
        else if (modulesPattern && !modulesHeading && std::regex_search(text, *modulesPattern))
            modulesHeading = line;
        if ((!servicesPattern || servicesHeading) && (!modulesPattern || modulesHeading))
            break;
    }

    if (servicesHeading)
    {
        std::optional<size_t> endLine = std::nullopt;
        if (modulesHeading && *modulesHeading > *servicesHeading)
            endLine = *modulesHeading;
        sections.services = ImportSectionRange{*servicesHeading, *servicesHeading + 1, endLine};
    }
    if (modulesHeading)
    {
        std::optional<size_t> endLine = std::nullopt;
        if (servicesHeading && *servicesHeading > *modulesHeading)
            endLine = *servicesHeading;
        sections.modules = ImportSectionRange{*modulesHeading, *modulesHeading + 1, endLine};
    }
    return sections;
}

std::vector<std::string> validateImportSections(const ClientCompletionImportsSectionsConfiguration& config)
{
    std::vector<std::string> warnings{};
    if (!config.services.empty() && !compilePattern(config.services))
        warnings.push_back("completion.imports.sections.services: invalid regular expression '" + config.services + "', section ignored");
    if (!config.modules.empty() && !compilePattern(config.modules))
        warnings.push_back("completion.imports.sections.modules: invalid regular expression '" + config.modules + "', section ignored");
    return warnings;
}

} // namespace Luau::LanguageServer::AutoImports
