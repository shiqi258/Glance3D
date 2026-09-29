/**
 * @file G3DCommandLine.h
 * @brief Build libf3d command strings from values -- the inverse of f3d::utils::tokenize.
 *
 * A command is a plain string (that is what lets the same notification action or UI intent run on
 * the desktop and in the web viewer), so every value spliced into one has to survive the tokenizer
 * on the way back out. It does not by default: tokenize reads a backslash as an escape even inside
 * quotes, so a Windows path pasted in as-is loses every separator ("G:\data\a.glb" arrives as
 * "G:dataa.glb"), and a quote in a name ends the argument early.
 *
 * Quote every argument that did not come from someone typing the command. The one thing no quoting
 * can carry is an empty argument: tokenize drops empty tokens.
 */

#ifndef G3DCommandLine_h
#define G3DCommandLine_h

#include <initializer_list>
#include <string>
#include <string_view>

/**
 * @p arg in double quotes, with backslashes and double quotes escaped, so that tokenize gives back
 * exactly @p arg.
 */
std::string G3DQuoteCommandArgument(std::string_view arg);

/**
 * @p name followed by each of @p args quoted with G3DQuoteCommandArgument: a command line tokenize
 * splits back into { name, args... }.
 */
std::string G3DCommandLine(std::string_view name, std::initializer_list<std::string_view> args);

#endif
