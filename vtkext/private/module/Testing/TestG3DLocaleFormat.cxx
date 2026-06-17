#include "G3DLocaleCore.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using Args = G3DLocaleCore::Args;

int failures = 0;

void Expect(const std::string& got, const std::string& want, const std::string& label)
{
  if (got != want)
  {
    std::cerr << "FAIL [" << label << "]: got \"" << got << "\" want \"" << want << "\"\n";
    ++failures;
  }
}

std::string Fmt(const std::string& tmpl, const Args& args, const std::string& lang)
{
  return G3DLocaleCore::FormatMessage(tmpl, args, lang);
}
}

int TestG3DLocaleFormat(int, char*[])
{
  // --- Plain argument substitution (back-compat with the old FormatNamed) ---
  Expect(Fmt("Hello {name}", { { "name", "World" } }, "en"), "Hello World", "arg.basic");
  Expect(Fmt("a {x} b {y}", { { "x", "1" }, { "y", "2" } }, "en"), "a 1 b 2", "arg.multi");
  // Unknown placeholder is preserved verbatim.
  Expect(Fmt("Hi {x}", { { "y", "1" } }, "en"), "Hi {x}", "arg.unknown");
  // Empty args: template returned unchanged.
  Expect(Fmt("Number of points: {n, number}", {}, "en"), "Number of points: {n, number}",
    "arg.empty");

  // --- number: thousands grouping ---
  Expect(Fmt("{n, number}", { { "n", "1234567" } }, "en"), "1,234,567", "number.en");
  Expect(Fmt("{n, number}", { { "n", "1234567" } }, "zh-CN"), "1,234,567", "number.zh");
  Expect(Fmt("{n, number}", { { "n", "42" } }, "en"), "42", "number.small");
  Expect(Fmt("{n, number}", { { "n", "1000" } }, "en"), "1,000", "number.boundary");
  Expect(Fmt("{n, number}", { { "n", "-12345" } }, "en"), "-12,345", "number.negative");
  // Non-integer values fall back to the raw string.
  Expect(Fmt("{n, number}", { { "n", "abc" } }, "en"), "abc", "number.nonint");

  // --- plural: English (one/other) ---
  Expect(Fmt("{n, plural, one{# item} other{# items}}", { { "n", "1" } }, "en"), "1 item",
    "plural.en.one");
  Expect(Fmt("{n, plural, one{# item} other{# items}}", { { "n", "5" } }, "en"), "5 items",
    "plural.en.other");
  // '#' is the grouped number.
  Expect(Fmt("{n, plural, one{# item} other{# items}}", { { "n", "1000" } }, "en"), "1,000 items",
    "plural.en.hashGrouped");
  // Exact "=N" wins over the category.
  Expect(Fmt("{n, plural, =0{no items} one{# item} other{# items}}", { { "n", "0" } }, "en"),
    "no items", "plural.exact");

  // --- plural: French (0 and 1 are "one") ---
  Expect(Fmt("{n, plural, one{# fichier} other{# fichiers}}", { { "n", "0" } }, "fr"), "0 fichier",
    "plural.fr.zero");
  Expect(Fmt("{n, plural, one{# fichier} other{# fichiers}}", { { "n", "1" } }, "fr"), "1 fichier",
    "plural.fr.one");
  Expect(Fmt("{n, plural, one{# fichier} other{# fichiers}}", { { "n", "2" } }, "fr"), "2 fichiers",
    "plural.fr.other");

  // --- plural: Chinese (always "other") ---
  Expect(Fmt("{n, plural, one{# 个} other{#个}}", { { "n", "1" } }, "zh-CN"), "1个", "plural.zh");

  // --- nested named arg inside a plural sub-message ---
  Expect(Fmt("{n, plural, one{# {thing}} other{# {thing}s}}",
           { { "n", "2" }, { "thing", "cat" } }, "en"),
    "2 cats", "plural.nested");

  // --- select ---
  Expect(Fmt("{g, select, male{he} female{she} other{they}}", { { "g", "female" } }, "en"), "she",
    "select.match");
  Expect(Fmt("{g, select, male{he} female{she} other{they}}", { { "g", "x" } }, "en"), "they",
    "select.fallback");

  // --- PluralCategory directly ---
  Expect(G3DLocaleCore::PluralCategory("en", 1), "one", "cat.en.1");
  Expect(G3DLocaleCore::PluralCategory("en", 0), "other", "cat.en.0");
  Expect(G3DLocaleCore::PluralCategory("en", 2), "other", "cat.en.2");
  Expect(G3DLocaleCore::PluralCategory("de", 1), "one", "cat.de.1");
  Expect(G3DLocaleCore::PluralCategory("fr", 0), "one", "cat.fr.0");
  Expect(G3DLocaleCore::PluralCategory("fr", 1), "one", "cat.fr.1");
  Expect(G3DLocaleCore::PluralCategory("fr", 2), "other", "cat.fr.2");
  Expect(G3DLocaleCore::PluralCategory("zh-CN", 1), "other", "cat.zh.1");
  Expect(G3DLocaleCore::PluralCategory("ja", 1), "other", "cat.ja.1");

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
