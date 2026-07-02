#pragma once

#include <optional>
#include <string>
#include <vector>

#include "LSP/ClientConfiguration.hpp"
#include "LSP/TextDocument.hpp"

namespace Luau::LanguageServer::AutoImports
{

/// A configured comment section within a file that generated imports are inserted into
struct ImportSectionRange
{
    /// The line of the section heading
    size_t headingLine = 0;
    /// The first line that may be inserted into (headingLine + 1)
    size_t firstLine = 0;
    /// Exclusive upper bound for insertions: the next configured heading below, if any
    std::optional<size_t> endLine = std::nullopt;

    /// Clamps a computed insertion line into this section
    [[nodiscard]] size_t clamp(size_t lineNumber) const
    {
        if (lineNumber < firstLine)
            lineNumber = firstLine;
        if (endLine && lineNumber > *endLine)
            lineNumber = *endLine;
        return lineNumber;
    }
};

/// Detected sections for the configured `completion.imports.sections` patterns
struct ImportSections
{
    std::optional<ImportSectionRange> services = std::nullopt;
    std::optional<ImportSectionRange> modules = std::nullopt;
};

/// Detects configured import sections in a document. Patterns are ECMAScript regexes matched
/// against each line; the first matching line wins, and a line claimed by the services pattern
/// is not considered for the modules pattern. Invalid or non-matching patterns leave the
/// section absent, falling back to the default insertion behaviour
ImportSections detectImportSections(const TextDocument& textDocument, const ClientCompletionImportsSectionsConfiguration& config);

/// Validates the section configuration, returning warnings for invalid patterns
std::vector<std::string> validateImportSections(const ClientCompletionImportsSectionsConfiguration& config);

/// Raises a minimum insertion line to the start of the given section, if present
inline size_t sectionMinimum(const std::optional<ImportSectionRange>& section, size_t minimumLineNumber)
{
    if (section && section->firstLine > minimumLineNumber)
        return section->firstLine;
    return minimumLineNumber;
}

/// Clamps a computed insertion line into the given section, if present
inline size_t sectionClamp(const std::optional<ImportSectionRange>& section, size_t lineNumber)
{
    return section ? section->clamp(lineNumber) : lineNumber;
}

} // namespace Luau::LanguageServer::AutoImports
