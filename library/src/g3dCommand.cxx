#include "g3dCommand.h"

#include "G3DCommandLine.h"

namespace g3d
{
//----------------------------------------------------------------------------
std::string command::quote(std::string_view arg)
{
  return G3DQuoteCommandArgument(arg);
}

//----------------------------------------------------------------------------
std::string command::line(std::string_view name, std::initializer_list<std::string_view> args)
{
  return G3DCommandLine(name, args);
}
}
