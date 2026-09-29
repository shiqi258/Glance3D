#ifndef g3d_command_h
#define g3d_command_h

#include "export.h"

/// @cond
#include <initializer_list>
#include <string>
#include <string_view>
/// @endcond

namespace g3d
{
/**
 * @class   command
 * @brief   Build command strings for interactor::triggerCommand from values.
 *
 * f3d::utils::tokenize reads a backslash as an escape even inside quotes, so a value spliced into a
 * command as-is does not come back out intact: a Windows path loses every separator, a quote in a
 * name ends the argument early. Build any command that carries a value (a path, a name, a
 * notification action) through here; tokenize then returns exactly the values given.
 */
class F3D_EXPORT command
{
public:
  /// @p arg quoted so that tokenize gives it back unchanged. An empty argument cannot be carried:
  /// tokenize drops empty tokens.
  static std::string quote(std::string_view arg);

  /// @p name followed by each of @p args, quoted: a command tokenize splits into { name, args... }.
  static std::string line(std::string_view name, std::initializer_list<std::string_view> args);
};
}

#endif
